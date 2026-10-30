/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
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

/**
 * @file
 * Tests for PROJECT_TEMPLATE class.
 */

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>
#include <project_template.h>
#include <kemi_format_profile.h>
#include <nlohmann/json.hpp>
#include <map>
#include <iterator>
#include <settings/settings_manager.h>

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;


class PROJECT_TEMPLATE_TEST_FIXTURE
{
public:
    PROJECT_TEMPLATE_TEST_FIXTURE() : m_directory( wxS( "kicad_template_test" ) )
    {
        m_originalDir = fs::current_path();
        m_tempDir = m_directory.Path();
    }

    ~PROJECT_TEMPLATE_TEST_FIXTURE()
    {
        fs::current_path( m_originalDir );
    }

    void CreateTemplateStructure( const std::string& templateName,
                                  const std::vector<std::string>& subdirs,
                                  const std::vector<std::string>& files )
    {
        fs::path templatePath = m_tempDir / templateName;
        fs::create_directories( templatePath );

        fs::path metaPath = templatePath / "meta";
        fs::create_directories( metaPath );

        std::ofstream infoFile( ( metaPath / "info.html" ).string() );
        infoFile << "<html><head><title>Test Template</title></head><body></body></html>";
        infoFile.close();

        for( const auto& subdir : subdirs )
        {
            fs::create_directories( templatePath / subdir );
        }

        for( const auto& file : files )
        {
            fs::path filePath = templatePath / file;
            fs::create_directories( filePath.parent_path() );
            std::ofstream f( filePath.string() );
            if( filePath.extension() == ".kicad_pro" || filePath.extension() == ".kicad_prl"
                || filePath.extension() == ".kicad_jobset" )
                f << "{\"meta\":{\"version\":1}}";
            else if( filePath.extension() == ".kicad_sch" )
                f << "(kicad_sch (version 20250114) (generator eeschema)"
                     " (uuid 00000000-0000-0000-0000-000000000001) (paper \"A4\") (lib_symbols))";
            else if( filePath.extension() == ".kicad_pcb" )
                f << "(kicad_pcb (version 20241229) (generator pcbnew))";
            else if( filePath.extension() == ".kicad_sym" )
                f << "(kicad_symbol_lib (version 20241209) (generator kicad_symbol_editor) (symbol \"Part\"))";
            else if( filePath.extension() == ".kicad_mod" )
                f << "(footprint \"Pad\" (version 20241229) (generator pcbnew))";
            else if( filePath.extension() == ".kicad_wks" )
                f << "(kicad_wks (version 20231120) (generator pl_editor))";
            else if( filePath.extension() == ".kicad_dru" )
                f << "(version 1)\n(rule \"Gap\" (constraint clearance (min 0.2mm)))";
            else
                f << "test content";

            f.close();
        }
    }

    fs::path m_originalDir;
    fs::path m_tempDir;

private:
    KI_TEST::SCOPED_TEMP_DIR m_directory;
};


BOOST_FIXTURE_TEST_SUITE( ProjectTemplate, PROJECT_TEMPLATE_TEST_FIXTURE )


BOOST_AUTO_TEST_CASE( DirectoriesRenamedCorrectly )
{
    // Create a template with subdirectories that should be renamed
    CreateTemplateStructure(
            "issue22289",
            { "issue22289-dir", "issue22289-backups", "other-dir" },
            { "issue22289.kicad_pro", "issue22289.kicad_sch", "issue22289-dir/test.kicad_sym" } );

    fs::path templatePath = m_tempDir / "issue22289";
    fs::path destPath = m_tempDir / "myproject";
    fs::create_directories( destPath );

    PROJECT_TEMPLATE tmpl( wxString::FromUTF8( templatePath.string() ) );

    // GetDestinationFiles expects a wxFileName with the project file path
    wxFileName newProjectPath;
    newProjectPath.SetPath( wxString::FromUTF8( destPath.string() ) );
    newProjectPath.SetName( wxS( "myproject" ) );
    newProjectPath.SetExt( wxS( "kemi_pro" ) );

    std::vector<wxFileName> destFiles;
    tmpl.GetDestinationFiles( newProjectPath, destFiles );

    bool foundRenamedDir = false;
    bool foundRenamedFile = false;
    bool foundOtherDir = false;

    for( const wxFileName& destFile : destFiles )
    {
        wxString fullPath = destFile.GetFullPath();

        if( fullPath.Contains( wxS( "myproject-dir" ) ) )
            foundRenamedDir = true;

        if( fullPath.Contains( wxS( "issue22289-dir" ) ) )
            BOOST_FAIL( "Directory should have been renamed from issue22289-dir to myproject-dir" );

        if( fullPath.Contains( wxS( "other-dir" ) ) )
            foundOtherDir = true;

        if( destFile.GetName() == wxS( "myproject" ) && destFile.GetExt() == wxS( "kemi_pro" ) )
            foundRenamedFile = true;
    }

    BOOST_CHECK_MESSAGE( foundRenamedDir, "Should find myproject-dir in destination files" );
    BOOST_CHECK_MESSAGE( foundRenamedFile, "Should find myproject.kemi_pro in destination files" );
    BOOST_CHECK_MESSAGE( foundOtherDir, "Should preserve other-dir (not matching template name)" );
}


