/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
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

#include <boost/test/unit_test.hpp>
#include <local_history.h>
#include <kemi_format_profile.h>
#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_text.h>
#include <wx/ffile.h>
#include <pgm_base.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <schematic.h>
#include <settings/common_settings.h>
#include <settings/settings_manager.h>
#include <wildcards_and_files_ext.h>

#include <vector>

#include <wx/filename.h>
#include <wx/stdpaths.h>

BOOST_AUTO_TEST_SUITE( SchHistoryAutosave )

template <typename T>
struct SCOPED_SETTING_OVERRIDE
{
    SCOPED_SETTING_OVERRIDE( T& aRef, T aValue ) : m_ref( aRef ), m_original( aRef ) { aRef = aValue; }
    ~SCOPED_SETTING_OVERRIDE() { m_ref = m_original; }

    T& m_ref;
    T  m_original;
};


struct HISTORY_AUTOSAVE_FIXTURE
{
    HISTORY_AUTOSAVE_FIXTURE()
    {
        wxString tempDir = wxStandardPaths::Get().GetTempDir();
        wxString projectPath = tempDir + wxFileName::GetPathSeparator() + wxT( "legacy_autosave.kicad_pro" );

        m_settingsManager.LoadProject( projectPath.ToStdString() );
        m_schematic = std::make_unique<SCHEMATIC>( nullptr );
        m_schematic->SetProject( &m_settingsManager.Prj() );
        m_schematic->CreateDefaultScreens();
    }

    ~HISTORY_AUTOSAVE_FIXTURE()
    {
        // Clean up the entire .history directory under the project
        wxString historyPath = m_settingsManager.Prj().GetProjectPath() + wxS( ".history" );
        wxFileName::Rmdir( historyPath, wxPATH_RMDIR_RECURSIVE );
    }

    SETTINGS_MANAGER m_settingsManager;
    std::unique_ptr<SCHEMATIC> m_schematic;
};

BOOST_FIXTURE_TEST_CASE( SavesLegacySheetIntoHistoryPath, HISTORY_AUTOSAVE_FIXTURE )
{
    std::vector<SCH_SHEET*> sheets = m_schematic->GetTopLevelSheets();
    BOOST_REQUIRE( !sheets.empty() );

    SCH_SHEET* sheet = sheets[0];
    BOOST_REQUIRE( sheet->GetScreen() != nullptr );

    sheet->GetScreen()->SetContentModified();
    sheet->SetFileName( wxS( "legacy/legacy.sch" ) );
    sheet->GetScreen()->SetFileName( wxS( "legacy/legacy.sch" ) );

    std::vector<HISTORY_FILE_DATA> fileData;
    m_schematic->SaveToHistory( m_settingsManager.Prj().GetProjectPath(), fileData );

    BOOST_REQUIRE_EQUAL( fileData.size(), 1 );

    // SaveToHistory now emits project-relative paths -- the dispatcher joins them with
    // the storage root (.history/ or the user data dir) at write time.
    wxFileName saved( fileData[0].relativePath );
    BOOST_CHECK_EQUAL( saved.GetExt(), wxS( "kemi_sch" ) );
    BOOST_CHECK( saved.GetPath().Contains( wxS( "legacy" ) ) );
    BOOST_CHECK( !wxFileName( fileData[0].relativePath ).IsAbsolute() );
    BOOST_CHECK( !fileData[0].content.empty() );
    BOOST_CHECK( fileData[0].prettify );
}

// A clean sheet must still be captured while local history is active (backups on, format
// incremental).  The manual-save flow clears the dirty flag before the saver runs, so
// filtering on it here would drop the whole snapshot -- the regression that left .history
// stale for users on the Zip backup format (issue 24773).
BOOST_FIXTURE_TEST_CASE( CleanSheetStillSavedWhenLocalHistoryEnabled, HISTORY_AUTOSAVE_FIXTURE )
{
    COMMON_SETTINGS* cs = Pgm().GetCommonSettings();
    BOOST_REQUIRE( cs );

    SCOPED_SETTING_OVERRIDE<bool>          backupOn( cs->m_Backup.enabled, true );
    SCOPED_SETTING_OVERRIDE<BACKUP_FORMAT> incrementalFormat( cs->m_Backup.format,
                                                             BACKUP_FORMAT::INCREMENTAL );

    std::vector<SCH_SHEET*> sheets = m_schematic->GetTopLevelSheets();
    BOOST_REQUIRE( !sheets.empty() );

    SCH_SHEET* sheet = sheets[0];
    BOOST_REQUIRE( sheet->GetScreen() != nullptr );

    sheet->SetFileName( wxS( "clean/clean.kicad_sch" ) );
    sheet->GetScreen()->SetFileName( wxS( "clean/clean.kicad_sch" ) );
    sheet->GetScreen()->SetContentModified( false );

    std::vector<HISTORY_FILE_DATA> fileData;
    m_schematic->SaveToHistory( m_settingsManager.Prj().GetProjectPath(), fileData );

    BOOST_CHECK_EQUAL( fileData.size(), 1 );
}

