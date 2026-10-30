/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Jon Evans <jon@craftyjon.com>
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <api/headless_pcb_context.h>
#include <board.h>
#include <board_loader.h>
#include <common.h>
#include <component_classes/component_class_manager.h>
#include <drc/drc_engine.h>
#include <drc/rule_editor/drc_re_rule_loader.h>
#include <drc/rule_editor/drc_re_rule_saver.h>
#include <wx/log.h>
#include <footprint.h>
#include <gestfich.h>
#include <kemi_format_profile.h>
#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/netlist_reader.h>
#include <netlist_reader/pcb_netlist_utils.h>
#include <pcbnew_settings.h>
#include <pgm_base.h>
#include <project.h>
#include <reporter.h>
#include <settings/settings_manager.h>
#include <spread_footprints.h>
#include <tool/tool_manager.h>
#include <tools/drc_tool.h>
#include <wx/debug.h>


HEADLESS_PCB_CONTEXT::HEADLESS_PCB_CONTEXT( std::unique_ptr<BOARD> aBoard, PROJECT* aProject,
                        APP_SETTINGS_BASE* aSettings,
                        KIWAY* aKiway ) :
        m_board( std::move( aBoard ) ),
        m_project( aProject ),
        m_kiway( aKiway ),
        m_toolManager( std::make_unique<TOOL_MANAGER>() )
{
    wxCHECK( m_board, /* void */ );
    wxCHECK( m_project, /* void */ );

    m_board->SetProject( m_project );
    m_toolManager->SetEnvironment( m_board.get(), nullptr, nullptr, aSettings, nullptr );
}


HEADLESS_PCB_CONTEXT::~HEADLESS_PCB_CONTEXT()
{
    // Sever the board↔project linkage before destruction. The PROJECT holds a raw pointer
    // (m_BoardSettings) to the board's design settings. If the board is destroyed while the
    // project still exists, that pointer becomes dangling.
    if( m_board )
        m_board->ClearProject();
}


BOARD* HEADLESS_PCB_CONTEXT::GetBoard() const
{
    return m_board.get();
}


PROJECT& HEADLESS_PCB_CONTEXT::Prj() const
{
    // Shouldn't be able to construct this without a project
    wxASSERT( m_project );
    return *m_project;
}


TOOL_MANAGER* HEADLESS_PCB_CONTEXT::GetToolManager() const
{
    return m_toolManager.get();
}


wxString HEADLESS_PCB_CONTEXT::GetCurrentFileName() const
{
    if( !m_board )
        return wxEmptyString;

    return m_board->GetFileName();
}


bool HEADLESS_PCB_CONTEXT::SaveBoard()
{
    if( !m_board )
        return false;

    wxString fileName = m_board->GetFileName();

    if( fileName.IsEmpty() )
        return false;

    bool success = BOARD_LOADER::SaveBoard( fileName, *m_board );

    if( success )
    {
        m_board->SetFileName( fileName );
        m_contentModified = false;

        wxFileName pro = fileName;
        pro.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::KEMI ) );
        pro.MakeAbsolute();

        if( !KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::PROJECT,
                                      wxFileName( Prj().GetProjectFullName() ).GetExt() )
                || !wxFileExists( Prj().GetProjectFullName() ) )
        {
            Pgm().GetSettingsManager().SaveProjectAs( pro.GetFullPath(), m_board->GetProject() );
        }
    }

    return success;
}


bool HEADLESS_PCB_CONTEXT::SaveBoardAs( const wxString& aFileName )
{
    if( !m_board || aFileName.IsEmpty() )
        return false;

    wxFileName boardPath;

    try
    {
        boardPath.Assign( KEMI_FORMAT::PersistentOutputPath( KEMI_FORMAT::DOC::PCB, aFileName, true ) );
    }
    catch( const IO_ERROR& error )
    {
        wxLogError( wxS( "%s" ), error.What() );
        return false;
    }

    boardPath.MakeAbsolute();

    if( boardPath.GetName().IsEmpty() )
        return false;

    if( ( !boardPath.DirExists() && !boardPath.Mkdir() ) || !boardPath.IsDirWritable() )
        return false;

    wxString outPath = boardPath.GetFullPath();

    if( !BOARD_LOADER::SaveBoard( outPath, *m_board ) )
        return false;

    m_board->SetFileName( outPath );
    m_contentModified = false;

    wxFileName pro( boardPath );
    pro.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT,
            m_board->IsKemiOnly() ? KEMI_FORMAT::FLAVOR::KEMI : KEMI_FORMAT::FLAVOR::LEGACY ) );

    if( !KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::PROJECT,
                                  wxFileName( Prj().GetProjectFullName() ).GetExt() )
            || !wxFileExists( Prj().GetProjectFullName() ) )
    {
        Pgm().GetSettingsManager().SaveProjectAs( pro.GetFullPath(), m_board->GetProject() );
    }

    return true;
}