BOOST_AUTO_TEST_CASE( CreateProjectRenamesDirectories )
{
    CreateTemplateStructure(
            "testtemplate",
            { "testtemplate-lib", "testtemplate" },
            { "testtemplate.kicad_pro", "testtemplate-lib/component.kicad_sym",
              "testtemplate/nested.txt" } );

    fs::path templatePath = m_tempDir / "testtemplate";
    fs::path destPath = m_tempDir / "newproject";
    fs::create_directories( destPath );

    PROJECT_TEMPLATE tmpl( wxString::FromUTF8( templatePath.string() ) );

    // CreateProject expects a wxFileName with the project file path (including extension)
    wxFileName newProjectPath;
    newProjectPath.SetPath( wxString::FromUTF8( destPath.string() ) );
    newProjectPath.SetName( wxS( "newproject" ) );
    newProjectPath.SetExt( wxS( "kemi_pro" ) );

    wxString errorMsg;
    bool     result = tmpl.CreateProject( newProjectPath, &errorMsg );

    BOOST_CHECK_MESSAGE( result, "CreateProject should succeed: " + errorMsg.ToStdString() );

    BOOST_CHECK( fs::exists( destPath / "newproject.kemi_pro" ) );
    BOOST_CHECK( fs::exists( destPath / "newproject-lib" ) );
    BOOST_CHECK( fs::exists( destPath / "newproject-lib" / "component.kemi_sym" ) );

    BOOST_CHECK_MESSAGE( !fs::exists( destPath / "testtemplate-lib" ),
                         "Old directory name should not exist" );
}


BOOST_AUTO_TEST_CASE( ExactMatchDirectoryRenamed )
{
    // Test that a directory exactly matching the template name is renamed
    CreateTemplateStructure( "mytemplate", { "mytemplate" },
                             { "mytemplate.kicad_pro", "mytemplate/subfile.txt" } );

    fs::path templatePath = m_tempDir / "mytemplate";
    fs::path destPath = m_tempDir / "finalproject";
    fs::create_directories( destPath );

    PROJECT_TEMPLATE tmpl( wxString::FromUTF8( templatePath.string() ) );

    // GetDestinationFiles expects a wxFileName with the project file path
    wxFileName newProjectPath;
    newProjectPath.SetPath( wxString::FromUTF8( destPath.string() ) );
    newProjectPath.SetName( wxS( "finalproject" ) );
    newProjectPath.SetExt( wxS( "kemi_pro" ) );

    std::vector<wxFileName> destFiles;
    tmpl.GetDestinationFiles( newProjectPath, destFiles );

    bool foundExactRenamedDir = false;

    for( const wxFileName& destFile : destFiles )
    {
        wxString fullPath = destFile.GetFullPath();

        if( fullPath.Contains( wxS( "/finalproject/finalproject/" ) )
            || fullPath.Contains( wxS( "\\finalproject\\finalproject\\" ) ) )
        {
            foundExactRenamedDir = true;
        }

        if( fullPath.Contains( wxS( "/finalproject/mytemplate/" ) )
            || fullPath.Contains( wxS( "\\finalproject\\mytemplate\\" ) ) )
        {
            BOOST_FAIL( "Exact match directory should be renamed from mytemplate to finalproject" );
        }
    }

    BOOST_CHECK_MESSAGE( foundExactRenamedDir, "Should find renamed subdirectory finalproject" );
}


