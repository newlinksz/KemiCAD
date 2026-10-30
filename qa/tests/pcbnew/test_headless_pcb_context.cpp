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

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <api/headless_pcb_context.h>
#include <board.h>
#include <board_loader.h>
#include <pcb_io/pcb_io_mgr.h>
#include <pcbnew_utils/board_file_utils.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <pgm_base.h>
#include <settings/settings_manager.h>
#include <tool/tool_manager.h>
#include <wx/ffile.h>
#include <wx/filename.h>


struct HEADLESS_PCB_CONTEXT_TEST_FIXTURE
{
    PROJECT* loadProject( const wxString& aBoardName )
    {
        wxFileName projectFile( wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() )
                                + aBoardName + ".kicad_pro" );

        m_settingsManager.LoadProject( projectFile.GetFullPath() );

        PROJECT* project = m_settingsManager.GetProject( projectFile.GetFullPath() );

        BOOST_REQUIRE_MESSAGE( project, "Could not load project" );
        return project;
    }

    wxString boardPath( const wxString& aBoardName ) const
    {
        return wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() ) + aBoardName + ".kicad_pcb";
    }

    SETTINGS_MANAGER m_settingsManager;
};


BOOST_FIXTURE_TEST_CASE( HeadlessBoardContextBasics, HEADLESS_PCB_CONTEXT_TEST_FIXTURE )
{
    PROJECT* project = loadProject( "reference_images_load_save" );

    std::unique_ptr<BOARD> board =
            BOARD_LOADER::Load( boardPath( "reference_images_load_save" ),
                                PCB_IO_MGR::KICAD_SEXP,
                                project );

    BOOST_REQUIRE( board );

    HEADLESS_PCB_CONTEXT context( std::move( board ), project, nullptr );

    BOOST_CHECK( context.GetBoard() );
    BOOST_CHECK_EQUAL( context.GetBoard()->GetProject(), project );
    BOOST_CHECK_EQUAL( context.Prj().GetProjectFullName(), project->GetProjectFullName() );
    BOOST_CHECK( context.GetToolManager() );
    BOOST_CHECK_EQUAL( context.GetToolManager()->GetModel(), context.GetBoard() );
    BOOST_CHECK_EQUAL( context.GetCurrentFileName(), context.GetBoard()->GetFileName() );
    BOOST_CHECK( context.CanAcceptApiCommands() );
}


// Phase-6 P1: rejected board copies must not create a companion rules file.
BOOST_AUTO_TEST_CASE( HeadlessRejectedKemiCopyDoesNotCopyRules )
{
    SETTINGS_MANAGER settings;
    KI_TEST::SCOPED_TEMP_PROJECT project( settings, wxS( "kemi-headless-copy" ), wxS( "board" ) );
    auto board = std::make_unique<BOARD>();
    const wxString source = wxFileName( project.DirStr(), wxS( "board.kemi_pcb" ) ).GetFullPath();
    board->SetFileName( source );
    HEADLESS_PCB_CONTEXT context( std::move( board ), &project.Project(), nullptr );
    const wxString rules = context.GetBoard()->GetDesignRulesPath();
    {
        wxFFile file( rules, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(kemi_dru (format_version 1) (version 2))\n" ), wxConvUTF8 ) );
    }
    const std::string before = KI_TEST::LoadStringData( rules );
    const wxString rejected = wxFileName( project.DirStr(), wxS( "copy.kicad_pcb" ) ).GetFullPath();
    wxLogNull silence;
    BOOST_CHECK( !context.SavePcbCopy( rejected, true, true ) );
    BOOST_CHECK( !wxFileExists( rejected ) );
    BOOST_CHECK( !wxFileExists( wxFileName( project.DirStr(), wxS( "copy.kicad_dru" ) ).GetFullPath() ) );
    BOOST_CHECK( !wxFileExists( wxFileName( project.DirStr(), wxS( "copy.kemi_dru" ) ).GetFullPath() ) );
    BOOST_CHECK( !wxFileExists( wxFileName( project.DirStr(), wxS( "copy.kicad_pro" ) ).GetFullPath() ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( rules ), before );
    BOOST_CHECK_EQUAL( context.GetCurrentFileName(), source );
}


// Phase-6 P2: an explicit native Save As target must keep its name and remain loadable.
BOOST_AUTO_TEST_CASE( HeadlessSaveAsAcceptsKemiBoardTarget )
{
    SETTINGS_MANAGER& settings = Pgm().GetSettingsManager();
    KI_TEST::SCOPED_TEMP_PROJECT project( settings, wxS( "kemi-headless-save-as" ), wxS( "board" ) );
    auto board = std::make_unique<BOARD>();
    board->SetKemiOnly();
    HEADLESS_PCB_CONTEXT context( std::move( board ), &project.Project(), nullptr );
    context.SetContentModified();
    const wxString rejected = wxFileName( project.DirStr(), wxS( "copy.kicad_pcb" ) ).GetFullPath();
    {
        wxLogNull silence;
        BOOST_CHECK( !context.SaveBoardAs( rejected ) );
    }
    BOOST_CHECK( !wxFileExists( rejected ) );
    BOOST_CHECK( context.IsContentModified() );
    const wxString target = wxFileName( project.DirStr(), wxS( "copy.kemi_pcb" ) ).GetFullPath();
    BOOST_REQUIRE( context.SaveBoardAs( target ) );
    BOOST_CHECK_EQUAL( context.GetCurrentFileName(), target );
    BOOST_CHECK( !context.IsContentModified() );
    BOOST_CHECK( !wxFileExists( target + wxS( ".kicad_pcb" ) ) );
    BOOST_CHECK( KI_TEST::LoadStringData( target ).starts_with( "(kemi_pcb" ) );
    PCB_IO_KICAD_SEXPR io;
    std::unique_ptr<BOARD> reopened = io.LoadBoard( target );
    BOOST_REQUIRE( reopened );
    BOOST_CHECK( reopened->IsKemiOnly() );
}
