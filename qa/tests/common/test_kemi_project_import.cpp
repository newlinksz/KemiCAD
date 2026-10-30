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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <gestfich.h>
#include <json_common.h>
#include <kemi_format_profile.h>
#include <kemi_project_import.h>
#include <kemi_project_paths.h>
#include <ki_exception.h>
#include <project.h>
#include <project/project_file.h>
#include <project/project_local_settings.h>
#include <settings/settings_manager.h>
#include <wx_filename.h>

#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <vector>

#include <array>
#include <filesystem>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <map>
#include <memory>

namespace
{
using KEMI_FORMAT::PROJECT_IMPORT_TARGETS;
using DIRECTORY_SNAPSHOT = std::map<std::filesystem::path, std::string>;


DIRECTORY_SNAPSHOT snapshotDirectory( const wxString& aDirectory )
{
    DIRECTORY_SNAPSHOT result;
    const auto root = ToFsPath( aDirectory );

    for( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
    {
        result[std::filesystem::relative( entry.path(), root )] = entry.is_directory()
                ? "<directory>" : KI_TEST::LoadStringData( FromFsPath( entry.path() ) );
    }

    return result;
}


void writeDocument( const wxString& aPath, const std::string& aBytes )
{
    wxFFile file( aPath, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE_EQUAL( file.Write( aBytes.data(), aBytes.size() ), aBytes.size() );
    BOOST_REQUIRE( file.Close() );
}


void checkMarker( const wxString& aPath, const char* aDocumentType )
{
    BOOST_REQUIRE( wxFileExists( aPath ) );
    const nlohmann::json document = nlohmann::json::parse( KI_TEST::LoadStringData( aPath ) );
    BOOST_REQUIRE( document.contains( "kemicad" ) );
    BOOST_CHECK( document["kemicad"]["document_type"] == aDocumentType );
    BOOST_CHECK( document["kemicad"]["format_version"] == 1 );
    BOOST_CHECK( !document.contains( "legacy_source" ) );
}


std::map<wxString, std::string> readZipContents( const wxString& aPath )
{
    wxFFileInputStream stream( aPath );
    BOOST_REQUIRE( stream.IsOk() );
    wxZipInputStream zip( stream );
    std::map<wxString, std::string> contents;

    while( std::unique_ptr<wxZipEntry> entry{ zip.GetNextEntry() } )
    {
        BOOST_REQUIRE( !entry->IsDir() );
        wxString name = entry->GetName();
        name.Replace( wxS( "\\" ), wxS( "/" ) );
        std::string bytes;
        std::array<char, 4096> buffer;

        do
        {
            zip.Read( buffer.data(), buffer.size() );
            bytes.append( buffer.data(), zip.LastRead() );
        } while( zip.LastRead() != 0 );

        BOOST_REQUIRE( zip.IsOk() || zip.Eof() );
        BOOST_REQUIRE( contents.emplace( name, std::move( bytes ) ).second );
    }

    return contents;
}


struct KEMI_PROJECT_IMPORT_FIXTURE
{
    KEMI_PROJECT_IMPORT_FIXTURE() : directory( wxS( "kemi-project-import" ) ) {}

    PROJECT_IMPORT_TARGETS resolve( const wxString& aOutput )
    {
        return KEMI_FORMAT::ResolveProjectImportTargets(
                aOutput, directory.ChildPathStr( wxS( "source.rev1.asc" ) ), directory.PathStr() );
    }

    KI_TEST::SCOPED_TEMP_DIR directory;
};
} // namespace


BOOST_FIXTURE_TEST_SUITE( KemiProjectImport, KEMI_PROJECT_IMPORT_FIXTURE )


// PG01: Manager and CLI share native project/sheet/board names, including dotted stems.
BOOST_AUTO_TEST_CASE( ResolvesNativeTargetsWithoutWriting )
{
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( directory.PathStr() );

    for( const wxString& output : { wxString(), wxString( "source.rev1" ),
                                   wxString( "source.rev1.kemi_pro" ) } )
    {
        const PROJECT_IMPORT_TARGETS targets = resolve( output );
        BOOST_CHECK_EQUAL( targets.project, directory.ChildPathStr( wxS( "source.rev1.kemi_pro" ) ) );
        BOOST_CHECK_EQUAL( targets.local, directory.ChildPathStr( wxS( "source.rev1.kemi_prl" ) ) );
        BOOST_CHECK_EQUAL( targets.schematic, directory.ChildPathStr( wxS( "source.rev1.kemi_sch" ) ) );
        BOOST_CHECK_EQUAL( targets.board, directory.ChildPathStr( wxS( "source.rev1.kemi_pcb" ) ) );
        BOOST_CHECK_NO_THROW( KEMI_FORMAT::ValidateProjectImportTargets( targets ) );
    }

    BOOST_CHECK( snapshotDirectory( directory.PathStr() ) == before );
}


// PG02: Manager's non-GUI core creates a new project and local state without migrating the source.
BOOST_AUTO_TEST_CASE( CreatesProjectAndLocalJsonWithoutMigratingLegacyInput )
{
    const wxString sourceDirectory = directory.CreateChildDirStr( wxS( "source" ) );
    writeDocument( wxFileName( sourceDirectory, wxS( "old.kicad_pro" ) ).GetFullPath(),
                   "{\"legacy_source\": \"keep project\"}\n" );
    writeDocument( wxFileName( sourceDirectory, wxS( "old.kicad_prl" ) ).GetFullPath(),
                   "{\"legacy_source\": \"keep local state\"}\n" );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( sourceDirectory );
    const wxString targetDirectory = directory.ChildPathStr( wxS( "new-project" ) );
    const PROJECT_IMPORT_TARGETS targets = resolve(
            wxFileName( targetDirectory, wxS( "imported.kemi_pro" ) ).GetFullPath() );

    KEMI_FORMAT::CreateProjectImportFiles( targets );
    checkMarker( targets.project, "project" );
    checkMarker( targets.local, "project_local" );
    BOOST_CHECK( !wxFileExists( targets.schematic ) );
    BOOST_CHECK( !wxFileExists( targets.board ) );
    BOOST_CHECK_EQUAL( snapshotDirectory( targetDirectory ).size(), 2u );
    BOOST_CHECK( snapshotDirectory( sourceDirectory ) == sourceBefore );

    SETTINGS_MANAGER manager;
    BOOST_REQUIRE( manager.LoadProject( targets.project, false ) );
    PROJECT* project = manager.GetProject( targets.project );
    BOOST_REQUIRE( project );
    BOOST_CHECK_EQUAL( project->GetProjectFullName(), targets.project );
    auto& sheets = project->GetProjectFile().GetTopLevelSheets();
    BOOST_REQUIRE_EQUAL( sheets.size(), 1u );
    BOOST_CHECK_EQUAL( sheets.front().filename, wxS( "imported.kemi_sch" ) );
    BOOST_REQUIRE( manager.SaveProject( targets.project, project ) );
    BOOST_CHECK( KI_TEST::LoadStringData( targets.project ).find( ".kicad_sch" )
                 == std::string::npos );
    checkMarker( targets.project, "project" );

    {
        PROJECT_LOCAL_SETTINGS local( project, targets.local );
        BOOST_REQUIRE( local.LoadFromFile() );
    }

    // A missing template reference must also resolve to the native schematic on the next load.
    sheets.front().filename = wxS( "missing-template.kemi_sch" );
    BOOST_REQUIRE( manager.SaveProject( targets.project, project ) );
    writeDocument( targets.schematic, "(kemi_sch (format_version 1))\n" );
    BOOST_REQUIRE( manager.UnloadProject( project, false ) );
    BOOST_REQUIRE( manager.LoadProject( targets.project, false ) );
    project = manager.GetProject( targets.project );
    BOOST_REQUIRE( project );
    const auto& correctedSheets = project->GetProjectFile().GetTopLevelSheets();
    BOOST_REQUIRE_EQUAL( correctedSheets.size(), 1u );
    BOOST_CHECK_EQUAL( correctedSheets.front().filename, wxS( "imported.kemi_sch" ) );
    BOOST_REQUIRE( manager.SaveProject( targets.project, project ) );
    BOOST_CHECK( KI_TEST::LoadStringData( targets.project ).find( ".kicad_sch" )
                 == std::string::npos );
    BOOST_REQUIRE( manager.UnloadProject( project, false ) );
    BOOST_CHECK( snapshotDirectory( sourceDirectory ) == sourceBefore );
}


// GI-META01: Both import entry points register saved native boards before saving the project.
BOOST_AUTO_TEST_CASE( ImportedBoardsPersistRelativePathsAndPreserveExistingIds )
{
    const wxString projectDirectory = directory.CreateChildDirStr( wxS( "kemi" ) );
    const PROJECT_IMPORT_TARGETS targets = resolve(
            wxFileName( projectDirectory, wxS( "imported" ) ).GetFullPath() );
    KEMI_FORMAT::CreateProjectImportFiles( targets );
    writeDocument( targets.board, "(kemi_pcb (format_version 1))\n" );
    const wxString secondBoard = wxFileName( projectDirectory, wxS( "second.kemi_pcb" ) ).GetFullPath();
    writeDocument( secondBoard, "(kemi_pcb (format_version 1))\n" );

    SETTINGS_MANAGER manager;
    BOOST_REQUIRE( manager.LoadProject( targets.project, false ) );
    PROJECT* project = manager.GetProject( targets.project );
    BOOST_REQUIRE( project );

    KEMI_FORMAT::RegisterImportedBoards( *project, { targets.board } );
    BOOST_REQUIRE_EQUAL( project->GetProjectFile().GetBoards().size(), 1u );
    const KIID firstId = project->GetProjectFile().GetBoards().at( 0 ).first;
    KEMI_FORMAT::RegisterImportedBoards( *project,
                                       { wxS( "imported.kemi_pcb" ), secondBoard, secondBoard } );
    const auto expected = project->GetProjectFile().GetBoards();
    BOOST_REQUIRE_EQUAL( expected.size(), 2u );
    BOOST_CHECK( expected.front().first == firstId );
    BOOST_CHECK_EQUAL( expected.front().second, wxS( "imported.kemi_pcb" ) );
    BOOST_CHECK_EQUAL( expected.back().second, wxS( "second.kemi_pcb" ) );
    BOOST_CHECK( expected.front().first != expected.back().first );

    BOOST_REQUIRE( manager.SaveProject( project->GetProjectFullName(), project ) );
    BOOST_REQUIRE( manager.UnloadProject( project, false ) );
    BOOST_REQUIRE( manager.LoadProject( targets.project, false ) );
    project = manager.GetProject( targets.project );
    BOOST_REQUIRE( project );
    BOOST_CHECK( project->GetProjectFile().GetBoards() == expected );
    checkMarker( targets.project, "project" );
    BOOST_REQUIRE( manager.UnloadProject( project, false ) );
}


BOOST_AUTO_TEST_CASE( ImportedBoardRegistrationRejectsInvalidBatchWithoutChangingProject )
{
    const wxString projectDirectory = directory.CreateChildDirStr( wxS( "kemi" ) );
    const PROJECT_IMPORT_TARGETS targets = resolve(
            wxFileName( projectDirectory, wxS( "imported" ) ).GetFullPath() );
    KEMI_FORMAT::CreateProjectImportFiles( targets );
    writeDocument( targets.board, "(kemi_pcb (format_version 1))\n" );
    const wxString outsideBoard = directory.ChildPathStr( wxS( "outside.kemi_pcb" ) );
    writeDocument( outsideBoard, "(kemi_pcb (format_version 1))\n" );

    SETTINGS_MANAGER manager;
    BOOST_REQUIRE( manager.LoadProject( targets.project, false ) );
    PROJECT* project = manager.GetProject( targets.project );
    BOOST_REQUIRE( project );
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( directory.PathStr() );

    KEMI_FORMAT::RegisterImportedBoards( *project, {} );
    BOOST_CHECK( project->GetProjectFile().GetBoards().empty() );

    for( const wxString& invalid : { wxString( "missing.kemi_pcb" ),
                                     wxString( "imported.kicad_pcb" ), outsideBoard } )
    {
        BOOST_TEST_CONTEXT( invalid )
        {
            BOOST_CHECK_THROW( KEMI_FORMAT::RegisterImportedBoards( *project,
                                                                   { targets.board, invalid } ), IO_ERROR );
            BOOST_CHECK( project->GetProjectFile().GetBoards().empty() );
            BOOST_CHECK( snapshotDirectory( directory.PathStr() ) == before );
        }
    }

    BOOST_REQUIRE( manager.UnloadProject( project, false ) );
}


// GI01: Imported roots must remain visible without admitting unrelated same-named files.
BOOST_AUTO_TEST_CASE( TopLevelSchematicMatchesDeclaredProjectRelativePaths )
{
    const wxString projectDirectory = directory.CreateChildDirStr( wxS( "kemi" ) );
    const PROJECT_IMPORT_TARGETS targets = resolve(
            wxFileName( projectDirectory, wxS( "imported" ) ).GetFullPath() );
    KEMI_FORMAT::CreateProjectImportFiles( targets );

    SETTINGS_MANAGER manager;
    BOOST_REQUIRE( manager.LoadProject( targets.project, false ) );
    PROJECT* project = manager.GetProject( targets.project );
    BOOST_REQUIRE( project );
    BOOST_CHECK( !wxDirExists( wxFileName( projectDirectory, wxS( ".git" ) ).GetFullPath() ) );

    const wxString page = wxString::FromUTF8( "02-更改记录.kemi_sch" );
    writeDocument( targets.schematic, "(kemi_sch (format_version 1))\n" );
    writeDocument( wxFileName( projectDirectory, page ).GetFullPath(),
                   "(kemi_sch (format_version 1))\n" );
    writeDocument( wxFileName( projectDirectory, wxS( "official.kicad_sch" ) ).GetFullPath(),
                   "(kicad_sch)\n" );
    auto& sheets = project->GetProjectFile().GetTopLevelSheets();
    sheets.emplace_back( KIID(), wxS( "Imported page" ), page );
    sheets.emplace_back( KIID(), wxS( "Official root" ), wxS( "official.kicad_sch" ) );
    sheets.emplace_back( KIID(), wxS( "Empty entry" ), wxString() );
    BOOST_REQUIRE( manager.SaveProject( targets.project, project ) );
    BOOST_REQUIRE( manager.UnloadProject( project, false ) );
    BOOST_REQUIRE( manager.LoadProject( targets.project, false ) );
    project = manager.GetProject( targets.project );
    BOOST_REQUIRE( project );

    const wxString pagePath = wxFileName( projectDirectory, page ).GetFullPath();
    BOOST_CHECK( KEMI_FORMAT::IsProjectTopLevelSchematic( *project, pagePath ) );
    BOOST_CHECK( KEMI_FORMAT::IsProjectTopLevelSchematic( *project, page ) );
    BOOST_CHECK( KEMI_FORMAT::IsProjectTopLevelSchematic(
            *project, wxS( "./official.kicad_sch" ) ) );
    BOOST_CHECK( !KEMI_FORMAT::IsProjectTopLevelSchematic(
            *project, wxFileName( directory.PathStr(), page ).GetFullPath() ) );
    BOOST_CHECK( !KEMI_FORMAT::IsProjectTopLevelSchematic(
            *project, wxS( "kemi/" ) + page ) );
    BOOST_CHECK( !KEMI_FORMAT::IsProjectTopLevelSchematic(
            *project, wxS( "unrelated.kemi_sch" ) ) );
    BOOST_CHECK( !KEMI_FORMAT::IsProjectTopLevelSchematic( *project, wxString() ) );
#ifdef _WIN32
    BOOST_CHECK( KEMI_FORMAT::IsProjectTopLevelSchematic( *project, pagePath.Upper() ) );
#endif
    BOOST_REQUIRE( manager.UnloadProject( project, false ) );
}


// GI-REOPEN01: Manager dispatch and the editor must resolve the same native project and roots.
BOOST_AUTO_TEST_CASE( ReopenNativeProjectKeepsAllDeclaredTopLevelPages )
{
    const wxString projectDirectory = directory.CreateChildDirStr( wxS( "reopen-kemi" ) );
    const PROJECT_IMPORT_TARGETS targets = resolve(
            wxFileName( projectDirectory, wxS( "project.rev1" ) ).GetFullPath() );
    KEMI_FORMAT::CreateProjectImportFiles( targets );

    SETTINGS_MANAGER manager;
    BOOST_REQUIRE( manager.LoadProject( targets.project, false ) );
    PROJECT* project = manager.GetProject( targets.project );
    BOOST_REQUIRE( project );
    auto& sheets = project->GetProjectFile().GetTopLevelSheets();
    sheets.clear();

    const std::vector<wxString> filenames = {
        wxS( "project.rev1.kemi_sch" ), wxString::FromUTF8( "02-更改记录.kemi_sch" ),
        wxS( "03-POWER.kemi_sch" ), wxS( "04-USBHUB.kemi_sch" ), wxS( "05-TestMCU.kemi_sch" )
    };

    for( const wxString& filename : filenames )
    {
        writeDocument( wxFileName( projectDirectory, filename ).GetFullPath(),
                       "(kemi_sch (format_version 1))\n" );
        sheets.emplace_back( KIID(), wxFileName( filename ).GetName(), filename );
    }

    BOOST_REQUIRE( manager.SaveProject( targets.project, project ) );
    BOOST_REQUIRE( manager.UnloadProject( project, false ) );

    const wxString root = KEMI_FORMAT::RelatedDocumentPath(
            KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::DOC::SCHEMATIC, targets.project );
    BOOST_CHECK_EQUAL( root, targets.schematic );
    const wxString reopenedProject = KEMI_FORMAT::RelatedDocumentPath(
            KEMI_FORMAT::DOC::SCHEMATIC, KEMI_FORMAT::DOC::PROJECT, root );
    BOOST_CHECK_EQUAL( reopenedProject, targets.project );
    BOOST_REQUIRE( manager.LoadProject( reopenedProject, false ) );
    project = manager.GetProject( reopenedProject );
    BOOST_REQUIRE( project );
    BOOST_REQUIRE_EQUAL( project->GetProjectFile().GetTopLevelSheets().size(), filenames.size() );

    for( const wxString& filename : filenames )
    {
        const wxString page = wxFileName( projectDirectory, filename ).GetFullPath();
        BOOST_CHECK( KEMI_FORMAT::IsProjectTopLevelSchematic( *project, page ) );
    }

    BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath(
            KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::DOC::PCB, reopenedProject ), targets.board );
    BOOST_CHECK( !KEMI_FORMAT::IsProjectTopLevelSchematic(
            *project, wxFileName( projectDirectory, wxS( "unrelated.kemi_sch" ) ).GetFullPath() ) );
    BOOST_REQUIRE( manager.UnloadProject( project, false ) );
    for( const auto& item : snapshotDirectory( projectDirectory ) )
        BOOST_CHECK( !FromFsPath( item.first ).Lower().Contains( wxS( ".kicad_" ) ) );
}


BOOST_AUTO_TEST_CASE( RelatedDocumentPathsPreserveOfficialAndNativeFlavor )
{
    using KEMI_FORMAT::DOC;
    const wxString stem = directory.ChildPathStr( wxS( "board.rev1" ) );

    for( const wxString& extension : { wxString( "kemi_pro" ), wxString( "KEMI_PRO" ) } )
    {
        const wxString project = stem + wxS( "." ) + extension;
        BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath( DOC::PROJECT, DOC::SCHEMATIC, project ),
                           stem + wxS( ".kemi_sch" ) );
        BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath( DOC::PROJECT, DOC::PCB, project ),
                           stem + wxS( ".kemi_pcb" ) );
    }

    for( const wxString& extension : { wxString( "kicad_pro" ), wxString( "pro" ) } )
    {
        const wxString project = stem + wxS( "." ) + extension;
        BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath( DOC::PROJECT, DOC::SCHEMATIC, project ),
                           stem + wxS( ".kicad_sch" ) );
        BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath( DOC::PROJECT, DOC::PCB, project ),
                           stem + wxS( ".kicad_pcb" ) );
    }

    BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath(
            DOC::SCHEMATIC, DOC::PROJECT, stem + wxS( ".kemi_sch" ) ), stem + wxS( ".kemi_pro" ) );
    BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath(
            DOC::SCHEMATIC, DOC::PROJECT, stem + wxS( ".kicad_sch" ) ), stem + wxS( ".kicad_pro" ) );
    BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath(
            DOC::SCHEMATIC, DOC::PCB, stem + wxS( ".kemi_sch" ) ), stem + wxS( ".kemi_pcb" ) );
    BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath(
            DOC::PCB, DOC::SCHEMATIC, stem + wxS( ".kemi_pcb" ) ), stem + wxS( ".kemi_sch" ) );
    BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath(
            DOC::SCHEMATIC, DOC::PCB, stem + wxS( ".kicad_sch" ) ), stem + wxS( ".kicad_pcb" ) );
    BOOST_CHECK_EQUAL( KEMI_FORMAT::RelatedDocumentPath(
            DOC::PCB, DOC::SCHEMATIC, stem + wxS( ".kicad_pcb" ) ), stem + wxS( ".kicad_sch" ) );
    BOOST_CHECK( KEMI_FORMAT::RelatedDocumentPath( DOC::PROJECT, DOC::SCHEMATIC, wxString() ).IsEmpty() );
}