BOOST_AUTO_TEST_CASE( TemplateWithoutMetaDirSetsErrorTitle )
{
    // Issue #23623: user template directories that are just regular KiCad projects
    // (no meta/info.html) should not crash. The constructor should set an error
    // title and GetTitle() should return that error without trying to open a
    // nonexistent file.
    fs::path noMetaPath = m_tempDir / "no_meta_template";
    fs::create_directories( noMetaPath );

    std::ofstream f( ( noMetaPath / "no_meta_template.kicad_pro" ).string() );
    f << "{}";
    f.close();

    PROJECT_TEMPLATE tmpl( wxString::FromUTF8( noMetaPath.string() ) );

    wxString* title = tmpl.GetTitle();
    BOOST_REQUIRE( title != nullptr );
    BOOST_CHECK_MESSAGE( !title->IsEmpty(), "Template without meta dir should have an error title" );

    // Calling GetTitle() again should return the same cached result without
    // attempting to open meta/info.html
    wxString* title2 = tmpl.GetTitle();
    BOOST_CHECK( *title == *title2 );
}


BOOST_AUTO_TEST_CASE( TemplateWithMetaDirButNoInfoHtml )
{
    // meta directory exists but info.html is missing
    fs::path tmplPath = m_tempDir / "meta_no_html";
    fs::create_directories( tmplPath / "meta" );

    std::ofstream f( ( tmplPath / "meta_no_html.kicad_pro" ).string() );
    f << "{}";
    f.close();

    PROJECT_TEMPLATE tmpl( wxString::FromUTF8( tmplPath.string() ) );

    wxString* title = tmpl.GetTitle();
    BOOST_REQUIRE( title != nullptr );
    BOOST_CHECK_MESSAGE( !title->IsEmpty(),
                         "Template with meta dir but no info.html should have an error title" );
}


// Regression test for https://gitlab.com/kicad/code/kicad/-/issues/24343
// EnsureDefaultProjectTemplate must seed the built-in "default" template under the given base
// directory and yield a directory that PROJECT_TEMPLATE can load as a valid template.
BOOST_AUTO_TEST_CASE( EnsureDefaultTemplateSeedsBaseDir )
{
    wxString   baseDir = wxString::FromUTF8( ( m_tempDir / "default_seed" ).string() );
    wxFileName seeded = EnsureDefaultProjectTemplate( baseDir );

    BOOST_REQUIRE_MESSAGE( seeded.IsOk(), "Seeding the default template should succeed" );

    // The default template must live directly under the requested base directory, not somewhere
    // derived from an environment variable.
    fs::path expected = m_tempDir / "default_seed" / "default";
    BOOST_CHECK( fs::exists( expected ) );
    BOOST_CHECK( fs::exists( expected / "meta" / "info.html" ) );
    BOOST_CHECK( fs::exists( expected / "default.kemi_pro" ) );

    // PROJECT_TEMPLATE must recognize the seeded directory as a valid template (meta dir and
    // info.html present, so no error is reported).
    PROJECT_TEMPLATE tmpl( seeded.GetPath() );

    BOOST_CHECK_MESSAGE( tmpl.GetError().IsEmpty(),
                         "Seeded default template should load without error: " + tmpl.GetError() );

    wxString* title = tmpl.GetTitle();

    BOOST_REQUIRE( title != nullptr );
    BOOST_CHECK( !title->IsEmpty() );
}


BOOST_AUTO_TEST_CASE( ProjectCreatedFromDefaultTemplateLoads )
{
    wxString   baseDir = wxString::FromUTF8( ( m_tempDir / "default_load" ).string() );
    wxFileName seeded = EnsureDefaultProjectTemplate( baseDir );

    BOOST_REQUIRE( seeded.IsOk() );

    fs::path destPath = m_tempDir / "fromdefault";
    fs::create_directories( destPath );

    PROJECT_TEMPLATE tmpl( seeded.GetPath() );

    wxFileName newProjectPath;
    newProjectPath.SetPath( wxString::FromUTF8( destPath.string() ) );
    newProjectPath.SetName( wxS( "fromdefault" ) );
    newProjectPath.SetExt( wxS( "kemi_pro" ) );

    wxString errorMsg;

    BOOST_REQUIRE_MESSAGE( tmpl.CreateProject( newProjectPath, &errorMsg ),
                           "CreateProject should succeed: " + errorMsg.ToStdString() );

    SETTINGS_MANAGER mgr;

    BOOST_CHECK_MESSAGE( mgr.LoadProject( newProjectPath.GetFullPath() ),
                         "A project made from the default template must load" );
}


