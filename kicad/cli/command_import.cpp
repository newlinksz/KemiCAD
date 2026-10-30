/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "command_import.h"
#include <cli/exit_codes.h>
#include <jobs/job_pcb_import.h>
#include <jobs/job_sch_import.h>
#include <jobs/job_sch_link_footprints.h>
#include <jobs/job_import_utils.h>
#include <kemi_format_profile.h>
#include <kemi_project_import.h>
#include <ki_exception.h>
#include <pgm_base.h>
#include <settings/settings_manager.h>
#include <project.h>
#include <project/project_file.h>
#include <project/project_local_settings.h>
#include <reporter.h>
#include <kiid.h>
#include <string_utils.h>
#include <macros.h>
#include <wildcards_and_files_ext.h>
#include <wx/crt.h>
#include <wx/filename.h>

#include <string>
#include <set>
#include <vector>


#define ARG_LAYER_MAP "--layer-map"


CLI::IMPORT_COMMAND::IMPORT_COMMAND() : COMMAND( "import" )
{
    m_argParser.add_description( UTF8STDSTR(
            _( "Import board and/or schematic files from other EDA tools into a new KemiCAD project" ) ) );

    m_argParser.add_argument( ARG_INPUT )
            .help( UTF8STDSTR( _( "Input file(s) to import; each is autodetected as a board or "
                                  "schematic" ) ) )
            .nargs( argparse::nargs_pattern::at_least_one )
            .metavar( "INPUT_FILES" );

    m_argParser.add_argument( "-o", ARG_OUTPUT )
            .default_value( std::string() )
            .help( UTF8STDSTR( _( "Output project path stem; produces <stem>.kemi_pro plus "
                                  "imported board and/or schematic files beside it. Defaults to the first "
                                  "input's name in the current directory." ) ) )
            .metavar( "PROJECT" );

    m_argParser.add_argument( ARG_LAYER_MAP )
            .default_value( std::string( "" ) )
            .help( UTF8STDSTR( _( "JSON file mapping source layer names to KiCad layer names for "
                                  "imported boards; unmapped layers use the automatic best-guess" ) ) )
            .metavar( "FILE" );
}


