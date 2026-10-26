/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2014-2016 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Maciej Suminski <maciej.suminski@cern.ch>
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
// kicad_curl_easy.h must be included before wxWidgets because on Windows (msys2), there are
// collision with wx headers and curl.h defs
#include <kicad_curl/kicad_curl_easy.h>

#include <bitmaps.h>
#include <build_version.h>
#include <common.h>     // for SearchHelpFileFullPath
#include <pgm_base.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <eda_draw_frame.h>
#include <view/view.h>
#include <gal/graphics_abstraction_layer.h>
#include <base_screen.h>
#include <tool/common_control.h>

#include <api/api_plugin_manager.h>
#include <id.h>
#include <kiface_base.h>
#include <dialogs/dialog_configure_paths.h>
#include <eda_doc.h>
#include <launch_ext.h>
#include <paths.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <executable_names.h>
#include <gestfich.h>
#include <tools/kicad_manager_actions.h>
#include <confirm.h>
#include <reporter.h>
#include <widgets/kistatusbar.h>

#define KEMI_DOCS_BASE wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/" )

/// Online documentation index (docs/kemi/README.md).
#define URL_DOCUMENTATION KEMI_DOCS_BASE wxS( "README.md" )

/// docs/kemi/入门指南.md, percent-encoded UTF-8.
#define URL_GETTING_STARTED wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E5%85%A5%E9%97%A8%E6%8C%87%E5%8D%97.md" )

/// docs/kemi/使用帮助.md, used by the project manager.
#define URL_USER_HELP wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E4%BD%BF%E7%94%A8%E5%B8%AE%E5%8A%A9.md" )


/**
 * Return the docs/kemi page for the frame's Help entry.  Symbol and footprint editors share a
 * kiface (and so a help file name) with their main editors, so pick by the untranslated
 * program title instead.
 */