// An empty base directory must not create anything and must report failure.
BOOST_AUTO_TEST_CASE( EnsureDefaultTemplateRejectsEmptyBaseDir )
{
    wxFileName seeded = EnsureDefaultProjectTemplate( wxEmptyString );

    BOOST_CHECK( !seeded.IsOk() );
}


// Calling EnsureDefaultProjectTemplate twice must be idempotent and not clobber existing content.
BOOST_AUTO_TEST_CASE( EnsureDefaultTemplateIsIdempotent )
{
    wxString baseDir = wxString::FromUTF8( ( m_tempDir / "default_idempotent" ).string() );

    wxFileName first = EnsureDefaultProjectTemplate( baseDir );
    BOOST_REQUIRE( first.IsOk() );

    // Overwrite the project file with custom content to verify it survives a second call.
    fs::path proFile = m_tempDir / "default_idempotent" / "default" / "default.kemi_pro";
    {
        std::ofstream f( proFile.string() );
        f << "{\"custom\":true}";
    }

    wxFileName second = EnsureDefaultProjectTemplate( baseDir );
    BOOST_REQUIRE( second.IsOk() );
    BOOST_CHECK_EQUAL( first.GetPath(), second.GetPath() );

    std::ifstream     in( proFile.string() );
    std::stringstream buffer;
    buffer << in.rdbuf();

    BOOST_CHECK_EQUAL( buffer.str(), std::string( "{\"custom\":true}" ) );
}


BOOST_AUTO_TEST_CASE( ReadOnlyTemplateProducesWritableProject )
{
    CreateTemplateStructure( "rotemplate", { "rotemplate-lib" },
                             { "rotemplate.kicad_pro", "rotemplate.kicad_sch", "rotemplate-lib/component.kicad_sym" } );

    fs::path templatePath = m_tempDir / "rotemplate";
    fs::path destPath = m_tempDir / "rwproject";
    fs::create_directories( destPath );

    const std::vector<fs::path> templateFiles = { templatePath / "rotemplate.kicad_pro",
                                                  templatePath / "rotemplate.kicad_sch",
                                                  templatePath / "rotemplate-lib" / "component.kicad_sym" };

    for( const fs::path& file : templateFiles )
    {
        fs::permissions( file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                         fs::perm_options::remove );
    }

    PROJECT_TEMPLATE tmpl( wxString::FromUTF8( templatePath.string() ) );

    wxFileName newProjectPath;
    newProjectPath.SetPath( wxString::FromUTF8( destPath.string() ) );
    newProjectPath.SetName( wxS( "rwproject" ) );
    newProjectPath.SetExt( wxS( "kemi_pro" ) );

    wxString errorMsg;
    bool     created = tmpl.CreateProject( newProjectPath, &errorMsg );

    // Restore write permission before asserting, so that a failed assertion still leaves the
    // fixture able to remove the temporary directory.
    for( const fs::path& file : templateFiles )
        fs::permissions( file, fs::perms::owner_write, fs::perm_options::add );

    BOOST_REQUIRE_MESSAGE( created, "CreateProject failed: " + errorMsg.ToStdString() );

    const std::vector<fs::path> projectFiles = { destPath / "rwproject.kemi_pro", destPath / "rwproject.kemi_sch",
                                                 destPath / "rwproject-lib" / "component.kemi_sym" };

    for( const fs::path& file : projectFiles )
    {
        wxFileName fn( wxString::FromUTF8( file.string() ) );

        BOOST_REQUIRE_MESSAGE( fn.FileExists(), file.string() + " was not created" );
        BOOST_CHECK_MESSAGE( fn.IsFileWritable(), file.string() + " is read only" );
    }
}


std::string readTemplateTestFile( const fs::path& aPath )
{
    std::ifstream input( aPath, std::ios::binary );
    BOOST_REQUIRE( input.good() );
    return std::string( std::istreambuf_iterator<char>( input ), std::istreambuf_iterator<char>() );
}


std::map<fs::path, std::string> snapshotTemplate( const fs::path& aPath )
{
    std::map<fs::path, std::string> result;

    for( const auto& entry : fs::recursive_directory_iterator( aPath ) )
    {
        if( entry.is_regular_file() )
            result.emplace( entry.path(), readTemplateTestFile( entry.path() ) );
    }

    return result;
}


