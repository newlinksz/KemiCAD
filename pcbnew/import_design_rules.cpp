/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "import_design_rules.h"

#include <board.h>
#include <common.h>
#include <drc/drc_rule_parser.h>
#include <kemi_format_profile.h>
#include <ki_exception.h>
#include <pcb_io/pcb_io_mgr.h>
#include <project.h>
#include <reporter.h>
#include <richio.h>
#include <settings/settings_manager.h>
#include <wx/ffile.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/log.h>

#include <functional>
#include <memory>


void SaveNativeDesignRules( const wxString& aSource, const wxString& aTarget )
{
    KEMI_FORMAT::CheckPersistentOutput( KEMI_FORMAT::DOC::DESIGN_RULES, aTarget,
                                        KEMI_FORMAT::FLAVOR::KEMI, true );

    if( wxFileName( aTarget ).Exists() )
        THROW_IO_ERRORF( _( "Template destination '%s' already exists." ), aTarget );

    wxString contents;

    {
        wxFFile input( aSource, wxS( "rb" ) );

        if( !input.IsOpened() || !input.ReadAll( &contents, wxConvUTF8 ) )
            THROW_IO_ERRORF( _( "Cannot read template rules '%s'." ), aSource );
    }

    // Match the template's preference for native companions when official originals remain.
    wxFileName projectPath( aSource );
    projectPath.SetExt( wxString::FromUTF8(
            KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::KEMI ) ) );

    if( !projectPath.FileExists() )
        projectPath.SetExt( wxString::FromUTF8(
                KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::LEGACY ) ) );

    SETTINGS_MANAGER settings;
    PROJECT* project = nullptr;

    if( projectPath.FileExists() )
    {
        // Passive loading neither takes a project lock nor changes the active project/cwd.
        if( !settings.LoadProject( projectPath.GetFullPath(), false ) )
            THROW_IO_ERRORF( _( "Cannot load template project '%s'." ), projectPath.GetFullPath() );

        project = settings.GetProject( projectPath.GetFullPath() );
        project->SetReadOnly();
    }

    wxFileName boardPath( aSource );
    boardPath.SetExt( wxString::FromUTF8(
            KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PCB, KEMI_FORMAT::FLAVOR::KEMI ) ) );

    if( !boardPath.FileExists() )
        boardPath.SetExt( wxString::FromUTF8(
                KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PCB, KEMI_FORMAT::FLAVOR::LEGACY ) ) );

    std::unique_ptr<BOARD> board;

    if( boardPath.FileExists() )
    {
        board = PCB_IO_MGR::Load( PCB_IO_MGR::KICAD_SEXP, boardPath.GetFullPath(), nullptr, project );
    }
    else
    {
        // Rules-only templates still have a board context when the new project is opened.
        board = std::make_unique<BOARD>();
        board->SetFileName( boardPath.GetFullPath() );
    }

    board->SetProject( project );

    std::function<bool( wxString* )> resolver =
            [&]( wxString* aToken ) -> bool
            {
                if( IsComponentClassSelector( *aToken ) )
                    return false;

                return board->ResolveTextVar( aToken, 0 );
            };

    auto parseRules = [&]( const wxString& aText, const wxString& aPath )
    {
        STRING_LINE_READER reader( aText.ToStdString( wxConvUTF8 ), aPath );
        wxString expanded;

        // As in DRC_ENGINE::loadRules, expand each line in INTERNAL context. Only this
        // validation copy is expanded; the document written below retains variables/comments.
        while( char* line = reader.ReadLine() )
        {
            const wxString converted = board->ConvertCrossReferencesToKIIDs( wxString::FromUTF8( line ) );
            expanded << ExpandTextVars( converted, &resolver, INTERNAL ) << '\n';
        }

        std::vector<std::shared_ptr<DRC_RULE>> rules;
        DRC_RULES_PARSER parser( expanded, aPath );
        parser.Parse( rules, nullptr );
        return rules;
    };

    const auto sourceRules = parseRules( contents, aSource );

    // Use the DRC lexer to recognize the wrapper without interpreting comments or strings.
    DRC_RULES_PARSER header( contents, aSource );
    const int firstToken = header.NextTok();
    const bool native = firstToken == DSN_LEFT && header.NextTok() == DRCRULE_T::T_kemi_dru;

    if( !native )
    {
        // Empty and comment-only official rules are valid, but a native file needs a version.
        if( firstToken == DSN_EOF )
            contents += wxS( "\n(version 1)\n" );

        contents = wxString::Format( wxS( "(kemi_dru (format_version %d)\n" ),
                                     KEMI_FORMAT::FORMAT_VERSION ) + contents + wxS( "\n)\n" );
    }

    FILE_OUTPUTFORMATTER output( aTarget, wxS( "wb" ) );
    output.Print( "%s", contents.utf8_str().data() );
    output.Finish();

    wxFFile written( aTarget, wxS( "rb" ) );
    wxString reloaded;

    if( !written.IsOpened() || !written.ReadAll( &reloaded, wxConvUTF8 ) )
        THROW_IO_ERRORF( _( "Cannot reload template rules '%s'." ), aTarget );

    const auto reloadedRules = parseRules( reloaded, aTarget );

    if( sourceRules.size() != reloadedRules.size() )
        THROW_IO_ERRORF( _( "Template rules changed while saving '%s'." ), aTarget );
}


bool SaveImportedDesignRules( const wxString& aRules, const wxFileName& aBoardPath, REPORTER& aReporter )
{
    if( aRules.IsEmpty() )
        return false;

    // a nameless board would put the rules in a hidden file
    if( aBoardPath.GetName().IsEmpty() )
    {
        aReporter.Report( _( "Imported design rules were not saved, the board has no name.\n" ), RPT_SEVERITY_WARNING );
        return false;
    }

    // Imported rules are always a Kemi container beside the board, never an official sidecar
    try
    {
        KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY policy( aBoardPath.GetPath() );

        policy.Write( KEMI_FORMAT::DOC::DESIGN_RULES, aBoardPath.GetName(),
                [&]( const wxString& aPath, const KEMI_FORMAT::OUTPUT_TARGET& aTarget )
                {
                    const wxString content = wxString::Format( wxS( "(%s (format_version %d)\n" ),
                            wxString::FromUTF8( KEMI_FORMAT::RootToken( aTarget.Document(), aTarget.Flavor() ) ),
                            KEMI_FORMAT::FORMAT_VERSION ) + aRules + wxS( "\n)\n" );

                    // Creating exclusively refuses an existing user file without a race
                    wxFile rulesFile;

                    {
                        wxLogNull noLog;
                        rulesFile.Open( aPath, wxFile::write_excl );
                    }

                    if( !rulesFile.IsOpened() )
                        return false;

                    bool written = rulesFile.Write( content, wxConvUTF8 );

                    if( !rulesFile.Close() )
                        written = false;

                    // A partial file would not parse and would block the next import
                    if( !written )
                        wxRemoveFile( aPath );

                    return written;
                } );
    }
    catch( const IO_ERROR& ioe )
    {
        aReporter.Report( wxString::Format( _( "Imported design rules were not saved: %s\n" ), ioe.What() ),
                          RPT_SEVERITY_WARNING );
        return false;
    }

    return true;
}