BOOST_AUTO_TEST_CASE( HierarchyAutosaveReopensModifiedChildrenAfterTargetRemapping )
{
    COMMON_SETTINGS* settings = Pgm().GetCommonSettings();
    BOOST_REQUIRE( settings );
    SCOPED_SETTING_OVERRIDE<BACKUP_FORMAT> zip( settings->m_Backup.format, BACKUP_FORMAT::ZIP );

    for( bool parentModified : { false, true } )
    {
        for( bool existingNative : { false, true } )
        {
            SETTINGS_MANAGER manager;
            KI_TEST::SCOPED_TEMP_PROJECT project( manager, wxS( "hierarchy-recovery" ), wxS( "root" ) );
            SCHEMATIC schematic( &project.Project() );
            SCH_SHEET* root = schematic.GetTopLevelSheet();
            const wxString rootSource = wxFileName( project.DirStr(), wxS( "root.kicad_sch" ) ).GetFullPath();
            root->SetFileName( wxS( "root.kicad_sch" ) );
            root->GetScreen()->SetFileName( rootSource );
            root->GetScreen()->SetContentModified( parentModified );
            SCH_SHEET* child = new SCH_SHEET( &schematic );
            child->SetFileName( wxS( "child.kicad_sch" ) );
            child->SetScreen( new SCH_SCREEN( &schematic ) );
            const wxString childSource = wxFileName( project.DirStr(), wxS( "child.kicad_sch" ) ).GetFullPath();
            child->GetScreen()->SetFileName( childSource );
            child->GetScreen()->Append( new SCH_TEXT( VECTOR2I( 1000, 2000 ), wxS( "unsaved child" ) ) );
            child->GetScreen()->SetContentModified();
            root->GetScreen()->Append( child );
            schematic.RefreshHierarchy();
            for( const wxString& source : { rootSource, childSource } )
            {
                wxFFile file( source, wxS( "wb" ) );
                BOOST_REQUIRE( file.Write( wxS( "official source sentinel" ) ) );
            }

            std::vector<HISTORY_FILE_DATA> snapshot;
            schematic.SaveToHistory( project.Project().GetProjectPath(), snapshot );
            BOOST_REQUIRE_EQUAL( snapshot.size(), 2U );
            BOOST_CHECK_EQUAL( child->GetFileName(), wxS( "child.kicad_sch" ) );
            BOOST_CHECK_EQUAL( root->GetScreen()->GetFileName(), rootSource );
            BOOST_CHECK_EQUAL( root->GetScreen()->IsContentModified(), parentModified );
            std::vector<std::pair<wxString, wxString>> files;
            std::map<wxString, wxString> targets;
            wxString recoveredRoot;

            for( const HISTORY_FILE_DATA& entry : snapshot )
            {
                const wxString source = wxFileName( project.DirStr(), entry.relativePath ).GetFullPath();
                const wxString autosave = source + wxS( ".snapshot" );
                wxFFile output( autosave, wxS( "wb" ) );
                BOOST_REQUIRE_EQUAL( output.Write( entry.content.data(), entry.content.size() ), entry.content.size() );
                BOOST_REQUIRE( output.Close() );
                wxFileName target( source );

                if( existingNative )
                {
                    wxFFile sentinel( source, wxS( "wb" ) );
                    BOOST_REQUIRE( sentinel.Write( wxS( "existing native sentinel" ) ) );
                    target.SetName( target.GetName() + wxS( ".recovered" ) );
                }

                targets.emplace( source, target.GetFullPath() );
                files.emplace_back( autosave, source );

                if( wxFileName( source ).GetName() == wxS( "root" ) )
                    recoveredRoot = target.GetFullPath();
            }

            BOOST_REQUIRE( LOCAL_HISTORY::RestoreAutosaveFiles( files, targets ) );
            SCHEMATIC reopened( &project.Project() );
            SCH_IO_KICAD_SEXPR io;
            SCH_SHEET* loaded = io.LoadSchematicFile( recoveredRoot, &reopened );
            BOOST_REQUIRE( loaded );
            reopened.SetTopLevelSheets( { loaded } );
            SCH_SCREENS screens( reopened.Root() );
            BOOST_REQUIRE_EQUAL( screens.GetCount(), 2U );
            bool found = false;

            for( size_t i = 0; i < screens.GetCount(); ++i )
            {
                BOOST_CHECK( !screens.GetScreen( i )->HasLoadError() );
                for( SCH_ITEM* item : screens.GetScreen( i )->Items().OfType( SCH_TEXT_T ) )
                    found |= static_cast<SCH_TEXT*>( item )->GetText() == wxS( "unsaved child" );
            }

            BOOST_CHECK( found );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( rootSource ), "official source sentinel" );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childSource ), "official source sentinel" );
            if( existingNative )
            {
                for( const auto& [autosave, source] : files )
                    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), "existing native sentinel" );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( RecoveryRemapsOnlySheetFilenameProperties )
{
    const wxString dir = wxFileName::GetTempDir();
    const wxString source = wxFileName( dir, wxS( "root.kemi_sch" ) ).GetFullPath();
    const wxString child = wxFileName( dir, wxS( "child.kemi_sch" ) ).GetFullPath();
    const wxString recovered = wxFileName( dir, wxS( "child.recovered.kemi_sch" ) ).GetFullPath();
    std::string content = "(kemi_sch (sheet (property \"Sheetfile\" \"child.kemi_sch\"))"
                          " (text \"child.kemi_sch\") (property \"Sheetfile\" \"child.kemi_sch\")"
                          " (at 1.000001 2.33333333))\r\n";
    std::string expected = content;
    expected.replace( expected.find( "child.kemi_sch" ), std::string( "child.kemi_sch" ).size(),
                      "child.recovered.kemi_sch" );
    KEMI_FORMAT::RemapSchematicSheetPaths( content, source, source, { { child, recovered } } );
    BOOST_CHECK_EQUAL( content, expected );
}


BOOST_AUTO_TEST_SUITE_END()