BOOST_AUTO_TEST_CASE( OfficialTemplateCreatesNativeDocumentsAndReferences )
{
    CreateTemplateStructure( "template", { "template-sub" },
            { "template.kicad_pro", "template.kicad_sch", "template.kicad_pcb",
              "template-sub/child.kicad_sch", "Parts.kicad_sym", "sym-lib-table" } );
    const fs::path source = m_tempDir / "template";
    const fs::path destination = m_tempDir / "result";
    fs::create_directories( destination );
    {
        std::ofstream project( source / "template.kicad_pro" );
        project << R"({"meta":{"version":1},"schematic":{"top_level_sheets":[)"
                   R"({"filename":"template.kicad_sch","name":"template"}]},)"
                   R"("boards":[["board-id","template.kicad_pcb"]]})";
        std::ofstream schematic( source / "template.kicad_sch" );
        schematic << R"((kicad_sch (version 20250114) (generator eeschema))" "\n"
                     R"( (sheet (property "Sheetfile" "template-sub/child.kicad_sch")))" "\n"
                     R"( (text "template-sub/child.kicad_sch")))";
        std::ofstream table( source / "sym-lib-table" );
        table << R"((sym_lib_table (version 7) (lib (name "Parts") (type "KiCad"))"
                 R"( (uri "${KIPRJMOD}/Parts.kicad_sym") (options "") (descr ""))))";
    }

    const auto sourceBefore = snapshotTemplate( source );
    PROJECT_TEMPLATE projectTemplate( wxString::FromUTF8( source.string() ) );
    wxFileName projectPath( wxString::FromUTF8( ( destination / "result.kemi_pro" ).string() ) );
    wxString error;
    BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( projectPath, &error ), error );
    BOOST_CHECK( snapshotTemplate( source ) == sourceBefore );
    const auto project = nlohmann::json::parse( readTemplateTestFile( destination / "result.kemi_pro" ) );
    BOOST_CHECK( KEMI_FORMAT::ValidateJsonMarker( project, KEMI_FORMAT::DOC::PROJECT,
                                                projectPath.GetFullPath() ).IsEmpty() );
    BOOST_CHECK_EQUAL( project["schematic"]["top_level_sheets"][0]["filename"].get<std::string>(),
                       "result.kemi_sch" );
    BOOST_CHECK_EQUAL( project["boards"][0][1].get<std::string>(), "result.kemi_pcb" );
    const std::string schematic = readTemplateTestFile( destination / "result.kemi_sch" );
    BOOST_CHECK( schematic.starts_with( "(kemi_sch" ) );
    BOOST_CHECK( schematic.find( "result-sub/child.kemi_sch" ) != std::string::npos );
    BOOST_CHECK( schematic.find( "template-sub/child.kicad_sch" ) != std::string::npos );
    BOOST_CHECK( readTemplateTestFile( destination / "result.kemi_pcb" ).starts_with( "(kemi_pcb" ) );
    BOOST_CHECK( readTemplateTestFile( destination / "result-sub/child.kemi_sch" ).starts_with( "(kemi_sch" ) );
    BOOST_CHECK( readTemplateTestFile( destination / "Parts.kemi_sym" ).starts_with( "(kemi_symbol_lib" ) );
    BOOST_CHECK( readTemplateTestFile( destination / "sym-lib-table" ).find( "${KIPRJMOD}/Parts.kemi_sym" )
                 != std::string::npos );

    for( const auto& entry : fs::recursive_directory_iterator( destination ) )
        BOOST_CHECK( !entry.path().extension().string().starts_with( ".kicad_" ) );

    SETTINGS_MANAGER settings;
    BOOST_REQUIRE( settings.LoadProject( projectPath.GetFullPath() ) );
    BOOST_REQUIRE( settings.SaveProject() );
    BOOST_REQUIRE( settings.UnloadProject( &settings.Prj(), false ) );
    BOOST_REQUIRE( settings.LoadProject( projectPath.GetFullPath() ) );
    BOOST_CHECK( snapshotTemplate( source ) == sourceBefore );
}