BOOST_AUTO_TEST_CASE( NativePageNavigationUsesUtf8WithoutLocaleConversion )
{
    const wxString page = directory.ChildPathStr( wxString::FromUTF8( "02-更改记录.kemi_sch" ) );
    const std::string packet = KEMI_FORMAT::NavigationPathToUtf8( page );
    const std::string expectedName = "02-\xE6\x9B\xB4\xE6\x94\xB9\xE8\xAE\xB0\xE5\xBD\x95.kemi_sch";
    BOOST_CHECK( packet.find( expectedName ) != std::string::npos );
    const wxString received = KEMI_FORMAT::NavigationPathFromUtf8( packet );
    BOOST_CHECK_EQUAL( received, page );
    BOOST_CHECK( wxFileName( received ).SameAs( wxFileName( page ) ) );
}


// PG03: Reject all official target spellings, including staging names, before even mkdir.
BOOST_AUTO_TEST_CASE( RejectsOfficialTargetsWithoutFilesystemChanges )
{
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( directory.PathStr() );
    const wxString outputDirectory = directory.ChildPathStr( wxS( "must-not-exist" ) );

    for( const char* extension : { "kicad_pro", "kicad_prl", "kicad_sch", "kicad_pcb",
                                  "kicad_sym", "kicad_mod", "kicad_dru", "kicad_wks",
                                  "kicad_jobset", "kicad_pro.tmp", "KICAD_PCB.tmp" } )
    {
        const wxString output = wxFileName( outputDirectory,
                                            wxS( "blocked." ) + wxString( extension ) ).GetFullPath();
        BOOST_TEST_CONTEXT( extension )
        {
            BOOST_CHECK_THROW( resolve( output ), IO_ERROR );
            BOOST_CHECK( snapshotDirectory( directory.PathStr() ) == before );
        }
    }

    const PROJECT_IMPORT_TARGETS valid = resolve(
            wxFileName( outputDirectory, wxS( "valid" ) ).GetFullPath() );
    const std::array<wxString PROJECT_IMPORT_TARGETS::*, 4> members = {
        &PROJECT_IMPORT_TARGETS::project, &PROJECT_IMPORT_TARGETS::local,
        &PROJECT_IMPORT_TARGETS::schematic, &PROJECT_IMPORT_TARGETS::board
    };

    // A caller cannot bypass early checks by mutating an already-resolved target structure.
    for( auto member : members )
    {
        PROJECT_IMPORT_TARGETS invalid = valid;
        invalid.*member = wxFileName( outputDirectory, wxS( "blocked.kicad_pro" ) ).GetFullPath();
        BOOST_CHECK_THROW( KEMI_FORMAT::CreateProjectImportFiles( invalid ), IO_ERROR );
        BOOST_CHECK( snapshotDirectory( directory.PathStr() ) == before );
    }
}