static wxString kemiHelpUrl( EDA_BASE_FRAME* aFrame )
{
    static const std::pair<const wxChar*, const wxChar*> pages[] = {
        { wxS( "KemiCAD Schematic Editor" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E5%8E%9F%E7%90%86%E5%9B%BE%E7%BC%96%E8%BE%91%E5%99%A8.md" ) }, // 原理图编辑器.md
        { wxS( "KemiCAD Symbol Editor" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E7%AC%A6%E5%8F%B7%E7%BC%96%E8%BE%91%E5%99%A8.md" ) }, // 符号编辑器.md
        { wxS( "KemiCAD Symbol Library Browser" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E7%AC%A6%E5%8F%B7%E4%B8%8E%E5%B0%81%E8%A3%85%E5%BA%93.md" ) }, // 符号与封装库.md
        { wxS( "KemiCAD PCB Editor" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/PCB%E7%BC%96%E8%BE%91%E5%99%A8.md" ) }, // PCB编辑器.md
        { wxS( "KemiCAD Footprint Editor" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E5%B0%81%E8%A3%85%E7%BC%96%E8%BE%91%E5%99%A8.md" ) }, // 封装编辑器.md
        { wxS( "KemiCAD Footprint Library Browser" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E7%AC%A6%E5%8F%B7%E4%B8%8E%E5%B0%81%E8%A3%85%E5%BA%93.md" ) }, // 符号与封装库.md
        { wxS( "KemiCAD 3D Viewer" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/PCB%E7%BC%96%E8%BE%91%E5%99%A8.md" ) }, // PCB编辑器.md
        { wxS( "KemiCAD Gerber Viewer" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/Gerber%E6%9F%A5%E7%9C%8B%E5%99%A8.md" ) }, // Gerber查看器.md
        { wxS( "KemiCAD Calculator Tools" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/PCB%E8%AE%A1%E7%AE%97%E5%99%A8.md" ) }, // PCB计算器.md
        { wxS( "KemiCAD Drawing Sheet Editor" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E5%9B%BE%E6%A1%86%E7%BC%96%E8%BE%91%E5%99%A8.md" ) }, // 图框编辑器.md
        { wxS( "KemiCAD Assign Footprints" ), wxS( "https://github.com/newlinksz/KemiCAD/blob/main/docs/kemi/%E5%88%86%E9%85%8D%E5%B0%81%E8%A3%85.md" ) }, // 分配封装.md
    };

    if( !aFrame )
        return URL_DOCUMENTATION;

    const wxString& title = aFrame->GetUntranslatedAboutTitle();

    for( const auto& [name, url] : pages )
    {
        if( title == name )
            return url;
    }

    if( aFrame->IsType( KICAD_MAIN_FRAME_T ) )
        return URL_USER_HELP;

    return URL_DOCUMENTATION;
}


/// URL to launch a new issue with pre-populated description
wxString COMMON_CONTROL::m_bugReportUrl =
        wxS( "https://github.com/newlinksz/KemiCAD/issues/new?body=%s" );


/// Issue template to use for reporting bugs (this should not be translated)
wxString COMMON_CONTROL::m_bugReportTemplate = wxS(
        "```\n"
        "%s\n"
        "```" );


void COMMON_CONTROL::Reset( RESET_REASON aReason )
{
    m_frame = getEditFrame<EDA_BASE_FRAME>();
}


int COMMON_CONTROL::OpenPreferences( const TOOL_EVENT& aEvent )
{
    m_frame->ShowPreferences( wxEmptyString, wxEmptyString );
    return 0;
}


int COMMON_CONTROL::ConfigurePaths( const TOOL_EVENT& aEvent )
{
    // If _pcbnew.kiface is running have it put up the dialog so the 3D paths can also
    // be edited
    if( KIFACE* pcbnew = m_frame->Kiway().KiFACE( KIWAY::FACE_PCB, false ) )
    {
        try
        {
            pcbnew->CreateKiWindow( m_frame, DIALOG_CONFIGUREPATHS, &m_frame->Kiway() );
        }
        catch( ... )
        {
            // Do nothing here.
            // A error message is displayed after trying to load _pcbnew.kiface.
        }
    }
    else
    {
        DIALOG_CONFIGURE_PATHS dlg( m_frame );

        if( dlg.ShowModal() == wxID_OK )
            m_frame->Kiway().CommonSettingsChanged( ENVVARS_CHANGED );
    }

    return 0;
}


int COMMON_CONTROL::ShowLibraryTable( const TOOL_EVENT& aEvent )
{
    if( aEvent.IsAction( &ACTIONS::showSymbolLibTable ) )
    {
        try     // Sch frame was not available, try to start it
        {
            if( KIFACE* kiface = m_frame->Kiway().KiFACE( KIWAY::FACE_SCH ) )
                kiface->CreateKiWindow( m_frame, DIALOG_SCH_LIBRARY_TABLE, &m_frame->Kiway() );
        }
        catch( ... )
        {
            // _eeschema.kiface is not available: it contains the library table dialog.
            // Do nothing here.
            // A error message is displayed after trying to load _eeschema.kiface.
        }
    }
    else if( aEvent.IsAction( &ACTIONS::showFootprintLibTable ) )
    {
        try     // Pcb frame was not available, try to start it
        {
            if( KIFACE* kiface = m_frame->Kiway().KiFACE( KIWAY::FACE_PCB ) )
                kiface->CreateKiWindow( m_frame, DIALOG_PCB_LIBRARY_TABLE, &m_frame->Kiway() );
        }
        catch( ... )
        {
            // _pcbnew.kiface is not available: it contains the library table dialog.
            // Do nothing here.
            // A error message is displayed after trying to load _pcbnew.kiface.
        }
    }
    else if( aEvent.IsAction( &ACTIONS::showDesignBlockLibTable ) )
    {
        try     // Kicad frame was not available, try to start it
        {
            if( KIFACE* kiface = m_frame->Kiway().KiFACE( KIWAY::FACE_SCH ) )
                kiface->CreateKiWindow( m_frame, DIALOG_DESIGN_BLOCK_LIBRARY_TABLE, &m_frame->Kiway() );
        }
        catch( ... )
        {
            // _eeschema.kiface is not available: it contains the library table dialog.
            // Do nothing here.
            // A error message is displayed after trying to load _eeschema.kiface.
        }
    }

    return 0;
}


void showFrame( EDA_BASE_FRAME* aFrame )
{
    // Needed on Windows, other platforms do not use it, but it creates no issue
    if( aFrame->IsIconized() )
        aFrame->Iconize( false );

    aFrame->Raise();

    // Raising the window does not set the focus on Linux.  This should work on
    // any platform.
    if( wxWindow::FindFocus() != aFrame )
        aFrame->SetFocus();

    // If the player is currently blocked, focus the user attention on the correct window
    if( wxWindow* blocking_win = aFrame->Kiway().GetBlockingDialog() )
    {
        blocking_win->Raise();
        blocking_win->SetFocus();
    }
}


int COMMON_CONTROL::ShowPlayer( const TOOL_EVENT& aEvent )
{
    FRAME_T       playerType = aEvent.Parameter<FRAME_T>();
    KIWAY_PLAYER* editor = m_frame->Kiway().Player( playerType, true );

    // editor can be null if Player() fails:
    wxCHECK_MSG( editor != nullptr, 0, wxT( "Cannot open/create the editor frame" ) );

    showFrame( editor );

    return 0;
}


int COMMON_CONTROL::Quit( const TOOL_EVENT& aEvent )
{
    m_frame->CallAfter(
            [this]()
            {
                m_frame->Kiway().OnKiCadExit();
            } );

    return 0;
}


class TERMINATE_HANDLER : public wxProcess
{
public:
    TERMINATE_HANDLER( const wxString& appName )
    { }

    void OnTerminate( int pid, int status ) override
    {
        delete this;
    }
};


int COMMON_CONTROL::Execute( const wxString& aExecutible, const wxString& aParam )
{
    TERMINATE_HANDLER* callback = new TERMINATE_HANDLER( aExecutible );

    bool isKicadBinary = false;

    if( aExecutible == GERBVIEW_EXE || aExecutible == PCB_CALCULATOR_EXE || aExecutible == PL_EDITOR_EXE
        || aExecutible == EESCHEMA_EXE || aExecutible == PCBNEW_EXE )
    {
        isKicadBinary = true;
    }

    long pid = ExecuteFile( aExecutible, aParam, callback, isKicadBinary );

    if( pid > 0 )
    {
#ifdef __WXMAC__
        wxString script;

        script.Printf( wxS( "tell application \"System Events\" to tell application process \"%s\"\n"
                            "   set frontmost to true\n"
                            "end tell" ),
                       aExecutible );

        // This non-parameterized use of wxExecute is fine because script is not derived
        // from user input.
        wxExecute( wxString::Format( "osascript -e '%s'", script ) );
#endif
    }
    else
    {
        delete callback;
    }

    return 0;
}


int COMMON_CONTROL::Execute( const TOOL_EVENT& aEvent )
{
    wxString execFile;
    wxString param;

    if( aEvent.IsAction( &ACTIONS::showCalculatorTools ) )
        execFile = PCB_CALCULATOR_EXE;
    else
        wxFAIL_MSG( "Execute(): unexpected request" );

    if( execFile.IsEmpty() )
        return 0;

    return Execute( execFile, param );
}


int COMMON_CONTROL::ShowProjectManager( const TOOL_EVENT& aEvent )
{
    // Note: dynamic_cast doesn't work over the Kiway() on MacOS.  We have to use static_cast
    // here.
    EDA_BASE_FRAME* top = static_cast<EDA_BASE_FRAME*>( m_frame->Kiway().GetTop() );

    if( top && top->GetFrameType() == KICAD_MAIN_FRAME_T )
        showFrame( top );
    else
        KICAD_MESSAGE_DIALOG( m_frame, _( "Can not switch to project manager in stand-alone mode." ) );

    return 0;
}


int COMMON_CONTROL::ShowHelp( const TOOL_EVENT& aEvent )
{
    wxString helpFile;

    /* We have to get document for beginners,
     * or the full specific doc
     * if event id is wxID_INDEX, we want the document for beginners.
     * else the specific doc file (its name is in Kiface().GetHelpFileName())
     * The document for beginners is the same for all KiCad utilities
     */
    if( aEvent.IsAction( &ACTIONS::gettingStarted ) )
    {
        // List of possible names for Getting Started in KiCad
        const wxChar* names[2] = {
                wxT( "getting_started_in_kicad" ),
                wxT( "Getting_Started_in_KiCad" )
        };

        // Search for "getting_started_in_kicad.html" or "getting_started_in_kicad.pdf"
        // or "Getting_Started_in_KiCad.html" or "Getting_Started_in_KiCad.pdf"
        for( auto& name : names )
        {
            helpFile = SearchHelpFileFullPath( name );

            if( !helpFile.IsEmpty() )
                break;
        }

        if( !helpFile )
        {
            // No local help is shipped; open the KemiCAD online page directly.
            helpFile = URL_GETTING_STARTED;
        }
    }
    else
    {
        wxString base_name = m_frame->help_name();

        helpFile = SearchHelpFileFullPath( base_name );

        if( !helpFile )
        {
            // No local help is shipped; open the KemiCAD online page directly.
            helpFile = kemiHelpUrl( m_frame );
        }
    }

    GetAssociatedDocument( m_frame, helpFile, &m_frame->Prj() );
    return 0;
}


int COMMON_CONTROL::About( const TOOL_EVENT& aEvent )
{
    void ShowAboutDialog( EDA_BASE_FRAME * aParent ); // See AboutDialog_main.cpp
    ShowAboutDialog( m_frame );
    return 0;
}


int COMMON_CONTROL::ListHotKeys( const TOOL_EVENT& aEvent )
{
    DisplayHotkeyList( m_frame );
    return 0;
}


int COMMON_CONTROL::ReportBug( const TOOL_EVENT& aEvent )
{
    if( WarnUserIfOperatingSystemUnsupported() )
        return 0;

    wxString version = GetVersionInfoData( m_frame->GetUntranslatedAboutTitle(), false, true );

    wxString message;
    message.Printf( m_bugReportTemplate, version );

    KICAD_CURL_EASY kcurl;
    wxString url_string;
    url_string.Printf( m_bugReportUrl, kcurl.Escape( std::string( message.utf8_str() ) ) );

    wxLaunchDefaultBrowser( url_string );

    return 0;
}


int COMMON_CONTROL::ReloadPlugins( const TOOL_EVENT& aEvent )
{
    if( Pgm().GetCommonSettings()->m_Api.enable_server )
    {
        std::shared_ptr<REPORTER> reporter;

        if( KISTATUSBAR* statusBar = dynamic_cast<KISTATUSBAR*>( m_frame->GetStatusBar() ) )
            reporter = std::make_shared<STATUSBAR_WARNING_REPORTER>( statusBar, wxS( "plugin" ) );

        Pgm().GetPluginManager().ReloadPlugins( std::nullopt, reporter );
    }
    return 0;
}


int COMMON_CONTROL::ShowPluginFolder( const TOOL_EVENT& aEvent )
{
    wxString dir( PATHS::GetUserPluginsPath() );

    if( !wxFileName::DirExists( dir ) )
    {
        if( !PATHS::EnsurePathExists( dir ) )
        {
            DisplayErrorMessage( m_frame,
                                 wxString::Format( _( "Unable to create plugin directory '%s'." ),
                                                   dir ) );
            return 0;
        }
    }

    if( !LaunchExternal( dir ) )
        DisplayErrorMessage( m_frame,
                             wxString::Format( _( "Unable to open plugin directory '%s'." ),
                                               dir ) );

    return 0;
}


void COMMON_CONTROL::setTransitions()
{
    Go( &COMMON_CONTROL::Quit,               ACTIONS::quit.MakeEvent() );

    Go( &COMMON_CONTROL::OpenPreferences,    ACTIONS::openPreferences.MakeEvent() );
    Go( &COMMON_CONTROL::ConfigurePaths,     ACTIONS::configurePaths.MakeEvent() );
    Go( &COMMON_CONTROL::ShowLibraryTable,   ACTIONS::showSymbolLibTable.MakeEvent() );
    Go( &COMMON_CONTROL::ShowLibraryTable,   ACTIONS::showFootprintLibTable.MakeEvent() );
    Go( &COMMON_CONTROL::ShowLibraryTable,   ACTIONS::showDesignBlockLibTable.MakeEvent() );
    Go( &COMMON_CONTROL::ShowPlayer,         ACTIONS::showSymbolBrowser.MakeEvent() );
    Go( &COMMON_CONTROL::ShowPlayer,         ACTIONS::showSymbolEditor.MakeEvent() );
    Go( &COMMON_CONTROL::ShowPlayer,         ACTIONS::showFootprintBrowser.MakeEvent() );
    Go( &COMMON_CONTROL::ShowPlayer,         ACTIONS::showFootprintEditor.MakeEvent() );
    Go( &COMMON_CONTROL::Execute,            ACTIONS::showCalculatorTools.MakeEvent() );
    Go( &COMMON_CONTROL::ShowProjectManager, ACTIONS::showProjectManager.MakeEvent() );

    Go( &COMMON_CONTROL::ShowHelp,           ACTIONS::gettingStarted.MakeEvent() );
    Go( &COMMON_CONTROL::ShowHelp,           ACTIONS::help.MakeEvent() );
    Go( &COMMON_CONTROL::ListHotKeys,        ACTIONS::listHotKeys.MakeEvent() );
    Go( &COMMON_CONTROL::ReportBug,          ACTIONS::reportBug.MakeEvent() );
    Go( &COMMON_CONTROL::About,              ACTIONS::about.MakeEvent() );
    Go( &COMMON_CONTROL::ReloadPlugins,      ACTIONS::pluginsReload.MakeEvent() );
    Go( &COMMON_CONTROL::ShowPluginFolder,   ACTIONS::pluginsShowFolder.MakeEvent() );
}