BOOST_AUTO_TEST_CASE( TemplateKeepsLibraryContainersAndConvertsCompanions )
{
    CreateTemplateStructure( "profiles", { "Shapes.pretty", "profiles-symbols.kicad_symdir" },
            { "profiles.kicad_pro", "profiles.kicad_prl", "profiles.kicad_jobset",
              "Shapes.pretty/Pad.kicad_mod", "profiles-symbols.kicad_symdir/Part.kicad_sym",
              "drawing.kicad_wks", "sym-lib-table", "fp-lib-table", "database.kicad_dbl" } );
    const fs::path source = m_tempDir / "profiles";
    const fs::path destination = m_tempDir / "companions";
    fs::create_directories( destination );
    {
        std::ofstream symbols( source / "sym-lib-table" );
        symbols << R"((sym_lib_table (version 7) (lib (name "Parts") (type "KiCad"))"
                   R"( (uri "${KIPRJMOD}/profiles-symbols.kicad_symdir") (options "") (descr ""))))";
        std::ofstream footprints( source / "fp-lib-table" );
        footprints << R"((fp_lib_table (version 7) (lib (name "Shapes") (type "KiCad"))"
                      R"( (uri "${KIPRJMOD}/Shapes.pretty") (options "") (descr ""))))";
    }

    const auto before = snapshotTemplate( source );
    PROJECT_TEMPLATE projectTemplate( wxString::FromUTF8( source.string() ) );
    wxFileName projectPath( wxString::FromUTF8( ( destination / "companions.kemi_pro" ).string() ) );
    wxString error;
    BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( projectPath, &error ), error );
    BOOST_CHECK( snapshotTemplate( source ) == before );
    BOOST_CHECK( fs::exists( destination / "Shapes.pretty/Pad.kemi_mod" ) );
    BOOST_CHECK( fs::exists( destination / "companions-symbols.kicad_symdir/Part.kemi_sym" ) );
    BOOST_CHECK( readTemplateTestFile( destination / "sym-lib-table" )
                         .find( "${KIPRJMOD}/companions-symbols.kicad_symdir" ) != std::string::npos );
    const std::string footprint = readTemplateTestFile( destination / "Shapes.pretty/Pad.kemi_mod" );
    BOOST_CHECK( footprint.starts_with( "(kemi_footprint" ) );
    BOOST_CHECK( footprint.find( "\"Pad\"" ) < footprint.find( "format_version" ) );
    BOOST_CHECK( readTemplateTestFile( destination / "drawing.kemi_wks" ).starts_with( "(kemi_wks" ) );
    BOOST_CHECK( readTemplateTestFile( destination / "database.kicad_dbl" )
                 == readTemplateTestFile( source / "database.kicad_dbl" ) );

    for( KEMI_FORMAT::DOC type : { KEMI_FORMAT::DOC::PROJECT_LOCAL, KEMI_FORMAT::DOC::JOBSET } )
    {
        const std::string filename = "companions." + KEMI_FORMAT::Ext( type, KEMI_FORMAT::FLAVOR::KEMI );
        const auto document = nlohmann::json::parse( readTemplateTestFile( destination / filename ) );
        BOOST_CHECK( KEMI_FORMAT::ValidateJsonMarker( document, type,
                wxString::FromUTF8( ( destination / filename ).string() ) ).IsEmpty() );
    }
}