// PG04: Neither a Kemi output nor its same-name official counterpart may be overwritten.
BOOST_AUTO_TEST_CASE( RejectsExistingTargetsAndOfficialCounterpartsWithoutChanges )
{
    for( const char* extension : { "kemi_pro", "kicad_pro", "kemi_prl", "kicad_prl",
                                  "kemi_sch", "kicad_sch", "kemi_pcb", "kicad_pcb" } )
    {
        const wxString outputDirectory = directory.CreateChildDirStr( wxString( extension ) );
        const wxString existing = wxFileName( outputDirectory,
                                              wxS( "existing." ) + wxString( extension ) ).GetFullPath();
        writeDocument( existing, "{\"legacy_source\": \"must remain byte-identical\"}\n" );
        const DIRECTORY_SNAPSHOT before = snapshotDirectory( directory.PathStr() );
        const PROJECT_IMPORT_TARGETS targets = resolve(
                wxFileName( outputDirectory, wxS( "existing.kemi_pro" ) ).GetFullPath() );

        BOOST_CHECK_THROW( KEMI_FORMAT::ValidateProjectImportTargets( targets ), IO_ERROR );
        BOOST_CHECK_THROW( KEMI_FORMAT::CreateProjectImportFiles( targets ), IO_ERROR );
        BOOST_CHECK( snapshotDirectory( directory.PathStr() ) == before );
    }
}