int CLI::IMPORT_COMMAND::doPerform( KIWAY& aKiway )
{
    std::vector<std::string> rawInputs = m_argParser.get<std::vector<std::string>>( ARG_INPUT );
    wxString outputArg = From_UTF8( m_argParser.get<std::string>( ARG_OUTPUT ).c_str() );

    if( rawInputs.empty() )
    {
        wxFprintf( stderr, _( "At least one input file is required\n" ) );
        return EXIT_CODES::ERR_ARGS;
    }

    std::vector<wxString> inputFiles;

    for( const std::string& raw : rawInputs )
        inputFiles.push_back( From_UTF8( raw.c_str() ) );

    std::map<wxString, wxString> layerMap;
    wxString layerMapFile = From_UTF8( m_argParser.get<std::string>( ARG_LAYER_MAP ).c_str() );

    if( !layerMapFile.IsEmpty() )
    {
        wxString error;

        if( !LoadLayerMapFile( layerMapFile, layerMap, error ) )
        {
            wxFprintf( stderr, wxS( "%s\n" ), error );
            return EXIT_CODES::ERR_ARGS;
        }
    }

    KEMI_FORMAT::PROJECT_IMPORT_TARGETS targets;

    try
    {
        targets = KEMI_FORMAT::ResolveProjectImportTargets(
                outputArg, inputFiles.front(), wxFileName::GetCwd() );
    }
    catch( const IO_ERROR& error )
    {
        wxFprintf( stderr, wxS( "%s\n" ), error.What() );
        return EXIT_CODES::ERR_ARGS;
    }

    try
    {
        KEMI_FORMAT::ValidateProjectImportTargets( targets );
    }
    catch( const IO_ERROR& error )
    {
        wxFprintf( stderr, wxS( "%s\n" ), error.What() );
        return EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
    }

    // Validate every top-level destination before mkdir, project loading or an import job.
    wxFileName projectFn( targets.project );
    wxFileName boardFn( targets.board );
    wxFileName schFn( targets.schematic );
    KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY outputPolicy( projectFn.GetPath() );

    if( !projectFn.DirExists()
        && !wxFileName::Mkdir( projectFn.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        wxFprintf( stderr, _( "Could not create output directory: %s\n" ), projectFn.GetPath() );
        return EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
    }

    // LoadProject returns false for a not-yet-existing file but still registers a fully-defaulted
    // active project, so success is signalled by GetProject() returning it.
    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();

    mgr.LoadProject( projectFn.GetFullPath(), true );

    PROJECT* project = mgr.GetProject( projectFn.GetFullPath() );

    if( !project )
    {
        wxFprintf( stderr, _( "Could not create project: %s\n" ), projectFn.GetFullPath() );
        return EXIT_CODES::ERR_UNKNOWN;
    }
    REPORTER& reporter = CLI_REPORTER::GetInstance();

    bool haveBoard = false;
    bool haveSch = false;
    std::vector<wxString> boards;
    int  retCode = EXIT_CODES::SUCCESS;

    std::map<wxString, wxString> netNameMap;
    wxString boardInput;
    wxString boardInputId;
    std::map<wxString, wxString> footprintIds;
    std::map<wxString, std::set<wxString>> footprintPads;

    // Preserve board-first format detection, but defer loading until the schematic supplies its names.
    for( const wxString& input : inputFiles )
    {
        bool accepted = false;

        if( !haveBoard )
        {
            JOB_PCB_IMPORT probe;
            probe.m_inputFile = input;
            probe.m_probeOnly = true;

            int pcbResult = aKiway.ProcessJob( KIWAY::FACE_PCB, &probe, &reporter );

            if( pcbResult == EXIT_CODES::SUCCESS )
            {
                const auto& sourcePcbs = probe.m_projectBoards;

                if( sourcePcbs.size() == 1 )
                {
                    boards.push_back( boardFn.GetFullPath() );
                    haveBoard = true;
                    boardInput = input;
                    boardInputId = sourcePcbs.front().first;
                }
                else if( !sourcePcbs.empty() )
                {
                    const bool         hasMultipleBoards = sourcePcbs.size() > 1;
                    std::set<wxString> usedBoardNames;

                    for( const auto& [pcbId, sourceName] : sourcePcbs )
                    {
                        wxFileName outputFn = boardFn;

                        if( hasMultipleBoards )
                        {
                            wxString boardName = EscapeString( sourceName, CTX_FILENAME );

                            if( boardName.empty() )
                                boardName = pcbId;

                            wxString uniqueBoardName = boardName;
                            unsigned duplicate = 2;

                            while( !usedBoardNames.insert( uniqueBoardName ).second )
                            {
                                uniqueBoardName = wxString::Format( wxS( "%s-%u" ), boardName, duplicate );
                                ++duplicate;
                            }

                            outputFn.SetName( uniqueBoardName );
                        }

                        try
                        {
                            outputFn.Assign( outputPolicy.Target( KEMI_FORMAT::DOC::PCB,
                                                                   outputFn.GetFullPath() ).FinalPath() );

                            if( outputFn.Exists() )
                                THROW_IO_ERROR( _( "An imported board's destination already exists." ) );

                            wxFileName officialOutput( outputFn );
                            officialOutput.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PCB,
                                                                    KEMI_FORMAT::FLAVOR::LEGACY ) );

                            if( officialOutput.Exists() )
                                THROW_IO_ERROR( _( "An official board already uses the imported board's name." ) );
                        }
                        catch( const IO_ERROR& error )
                        {
                            wxFprintf( stderr, wxS( "%s\n" ), error.What() );
                            retCode = EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
                            break;
                        }

                        JOB_PCB_IMPORT pcbJob;
                        pcbJob.m_inputFile = input;
                        pcbJob.m_importPcbId = pcbId;
                        pcbJob.m_isPartOfMultiBoardProject = hasMultipleBoards;
                        pcbJob.SetConfiguredOutputPath( outputFn.GetFullPath() );

                        pcbResult = aKiway.ProcessJob( KIWAY::FACE_PCB, &pcbJob, &reporter );

                        if( pcbResult != EXIT_CODES::SUCCESS )
                        {
                            retCode = pcbResult;
                            break;
                        }

                        boards.push_back( outputFn.GetFullPath() );
                        haveBoard = true;
                    }

                    if( retCode != EXIT_CODES::SUCCESS )
                        break;
                }
                else
                {
                    boards.push_back( boardFn.GetFullPath() );
                    haveBoard = true;
                    boardInput = input;
                }

                accepted = true;
            }
            else if( pcbResult != EXIT_CODES::ERR_UNKNOWN_FILE_FORMAT )
            {
                retCode = pcbResult;
                break;
            }
        }

        // Some formats keep the board and the schematic in one file, so a board input is offered to both faces;
        // a board file without a schematic is still a good board
        if( !haveSch )
        {
            JOB_SCH_IMPORT schJob;
            schJob.m_inputFile = input;
            schJob.m_format = JOB_SCH_IMPORT::FORMAT::AUTO;
            schJob.SetConfiguredOutputPath( schFn.GetFullPath() );

            int schResult = aKiway.ProcessJob( KIWAY::FACE_SCH, &schJob, &reporter );

            if( schResult == EXIT_CODES::SUCCESS )
            {
                haveSch = true;
                netNameMap = std::move( schJob.m_netNameMap );
                accepted = true;
            }
            else if( schResult != EXIT_CODES::ERR_UNKNOWN_FILE_FORMAT && !accepted )
            {
                retCode = schResult;
                break;
            }
        }

        if( accepted )
            continue;

        if( haveBoard && haveSch )
        {
            wxFprintf( stderr, _( "Could not import '%s': a board and a schematic have already "
                                  "been imported for this project\n" ), input );
        }
        else
        {
            wxFprintf( stderr, _( "No board or schematic importer recognizes the file: %s\n" ), input );
        }

        retCode = EXIT_CODES::ERR_INVALID_INPUT_FILE;
        break;
    }

    if( retCode == EXIT_CODES::SUCCESS && !boardInput.empty() )
    {
        JOB_PCB_IMPORT pcbJob;
        pcbJob.m_inputFile = boardInput;
        pcbJob.m_format = JOB_PCB_IMPORT::FORMAT::AUTO;
        pcbJob.m_importPcbId = boardInputId;
        pcbJob.m_layerMap = layerMap;
        pcbJob.m_netNameMap = std::move( netNameMap );
        pcbJob.m_collectFootprintLinks = haveSch && boards.size() == 1;
        pcbJob.SetConfiguredOutputPath( boardFn.GetFullPath() );
        retCode = aKiway.ProcessJob( KIWAY::FACE_PCB, &pcbJob, &reporter );
        footprintIds = std::move( pcbJob.m_footprintIds );
        footprintPads = std::move( pcbJob.m_footprintPadNumbers );
    }

    if( retCode == EXIT_CODES::SUCCESS && haveSch && haveBoard )
    {
        if( boards.size() == 1 && !boardInput.IsEmpty() )
        {
            JOB_SCH_LINK_FOOTPRINTS link;
            link.m_filename = schFn.GetFullPath();
            link.m_projectFile = projectFn.GetFullPath();
            link.m_footprintsByRef = std::move( footprintIds );
            link.m_padNumbersByRef = std::move( footprintPads );
            retCode = aKiway.ProcessJob( KIWAY::FACE_SCH, &link, &reporter );
        }
        else
        {
            reporter.Report( _( "IMPORT_LINK_MULTIBOARD: automatic footprint linking was skipped because no unique schematic-to-board pairing is known." ),
                             RPT_SEVERITY_WARNING );
        }
    }

    if( retCode == EXIT_CODES::SUCCESS && !haveBoard && !haveSch )
    {
        wxFprintf( stderr, _( "No inputs could be imported\n" ) );
        retCode = EXIT_CODES::ERR_INVALID_INPUT_FILE;
    }

    if( retCode == EXIT_CODES::SUCCESS && project )
    {
        // The schematic handler already registered any sheets; link the board here.
        if( haveBoard )
        {
            try
            {
                KEMI_FORMAT::RegisterImportedBoards( *project, boards );
            }
            catch( const IO_ERROR& error )
            {
                wxFprintf( stderr, wxS( "%s\n" ), error.What() );
                return EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
            }
        }

        if( mgr.SaveProject( projectFn.GetFullPath() )
            && project->GetLocalSettings().SaveToFile( projectFn.GetPath(), true ) )
        {
            reporter.Report( wxString::Format( _( "Created project '%s'\n" ),
                                               projectFn.GetFullPath() ),
                             RPT_SEVERITY_INFO );

            if( KEMI_FORMAT::WriteImportZip( projectFn.GetFullPath(), &reporter,
                                              inputFiles ) )
            {
                reporter.Report( _( "Wrote project zip archive\n" ), RPT_SEVERITY_INFO );
            }
            else
            {
                wxFprintf( stderr, _( "Failed to write project zip for '%s'\n" ),
                           projectFn.GetFullPath() );
                retCode = EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
            }
        }
        else
        {
            wxFprintf( stderr, _( "Failed to write project file: %s\n" ),
                       projectFn.GetFullPath() );
            retCode = EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
        }
    }

    mgr.UnloadProject( project, false );

    return retCode;
}