// MF1's rule round trips use the real DRC parser in qa_pcbnew's ProjectTemplate suite.
// SF1's failure injection here covers preparation and publication independently.
// See project_template_regressions.md for the regression checklist and execution status.
BOOST_AUTO_TEST_CASE( TemplateWriteFailureCanBeRetried )
{
    CreateTemplateStructure( "retry", {},
                             { "retry.kicad_pro", "one.kicad_dru", "two.kicad_dru" } );
    const fs::path source = m_tempDir / "retry";
    const fs::path destination = m_tempDir / "retried";
    fs::create_directories( destination );
    {
        std::ofstream sentinel( destination / "user.txt" );
        sentinel << "pre-existing user file";
    }

    const auto sourceBefore = snapshotTemplate( source );
    const auto targetBefore = snapshotTemplate( destination );
    bool failWrite = true;
    size_t conversions = 0;
    PROJECT_TEMPLATE projectTemplate( wxString::FromUTF8( source.string() ),
            [&]( const wxString&, const wxString& aTarget )
            {
                ++conversions;
                wxFFile output( aTarget, wxS( "wb" ) );
                BOOST_REQUIRE( output.IsOpened() );

                // Leave an actual partial staging file after another output was prepared.
                const bool fail = failWrite && conversions == 2;
                BOOST_REQUIRE( output.Write( fail ? wxS( "partial rules" )
                        : wxS( "(kemi_dru (format_version 1) (version 1))\n" ), wxConvUTF8 ) );
                BOOST_REQUIRE( output.Close() );
                return !fail;
            } );
    wxFileName projectPath( wxString::FromUTF8( ( destination / "retried.kemi_pro" ).string() ) );
    wxString error;
    BOOST_CHECK( !projectTemplate.CreateProject( projectPath, &error ) );
    BOOST_CHECK( !error.IsEmpty() );
    BOOST_CHECK_EQUAL( conversions, 2u );
    BOOST_CHECK( snapshotTemplate( source ) == sourceBefore );
    BOOST_CHECK( snapshotTemplate( destination ) == targetBefore );

    for( const auto& entry : fs::directory_iterator( destination ) )
        BOOST_CHECK( !entry.path().filename().string().starts_with( ".kemi-template-" ) );

    failWrite = false;
    conversions = 0;
    error.clear();
    BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( projectPath, &error ), error );
    BOOST_CHECK_EQUAL( conversions, 2u );
    BOOST_CHECK( fs::exists( destination / "retried.kemi_pro" ) );
    BOOST_CHECK( fs::exists( destination / "one.kemi_dru" ) );
    BOOST_CHECK( fs::exists( destination / "two.kemi_dru" ) );
    BOOST_CHECK_EQUAL( readTemplateTestFile( destination / "user.txt" ), "pre-existing user file" );
    BOOST_CHECK( snapshotTemplate( source ) == sourceBefore );
}


BOOST_AUTO_TEST_CASE( TemplatePublishFailureKeepsUserFilesAndCanBeRetried )
{
    CreateTemplateStructure( "retry", {},
                             { "retry.kicad_pro", "one.kicad_dru", "two.kicad_dru" } );
    const fs::path source = m_tempDir / "retry";
    const fs::path destination = m_tempDir / "retried";
    fs::create_directories( destination );
    {
        std::ofstream sentinel( destination / "user.txt" );
        sentinel << "pre-existing user file";
    }

    const auto sourceBefore = snapshotTemplate( source );
    auto expectedAfterFailure = snapshotTemplate( destination );
    bool introduceCollision = true;
    size_t conversions = 0;
    fs::path collision;
    PROJECT_TEMPLATE projectTemplate( wxString::FromUTF8( source.string() ),
            [&]( const wxString& aSource, const wxString& aTarget )
            {
                wxFFile output( aTarget, wxS( "wb" ) );
                BOOST_REQUIRE( output.IsOpened() );
                BOOST_REQUIRE( output.Write( wxS( "(kemi_dru (format_version 1) (version 1))\n" ),
                                             wxConvUTF8 ) );
                BOOST_REQUIRE( output.Close() );

                if( ++conversions == 2 && introduceCollision )
                {
                    // Simulate a user file arriving after preflight. The earlier rule will
                    // already have been published when this destination rejects its rename.
                    collision = destination / ( wxFileName( aSource ).GetName().ToStdString()
                                                  + ".kemi_dru" );
                    std::ofstream sentinel( collision, std::ios::binary );
                    sentinel << "user rules created during template preparation";
                    BOOST_REQUIRE( sentinel.good() );
                }

                return true;
            } );
    wxFileName projectPath( wxString::FromUTF8( ( destination / "retried.kemi_pro" ).string() ) );
    wxString error;
    BOOST_CHECK( !projectTemplate.CreateProject( projectPath, &error ) );
    BOOST_CHECK( !error.IsEmpty() );
    BOOST_REQUIRE_EQUAL( conversions, 2u );
    expectedAfterFailure.emplace( collision, "user rules created during template preparation" );
    BOOST_CHECK( snapshotTemplate( destination ) == expectedAfterFailure );
    BOOST_CHECK( snapshotTemplate( source ) == sourceBefore );

    // Preserve the conflicting file under a different name, then retry the same destination.
    const fs::path preserved = destination / "preserved-user-rules.kemi_dru";
    fs::rename( collision, preserved );
    introduceCollision = false;
    conversions = 0;
    error.clear();
    BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( projectPath, &error ), error );
    BOOST_CHECK_EQUAL( conversions, 2u );
    BOOST_CHECK( fs::exists( destination / "retried.kemi_pro" ) );
    BOOST_CHECK( fs::exists( destination / "one.kemi_dru" ) );
    BOOST_CHECK( fs::exists( destination / "two.kemi_dru" ) );
    BOOST_CHECK_EQUAL( readTemplateTestFile( preserved ), "user rules created during template preparation" );
    BOOST_CHECK_EQUAL( readTemplateTestFile( destination / "user.txt" ), "pre-existing user file" );
    BOOST_CHECK( snapshotTemplate( source ) == sourceBefore );
}