BOOST_AUTO_TEST_CASE( WriteImportZipBesideKemiLeaf )
{
    KI_TEST::SCOPED_TEMP_DIR root( wxS( "kemi-import-zip" ) );

    // GUI imports can use the project name as the directory, while CLI callers choose any leaf.
    for( const wxString& leaf : { wxString( "kemi" ), wxString( "demo" ), wxString( "arbitrary" ) } )
    {
        BOOST_TEST_CONTEXT( leaf )
        {
            const wxString caseDir = root.CreateChildDirStr( leaf + wxS( "-case" ) );
            const wxString projectDir = wxFileName( caseDir, leaf ).GetFullPath();
            BOOST_REQUIRE( wxFileName::Mkdir( projectDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
            const wxString footprintDir = wxFileName( projectDir, wxS( "demo.pretty" ) ).GetFullPath();
            BOOST_REQUIRE( wxFileName::Mkdir( footprintDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

            nlohmann::json project = nlohmann::json::object();
            KEMI_FORMAT::StampJsonMarker( project, KEMI_FORMAT::DOC::PROJECT );
            project["meta"] = { { "filename", "demo.kemi_pro" }, { "version", 1 } };
            nlohmann::json local = nlohmann::json::object();
            KEMI_FORMAT::StampJsonMarker( local, KEMI_FORMAT::DOC::PROJECT_LOCAL );
            const std::map<wxString, std::string> expected = {
                { wxS( "demo.kemi_pro" ), project.dump( 0 ) + "\n" },
                { wxS( "demo.kemi_prl" ), local.dump( 0 ) + "\n" },
                { wxS( "demo.kemi_sch" ), "(kemi_sch (format_version 1))\n" },
                { wxS( "demo.kemi_pcb" ), "(kemi_pcb (format_version 1))\n" },
                { wxS( "demo.kemi_sym" ), "(kemi_symbol_lib (format_version 1))\n" },
                { wxS( "demo.pretty/pad.kemi_mod" ), "(kemi_footprint (format_version 1))\n" },
                { wxS( "sym-lib-table" ),
                  "(sym_lib_table (lib (name demo)(type KiCad)(uri ${KIPRJMOD}/demo.kemi_sym)))\n" },
                { wxS( "fp-lib-table" ),
                  "(fp_lib_table (lib (name demo)(type KiCad)(uri ${KIPRJMOD}/demo.pretty)))\n" }
            };

            for( const auto& [name, bytes] : expected )
                writeDocument( wxFileName( projectDir + wxS( "/" ) + name ).GetFullPath(), bytes );

            // Existing official files stay on disk, but neither they nor an older zip enter the archive.
            const wxString official = wxFileName( projectDir, wxS( "leak.kicad_pro" ) ).GetFullPath();
            writeDocument( official, "{}\n" );
            const wxString oldZip = wxFileName( projectDir, wxS( "old.zip" ) ).GetFullPath();
            writeDocument( oldZip, "OLDER-ARCHIVE\n" );
            const wxString projectPath = wxFileName( projectDir, wxS( "demo.kemi_pro" ) ).GetFullPath();

            BOOST_REQUIRE( KEMI_FORMAT::WriteImportZip( projectPath, nullptr ) );

            const wxString zipPath = wxFileName( caseDir, wxS( "demo.zip" ) ).GetFullPath();
            BOOST_REQUIRE( wxFileExists( zipPath ) );
            BOOST_CHECK( !wxFileExists( wxFileName( projectDir, wxS( "demo.zip" ) ).GetFullPath() ) );
            // Entries are project-relative with intact library bytes, without an enclosing directory.
            BOOST_CHECK( readZipContents( zipPath ) == expected );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( official ), "{}\n" );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( oldZip ), "OLDER-ARCHIVE\n" );
        }
    }
}


BOOST_AUTO_TEST_CASE( WriteImportZipRefusesProtectedSourceCollision )
{
    KI_TEST::SCOPED_TEMP_DIR root( wxS( "kemi-import-zip-protect" ) );

    for( const wxString& leaf : { wxString( "kemi" ), wxString( "demo" ), wxString( "arbitrary" ) } )
    {
        BOOST_TEST_CONTEXT( leaf )
        {
            const wxString caseDir = root.CreateChildDirStr( leaf + wxS( "-case" ) );
            const wxString projectDir = wxFileName( caseDir, leaf ).GetFullPath();
            BOOST_REQUIRE( wxFileName::Mkdir( projectDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
            const wxString projectPath = wxFileName( projectDir, wxS( "demo.kemi_pro" ) ).GetFullPath();
            nlohmann::json project = nlohmann::json::object();
            KEMI_FORMAT::StampJsonMarker( project, KEMI_FORMAT::DOC::PROJECT );
            project["meta"] = { { "filename", "demo.kemi_pro" }, { "version", 1 } };
            writeDocument( projectPath, project.dump( 0 ) + "\n" );

            const wxString sourceZip = wxFileName( caseDir, wxS( "demo.zip" ) ).GetFullPath();
            const std::string original = "SOURCE-ARCHIVE-BYTES-MUST-NOT-CHANGE\n";
            writeDocument( sourceZip, original );
            const DIRECTORY_SNAPSHOT before = snapshotDirectory( caseDir );

            BOOST_CHECK( !KEMI_FORMAT::WriteImportZip( projectPath, nullptr,
                                                     std::vector<wxString>{ sourceZip } ) );
            BOOST_CHECK( snapshotDirectory( caseDir ) == before );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( sourceZip ), original );
        }
    }
}


BOOST_AUTO_TEST_CASE( WriteImportZipRefusesSecondProtectedSourceCollision )
{
    // CLI may pass several inputs; a later input that lands on the zip path must also be protected.
    KI_TEST::SCOPED_TEMP_DIR root( wxS( "kemi-import-zip-protect2" ) );
    const wxString caseDir = root.CreateChildDirStr( wxS( "case1" ) );
    const wxString kemiDir = wxFileName( caseDir, wxS( "kemi" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( kemiDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    const wxString projectPath = wxFileName( kemiDir, wxS( "demo.kemi_pro" ) ).GetFullPath();
    {
        nlohmann::json doc = nlohmann::json::object();
        KEMI_FORMAT::StampJsonMarker( doc, KEMI_FORMAT::DOC::PROJECT );
        doc["meta"] = { { "filename", "demo.kemi_pro" }, { "version", 1 } };
        wxFFile file( projectPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        const std::string bytes = doc.dump( 0 ) + "\n";
        BOOST_REQUIRE_EQUAL( file.Write( bytes.data(), bytes.size() ), bytes.size() );
        BOOST_REQUIRE( file.Close() );
    }

    const wxString schInput = wxFileName( caseDir, wxS( "board.sch" ) ).GetFullPath();
    {
        wxFFile file( schInput, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        const std::string bytes = "SCH-INPUT\n";
        BOOST_REQUIRE_EQUAL( file.Write( bytes.data(), bytes.size() ), bytes.size() );
        BOOST_REQUIRE( file.Close() );
    }

    const wxString sourceZip = wxFileName( caseDir, wxS( "demo.zip" ) ).GetFullPath();
    const std::string original = "SECOND-INPUT-ZIP-BYTES-MUST-NOT-CHANGE\n";
    {
        wxFFile file( sourceZip, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE_EQUAL( file.Write( original.data(), original.size() ), original.size() );
        BOOST_REQUIRE( file.Close() );
    }

    const std::vector<wxString> protectedPaths{ schInput, sourceZip };
    BOOST_CHECK( !KEMI_FORMAT::WriteImportZip( projectPath, nullptr, protectedPaths ) );

    wxFFile verify( sourceZip, wxS( "rb" ) );
    BOOST_REQUIRE( verify.IsOpened() );
    wxString kept;
    BOOST_REQUIRE( verify.ReadAll( &kept ) );
    BOOST_CHECK_EQUAL( kept.ToStdString(), original );
}


BOOST_AUTO_TEST_CASE( WriteImportZipRefusesJunctionAliasCollision )
{
#if defined( _WIN32 )
    // Directory junction: alias\demo.zip is the same entity as case1\demo.zip.
    KI_TEST::SCOPED_TEMP_DIR root( wxS( "kemi-import-zip-junction" ) );
    const wxString caseDir = root.CreateChildDirStr( wxS( "case1" ) );
    const wxString kemiDir = wxFileName( caseDir, wxS( "kemi" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( kemiDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    const wxString projectPath = wxFileName( kemiDir, wxS( "demo.kemi_pro" ) ).GetFullPath();
    {
        nlohmann::json doc = nlohmann::json::object();
        KEMI_FORMAT::StampJsonMarker( doc, KEMI_FORMAT::DOC::PROJECT );
        doc["meta"] = { { "filename", "demo.kemi_pro" }, { "version", 1 } };
        wxFFile file( projectPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        const std::string bytes = doc.dump( 0 ) + "\n";
        BOOST_REQUIRE_EQUAL( file.Write( bytes.data(), bytes.size() ), bytes.size() );
        BOOST_REQUIRE( file.Close() );
    }

    const wxString sourceZip = wxFileName( caseDir, wxS( "demo.zip" ) ).GetFullPath();
    const std::string original = "JUNCTION-ALIAS-ZIP-BYTES-MUST-NOT-CHANGE\n";
    {
        wxFFile file( sourceZip, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE_EQUAL( file.Write( original.data(), original.size() ), original.size() );
        BOOST_REQUIRE( file.Close() );
    }

    const wxString aliasDir = wxFileName( root.PathStr(), wxS( "alias" ) ).GetFullPath();
    const wxString cmd = wxString::Format( wxS( "cmd /c mklink /J \"%s\" \"%s\"" ), aliasDir, caseDir );
    BOOST_REQUIRE_EQUAL( wxExecute( cmd, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE ), 0 );
    BOOST_REQUIRE( wxDirExists( aliasDir ) );

    const wxString aliasZip = wxFileName( aliasDir, wxS( "demo.zip" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileExists( aliasZip ) );

    wxFileName aliasFn( aliasZip );
    wxFileName sourceFn( sourceZip );
    BOOST_REQUIRE( aliasFn.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE ) );
    BOOST_REQUIRE( sourceFn.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE ) );
    // Preconditions: distinct path strings, same filesystem entity via the junction.
    BOOST_REQUIRE( !aliasFn.GetFullPath().IsSameAs( sourceFn.GetFullPath(),
                                                     wxFileName::IsCaseSensitive() ) );

    BOOST_CHECK( !KEMI_FORMAT::WriteImportZip( projectPath, nullptr,
                                                std::vector<wxString>{ aliasZip } ) );

    wxFFile verify( sourceZip, wxS( "rb" ) );
    BOOST_REQUIRE( verify.IsOpened() );
    wxString kept;
    BOOST_REQUIRE( verify.ReadAll( &kept ) );
    BOOST_CHECK_EQUAL( kept.ToStdString(), original );
#else
    BOOST_TEST_MESSAGE( "Junction alias collision covered on Windows only." );
#endif
}


BOOST_AUTO_TEST_CASE( WriteImportZipRefusesSymlinkParentDotDotCollision )
{
    // POSIX (and Windows when the OS resolves symlink+..) must not lexically fold
    // "alias/link/../demo.zip" into "alias/demo.zip" before the identity check.
    KI_TEST::SCOPED_TEMP_DIR root( wxS( "kemi-import-zip-symlink-dotdot" ) );
    const wxString caseDir = root.CreateChildDirStr( wxS( "case1" ) );
    const wxString subDir = wxFileName( caseDir, wxS( "sub" ) ).GetFullPath();
    const wxString kemiDir = wxFileName( caseDir, wxS( "kemi" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( subDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    BOOST_REQUIRE( wxFileName::Mkdir( kemiDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    const wxString projectPath = wxFileName( kemiDir, wxS( "demo.kemi_pro" ) ).GetFullPath();
    {
        nlohmann::json doc = nlohmann::json::object();
        KEMI_FORMAT::StampJsonMarker( doc, KEMI_FORMAT::DOC::PROJECT );
        doc["meta"] = { { "filename", "demo.kemi_pro" }, { "version", 1 } };
        wxFFile file( projectPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        const std::string bytes = doc.dump( 0 ) + "\n";
        BOOST_REQUIRE_EQUAL( file.Write( bytes.data(), bytes.size() ), bytes.size() );
    }

    const wxString sourceZip = wxFileName( caseDir, wxS( "demo.zip" ) ).GetFullPath();
    const std::string original = "SOURCE_ARCHIVE_BYTES_SYMLINK_DOTDOT\n";
    {
        wxFFile file( sourceZip, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE_EQUAL( file.Write( original.data(), original.size() ), original.size() );
    }

    const wxString aliasDir = root.CreateChildDirStr( wxS( "alias" ) );
    const wxString linkPath = wxFileName( aliasDir, wxS( "link" ) ).GetFullPath();

#if defined( _WIN32 )
    {
        wxString cmd = wxString::Format( wxS( "cmd /c mklink /D \"%s\" \"%s\"" ), linkPath, subDir );
        long rc = wxExecute( cmd, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE );

        if( rc != 0 )
        {
            cmd = wxString::Format( wxS( "cmd /c mklink /J \"%s\" \"%s\"" ), linkPath, subDir );
            rc = wxExecute( cmd, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE );
        }

        if( rc != 0 )
        {
            BOOST_TEST_MESSAGE( "Could not create directory symlink/junction; skipping." );
            return;
        }
    }
#else
    if( ::symlink( static_cast<const char*>( subDir.utf8_str() ),
                   static_cast<const char*>( linkPath.utf8_str() ) ) != 0 )
    {
        BOOST_TEST_MESSAGE( "symlink() failed; skipping." );
        return;
    }
#endif

    wxFileName oddFn;
    oddFn.AssignDir( linkPath );
    oddFn.AppendDir( wxS( ".." ) );
    oddFn.SetFullName( wxS( "demo.zip" ) );
    const wxString oddProtected = oddFn.GetFullPath();

    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    const fs::path oddFs( oddProtected.ToStdWstring() );
    const fs::path srcFs( sourceZip.ToStdWstring() );
#else
    const fs::path oddFs( static_cast<const char*>( oddProtected.utf8_str() ) );
    const fs::path srcFs( static_cast<const char*>( sourceZip.utf8_str() ) );
#endif

    if( !fs::exists( oddFs, ec ) || !fs::equivalent( oddFs, srcFs, ec ) )
    {
        BOOST_TEST_MESSAGE( "OS does not resolve symlink+.. to the source zip; skipping." );
        return;
    }

    // Lexical DOTS fold would wrongly compare against alias/demo.zip (must NOT be required).
    wxFileName lexical( oddProtected );
    BOOST_REQUIRE( lexical.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE ) );
    BOOST_REQUIRE( !lexical.GetFullPath().IsSameAs( sourceZip, wxFileName::IsCaseSensitive() ) );

    BOOST_CHECK( !KEMI_FORMAT::WriteImportZip( projectPath, nullptr,
                                               std::vector<wxString>{ oddProtected } ) );

    wxFFile verify( sourceZip, wxS( "rb" ) );
    BOOST_REQUIRE( verify.IsOpened() );
    wxString kept;
    BOOST_REQUIRE( verify.ReadAll( &kept ) );
    BOOST_CHECK_EQUAL( kept.ToStdString(), original );
}

BOOST_AUTO_TEST_SUITE_END()