bool HEADLESS_PCB_CONTEXT::SavePcbCopy( const wxString& aFileName, bool aCreateProject, bool aHeadless )
{
    if( !m_board || aFileName.IsEmpty() )
        return false;

    wxString outPath;

    try
    {
        outPath = KEMI_FORMAT::PersistentOutputPath( KEMI_FORMAT::DOC::PCB, aFileName, true );
    }
    catch( const IO_ERROR& error )
    {
        wxLogError( wxS( "%s" ), error.What() );
        return false;
    }

    wxFileName pro = outPath;
    pro.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::KEMI ) );
    pro.MakeAbsolute();

    if( aCreateProject && pro.FileExists() )
        return false;

    wxFileName currentRules( m_board->GetDesignRulesPath() );
    wxFileName rulesFile = outPath;
    rulesFile.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::DESIGN_RULES, KEMI_FORMAT::FLAVOR::KEMI ) );
    rulesFile.MakeAbsolute();

    if( currentRules.FileExists() && rulesFile.FileExists() )
        return false;

    if( aCreateProject )
    {
        // A real official project requires whole-project import.  Fail before writing the board.
        if( KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::PROJECT,
                                      wxFileName( Prj().GetProjectFullName() ).GetExt() )
                && wxFileExists( Prj().GetProjectFullName() ) )
        {
            return false;
        }

        if( !Pgm().GetSettingsManager().SaveProjectCopy( pro.GetFullPath(), m_board->GetProject() ) )
            return false;
    }

    bool success = BOARD_LOADER::SaveBoard( outPath, *m_board, PCB_IO_MGR::KICAD_SEXP,
                                            /* aAllowOverwrite = */ true );

    if( success && currentRules.FileExists() )
    {
        DRC_RULE_LOADER loader;
        const auto entries = loader.LoadFile( currentRules.GetFullPath() );
        DRC_RULE_SAVER saver;

        if( !loader.GetLoadError().IsEmpty()
                || !saver.SaveFile( rulesFile.GetFullPath(), entries, nullptr, loader.GetFileTrivia() ) )
        {
            return false;
        }
    }

    return success;
}


bool HEADLESS_PCB_CONTEXT::ReadNetlistFromFile( const wxString& aFilename, NETLIST& aNetlist, REPORTER& aReporter )
{
    wxString msg;

    try
    {
        std::unique_ptr<NETLIST_READER> netlistReader(
                NETLIST_READER::GetNetlistReader( &aNetlist, aFilename, wxEmptyString ) );

        if( !netlistReader.get() )
        {
            msg.Printf( _( "Cannot open netlist file '%s'." ), aFilename );
            aReporter.Report( msg, RPT_SEVERITY_ERROR );
            return false;
        }

        netlistReader->LoadNetlist();
        LoadNetlistFootprints( GetBoard(), aNetlist, aReporter );
    }
    catch( const IO_ERROR& ioe )
    {
        msg.Printf( _( "Error loading netlist.\n%s" ), ioe.What().GetData() );
        aReporter.Report( msg, RPT_SEVERITY_ERROR );
        return false;
    }

    return true;
}


std::unique_ptr<BOARD_NETLIST_UPDATER> HEADLESS_PCB_CONTEXT::MakeNetlistUpdater()
{
    return std::make_unique<BOARD_NETLIST_UPDATER>( GetToolManager(), GetBoard() );
}


void HEADLESS_PCB_CONTEXT::OnNetlistChanged( BOARD_NETLIST_UPDATER& aUpdater )
{
    BOARD* board = GetBoard();

    // Re-sync nets and  netclasses
    board->SynchronizeNetsAndNetClasses( false );

    // Recompute component classes
    board->GetComponentClassManager().InvalidateComponentClasses();
    board->GetComponentClassManager().RebuildRequiredCaches();

    // Resync DRC rules to account for new aggregate netclass / component class rules
    if( DRC_TOOL* drcTool = m_toolManager->GetTool<DRC_TOOL>() )
    {
        if( drcTool->GetDRCEngine() )
        {
            try
            {
                drcTool->GetDRCEngine()->InitEngine( board->GetDesignRulesPath() );
            }
            catch( PARSE_ERROR& )
            {
            }
        }
    }

    std::vector<FOOTPRINT*> newFootprints = aUpdater.GetAddedFootprints();
    SpreadFootprints( &newFootprints, { 0, 0 }, true );

    board->CompileRatsnest();

    SetContentModified();
}


bool HEADLESS_PCB_CONTEXT::RevertToSaved()
{
    wxCHECK( m_board && m_project, false );

    wxString fileName = m_board->GetFileName();

    if( fileName.IsEmpty() || !wxFileExists( fileName ) )
        return false;

    PCB_IO_MGR::PCB_FILE_T pluginType = PCB_IO_MGR::FindPluginTypeFromBoardPath( fileName, KICTL_KICAD_ONLY );

    if( pluginType == PCB_IO_MGR::FILE_TYPE_NONE )
        return false;

    std::unique_ptr<BOARD> reloaded;

    try
    {
        reloaded = BOARD_LOADER::Load( fileName, pluginType, m_project );
    }
    catch( ... )
    {
        return false;
    }

    if( !reloaded )
        return false;

    m_board = std::move( reloaded );
    m_board->SetProject( m_project );
    m_toolManager->SetEnvironment( m_board.get(), nullptr, nullptr, GetAppSettings<PCBNEW_SETTINGS>( "pcbnew" ),
                                   nullptr );
    m_contentModified = false;

    return true;
}