BOOST_AUTO_TEST_CASE( TemplateCollisionLeavesAllFilesUntouched )
{
    CreateTemplateStructure( "collision", {}, { "collision.kicad_pro", "collision.kicad_sch" } );
    const fs::path source = m_tempDir / "collision";
    const fs::path destination = m_tempDir / "existing";
    fs::create_directories( destination );
    {
        std::ofstream sentinel( destination / "existing.kemi_sch" );
        sentinel << "existing native document";
    }

    const auto sourceBefore = snapshotTemplate( source );
    const auto targetBefore = snapshotTemplate( destination );
    PROJECT_TEMPLATE projectTemplate( wxString::FromUTF8( source.string() ) );
    wxFileName projectPath( wxString::FromUTF8( ( destination / "existing.kemi_pro" ).string() ) );
    wxString error;
    BOOST_CHECK( !projectTemplate.CreateProject( projectPath, &error ) );
    BOOST_CHECK( !error.IsEmpty() );
    BOOST_CHECK( snapshotTemplate( source ) == sourceBefore );
    BOOST_CHECK( snapshotTemplate( destination ) == targetBefore );
}


BOOST_AUTO_TEST_CASE( NativeTemplateCounterpartTakesPrecedence )
{
    CreateTemplateStructure( "mixed", {}, { "mixed.kicad_pro", "Parts.kicad_sym", "Parts.kemi_sym" } );
    const fs::path source = m_tempDir / "mixed";
    const fs::path destination = m_tempDir / "native";
    fs::create_directories( destination );
    {
        std::ofstream library( source / "Parts.kemi_sym" );
        library << R"((kemi_symbol_lib (format_version 1) (version 20241209))"
                   R"( (generator kicad_symbol_editor) (symbol "NativePart")))";
    }

    const auto before = snapshotTemplate( source );
    PROJECT_TEMPLATE projectTemplate( wxString::FromUTF8( source.string() ) );
    wxFileName projectPath( wxString::FromUTF8( ( destination / "native.kemi_pro" ).string() ) );
    wxString error;
    BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( projectPath, &error ), error );
    BOOST_CHECK( readTemplateTestFile( destination / "Parts.kemi_sym" ).find( "NativePart" ) != std::string::npos );
    BOOST_CHECK( !fs::exists( destination / "Parts.kicad_sym" ) );
    BOOST_CHECK( snapshotTemplate( source ) == before );
}


BOOST_AUTO_TEST_CASE( ExistingOfficialDefaultTemplateIsPreserved )
{
    CreateTemplateStructure( "old-default/default", {}, { "default.kicad_pro" } );
    const fs::path source = m_tempDir / "old-default/default";
    const auto before = snapshotTemplate( source );
    const wxFileName seeded = EnsureDefaultProjectTemplate(
            wxString::FromUTF8( ( m_tempDir / "old-default" ).string() ) );
    BOOST_REQUIRE( seeded.IsOk() );
    BOOST_CHECK( !fs::exists( source / "default.kemi_pro" ) );
    BOOST_CHECK( snapshotTemplate( source ) == before );
}


BOOST_AUTO_TEST_CASE( TemplateRejectsOfficialProjectDestination )
{
    CreateTemplateStructure( "official-target", {}, { "official-target.kicad_pro" } );
    const fs::path source = m_tempDir / "official-target";
    const fs::path destination = m_tempDir / "rejected";
    fs::create_directories( destination );
    PROJECT_TEMPLATE projectTemplate( wxString::FromUTF8( source.string() ) );
    wxFileName projectPath( wxString::FromUTF8( ( destination / "rejected.kicad_pro" ).string() ) );
    wxString error;
    BOOST_CHECK( !projectTemplate.CreateProject( projectPath, &error ) );
    BOOST_CHECK( fs::is_empty( destination ) );
}


BOOST_AUTO_TEST_SUITE_END()
