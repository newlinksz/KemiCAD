/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 */

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>
#include <cli/exit_codes.h>
#include <design_block.h>
#include <design_block_io.h>
#include <api/sch_api_save.h>
#include <downgrade/sch_downgrade.h>
#include <downgrade_target.h>
#include <eeschema_jobs_handler.h>
#include <jobs/job_sch_downgrade.h>
#include <kemi_format_profile.h>
#include <eeschema_save_file_as.h>
#include <gestfich.h>
#include <lib_symbol.h>
#include <libraries/legacy_symbol_library.h>
#include <libraries/library_table.h>
#include <local_history.h>
#include <project.h>
#include <project_rescue.h>
#include <pgm_base.h>
#include <libraries/library_manager.h>
#include <wx/log.h>
#include <project/project_file.h>
#include <reporter.h>
#include <richio.h>
#include <wx_filename.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_text.h>
#include <sch_sheet.h>
#include <sch_save_state.h>
#include <save_project_utils.h>
#include <schematic.h>
#include <settings/settings_manager.h>
#include <symbol_library_manager.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <wx/ffile.h>

namespace
{
using DIRECTORY_SNAPSHOT = std::map<std::filesystem::path, std::string>;

DIRECTORY_SNAPSHOT snapshotDirectory( const std::filesystem::path& aDirectory )
{
    DIRECTORY_SNAPSHOT snapshot;

    for( const auto& entry : std::filesystem::recursive_directory_iterator( aDirectory ) )
    {
        if( entry.is_regular_file() )
        {
            std::ifstream file( entry.path(), std::ios::binary );
            BOOST_REQUIRE( file.good() );
            snapshot.emplace( entry.path(),
                    std::string( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() ) );
        }
        else
        {
            snapshot.emplace( entry.path(), std::string() );
        }
    }

    return snapshot;
}


struct KEMI_PERSISTENT_SCHEMATIC_FIXTURE
{
    KEMI_PERSISTENT_SCHEMATIC_FIXTURE() : directory( "kemi-persistent-sch" )
    {
        const wxString projectPath = directory.ChildPathStr( "project.kemi_pro" );
        PROJECT seedProject;
        PROJECT_FILE seedFile( projectPath );
        seedFile.SetProject( &seedProject );

        // Loading requires an existing project with valid schema metadata and a Kemi marker.
        BOOST_REQUIRE( seedFile.SaveToFile( wxEmptyString, true ) );
        BOOST_REQUIRE( settings.LoadProject( projectPath, true ) );
        schematic = std::make_unique<SCHEMATIC>( &settings.Prj() );
        sheet = schematic->GetTopLevelSheet( 0 );
        BOOST_REQUIRE( sheet );
        const wxString path = directory.ChildPathStr( "root.kemi_sch" );
        sheet->SetFileName( path );
        sheet->GetScreen()->SetFileName( path );
        schematic->RefreshHierarchy();
    }

    ~KEMI_PERSISTENT_SCHEMATIC_FIXTURE()
    {
        schematic.reset();
        settings.UnloadProject( &settings.Prj(), false );
    }

    KI_TEST::SCOPED_TEMP_DIR directory;
    SETTINGS_MANAGER settings;
    std::unique_ptr<SCHEMATIC> schematic;
    SCH_SHEET* sheet = nullptr;
};
}

BOOST_FIXTURE_TEST_SUITE( KemiPersistentSchematicOutput, KEMI_PERSISTENT_SCHEMATIC_FIXTURE )

BOOST_AUTO_TEST_CASE( ApiSavePreservesNativeSheetVariablesAndConvertsOnlyTheSuffix )
{
    settings.Prj().GetTextVars()[wxS( "SUBSHEET_FILE" )] = wxS( "resolved_subsheet" );
    SCH_SHEET* child = new SCH_SHEET( schematic.get() );
    child->SetScreen( new SCH_SCREEN( schematic.get() ) );
    child->SetName( wxS( "Child" ) );
    child->SetFileName( wxS( "${SUBSHEET_FILE}.kemi_sch" ) );
    child->GetScreen()->SetFileName( directory.ChildPathStr( "resolved_subsheet.kemi_sch" ) );
    sheet->GetScreen()->Append( child );
    schematic->RefreshHierarchy();
    BOOST_REQUIRE( SCH_API_SAVE::SaveSchematic( *schematic, settings.Prj() ) );
    BOOST_CHECK_EQUAL( child->GetFileName(), wxS( "${SUBSHEET_FILE}.kemi_sch" ) );
    BOOST_CHECK( KI_TEST::LoadStringData( sheet->GetScreen()->GetFileName() )
                         .find( "${SUBSHEET_FILE}.kemi_sch" ) != std::string::npos );

    const wxString officialRoot = directory.ChildPathStr( "official.kicad_sch" );
    const wxString officialChild = directory.ChildPathStr( "converted_child.kicad_sch" );
    for( const wxString& path : { officialRoot, officialChild } )
    {
        wxFFile file( path, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "official source sentinel" ) ) );
    }

    // Use a different native target so the first-save collision guard remains exercised.
    settings.Prj().GetTextVars()[wxS( "SUBSHEET_FILE" )] = wxS( "converted_child" );
    child->SetFileName( wxS( "${SUBSHEET_FILE}.kicad_sch" ) );
    child->GetScreen()->SetFileName( directory.ChildPathStr( "converted_child.kicad_sch" ) );
    sheet->GetScreen()->SetFileName( officialRoot );
    BOOST_REQUIRE( SCH_API_SAVE::SaveSchematic( *schematic, settings.Prj() ) );
    BOOST_CHECK_EQUAL( child->GetFileName(), wxS( "${SUBSHEET_FILE}.kemi_sch" ) );
    BOOST_CHECK( wxFileExists( directory.ChildPathStr( "converted_child.kemi_sch" ) ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( officialRoot ), "official source sentinel" );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( officialChild ), "official source sentinel" );
}


BOOST_AUTO_TEST_CASE( GuiSaveAsPreparationKeepsVariablesThatFollowTheCopiedSheet )
{
    settings.Prj().GetTextVars()[wxS( "CHILD" )] = wxS( "child" );
    SCH_SHEET* child = new SCH_SHEET( schematic.get() );
    child->SetScreen( new SCH_SCREEN( schematic.get() ) );
    child->SetFileName( wxS( "${CHILD}.kemi_sch" ) );
    child->GetScreen()->SetFileName( directory.ChildPathStr( "child.kemi_sch" ) );
    sheet->GetScreen()->Append( child );
    schematic->RefreshHierarchy();
    SCH_SCREENS screens( schematic->Root() );
    wxFileName oldRoot( sheet->GetScreen()->GetFileName() );
    const wxString destination = directory.CreateChildDirStr( "save-as" );
    wxFileName newRoot( destination, wxS( "renamed.kemi_sch" ) );
    sheet->GetScreen()->SetFileName( newRoot.GetFullPath() );
    std::unordered_map<SCH_SCREEN*, wxString> targets;
    wxString error;
    BOOST_REQUIRE( PrepareSaveAsFiles( *schematic, screens, oldRoot, newRoot,
                                       false, true, false, targets, error ) );
    BOOST_CHECK_EQUAL( child->GetFileName(), wxS( "${CHILD}.kemi_sch" ) );

    // A target that the expression can no longer represent must get a real new reference.
    child->GetScreen()->SetFileName( wxFileName( destination, wxS( "different.kemi_sch" ) ).GetFullPath() );
    for( const SCH_SHEET_PATH& path : schematic->Hierarchy() )
        UpdateSheetFileReference( *schematic, path );
    BOOST_CHECK_EQUAL( child->GetFileName(), wxS( "different.kemi_sch" ) );
}


BOOST_AUTO_TEST_CASE( OfficialSaveAsDoesNotCreateFile )
{
    SCH_IO_KICAD_SEXPR writer;
    const wxString rejected = directory.ChildPathStr( "copy.kicad_sch" );
    BOOST_CHECK_THROW( writer.SaveSchematicFile( rejected, sheet, schematic.get() ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( rejected ) );
    BOOST_CHECK( !SCH_API_SAVE::SaveSchematicCopy( *schematic, settings.Prj(), rejected, true ) );
    BOOST_CHECK( !SCH_API_SAVE::SaveSchematicAs( *schematic, settings.Prj(), rejected ) );
    BOOST_CHECK( !wxFileExists( rejected ) );

    // Renaming the in-memory document must not erase its loaded/native restriction.
    sheet->GetScreen()->SetFileName( rejected );
    STRING_FORMATTER formatter;
    BOOST_CHECK_THROW( writer.FormatSchematicToFormatter( &formatter, sheet, schematic.get() ), IO_ERROR );
    BOOST_CHECK( formatter.GetString().empty() );
    BOOST_CHECK_NO_THROW( writer.SaveSchematicFile( directory.ChildPathStr( "copy.kemi_sch" ),
                                                   sheet, schematic.get() ) );
}

BOOST_AUTO_TEST_CASE( DowngradeJobAndWritersRejectBeforeOutput )
{
    const wxString source = directory.ChildPathStr( "root.kemi_sch" );
    SCH_IO_KICAD_SEXPR writer;
    writer.SaveSchematicFile( source, sheet, schematic.get() );
    WX_STRING_REPORTER reporter;
    EESCHEMA_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    JOB_SCH_DOWNGRADE job;
    job.m_filename = source;
    job.m_target = "9.0";
    job.m_outputDir = directory.ChildPathStr( "downgraded" );
    const auto nativeBefore = snapshotDirectory( directory.Path() );
    BOOST_CHECK_EQUAL( handler.JobDowngrade( &job ), CLI::EXIT_CODES::ERR_INVALID_INPUT_FILE );
    BOOST_CHECK( !wxDirExists( job.m_outputDir ) );
    BOOST_CHECK( snapshotDirectory( directory.Path() ) == nativeBefore );

    // A native root renamed to an official extension must fail before the loader can
    // create its corresponding project/local-settings files in the input directory.
    const wxString renamed = directory.ChildPathStr( "renamed.kicad_sch" );
    BOOST_REQUIRE( wxCopyFile( source, renamed ) );
    job.m_filename = renamed;
    const auto renamedBefore = snapshotDirectory( directory.Path() );
    BOOST_CHECK_EQUAL( handler.JobDowngrade( &job ), CLI::EXIT_CODES::ERR_INVALID_INPUT_FILE );
    BOOST_CHECK( snapshotDirectory( directory.Path() ) == renamedBefore );

    // Project markers remain binding even under an official project extension.
    const wxString official = directory.ChildPathStr( "marked-project.kicad_sch" );
    {
        wxFFile schematicFile( official, wxS( "wb" ) );
        BOOST_REQUIRE( schematicFile.IsOpened() );
        BOOST_REQUIRE( schematicFile.Write( wxS( "(kicad_sch)" ) ) );
        wxFFile projectFile( directory.ChildPathStr( "marked-project.kicad_pro" ), wxS( "wb" ) );
        BOOST_REQUIRE( projectFile.IsOpened() );
        BOOST_REQUIRE( projectFile.Write(
                wxS( "{\"kemicad\":{\"document_type\":\"project\",\"format_version\":1}}" ) ) );
    }
    job.m_filename = official;
    const auto projectBefore = snapshotDirectory( directory.Path() );
    BOOST_CHECK_EQUAL( handler.JobDowngrade( &job ), CLI::EXIT_CODES::ERR_INVALID_INPUT_FILE );
    BOOST_CHECK( snapshotDirectory( directory.Path() ) == projectBefore );

    for( const DOWNGRADE_TARGET& target : GetDowngradeTargets() )
    {
        const wxString output = directory.ChildPathStr( "old.kicad_sch" );
        BOOST_CHECK_THROW( SaveSchematicForTarget( sheet, schematic.get(), output, target ), IO_ERROR );
        BOOST_CHECK( !wxFileExists( output ) );
    }

    const wxString library = directory.ChildPathStr( "native.kemi_sym" );
    writer.CreateLibrary( library );
    const wxString libraryDir = directory.CreateChildDirStr( "libraries" );
    BOOST_REQUIRE( wxCopyFile( library, wxFileName( libraryDir, "renamed.kicad_sym" ).GetFullPath() ) );
    JOB_SCH_DOWNGRADE libraryJob;
    libraryJob.m_libraryDir = libraryDir;
    libraryJob.m_outputDir = directory.ChildPathStr( "old-libraries" );
    libraryJob.m_target = "9.0";
    const auto librariesBefore = snapshotDirectory( directory.Path() );
    BOOST_CHECK_EQUAL( handler.JobDowngrade( &libraryJob ), CLI::EXIT_CODES::ERR_INVALID_INPUT_FILE );
    BOOST_CHECK( !wxDirExists( libraryJob.m_outputDir ) );
    BOOST_CHECK( snapshotDirectory( directory.Path() ) == librariesBefore );
}

BOOST_AUTO_TEST_CASE( SymbolLibraryAndEditBufferCannotRevert )
{
    const wxString source = directory.ChildPathStr( "symbols.kemi_sym" );
    const wxString output = directory.ChildPathStr( "symbols.kicad_sym" );
    SCH_IO_KICAD_SEXPR writer;
    writer.CreateLibrary( source );
    writer.SaveSymbol( source, std::make_unique<LIB_SYMBOL>( "Native" ) );
    BOOST_REQUIRE( writer.LoadSymbol( source, "Native" ) );
    BOOST_CHECK_THROW( writer.SaveLibrary( output ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( output ) );

    LIB_BUFFER buffer( "Native" );
    buffer.Load( source );
    SCH_IO_KICAD_SEXPR copyWriter;
    {
        wxLogNull silence;
        BOOST_CHECK( !buffer.SaveLibrary( output, &copyWriter, false ) );
    }
    BOOST_CHECK( !wxFileExists( output ) );

    // Empty Kemi libraries retain the restriction even without a symbol to carry it.
    const wxString empty = directory.ChildPathStr( "empty.kemi_sym" );
    SCH_IO_KICAD_SEXPR emptyWriter;
    emptyWriter.CreateLibrary( empty );
    BOOST_CHECK_THROW( emptyWriter.SaveLibrary( output ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( output ) );
}

BOOST_AUTO_TEST_CASE( OfficialSymbolLibraryFirstSaveRejectsExistingNativeTargets )
{
    for( bool buffered : { false, true } )
    {
        BOOST_TEST_CONTEXT( ( buffered ? "buffered manager" : "unbuffered manager" ) )
        {
            SETTINGS_MANAGER& managerSettings = Pgm().GetSettingsManager();
            KI_TEST::SCOPED_TEMP_PROJECT project( managerSettings, wxS( "kemi-library-first-save" ),
                                                 wxS( "source" ) );
            const wxString official = wxFileName( project.DirStr(), wxS( "Parts.kicad_sym" ) ).GetFullPath();
            const wxString native = wxFileName( project.DirStr(), wxS( "Parts.kemi_sym" ) ).GetFullPath();
            const wxString fresh = wxFileName( project.DirStr(), wxS( "Fresh.kemi_sym" ) ).GetFullPath();
            {
                wxFFile file( official, wxS( "wb" ) );
                BOOST_REQUIRE( file.IsOpened() );
                BOOST_REQUIRE( file.Write( wxS( "(kicad_symbol_lib (version 20241209)"
                        " (generator kicad_symbol_editor) (symbol \"Part\"))" ), wxConvUTF8 ) );
            }

            SCH_IO_KICAD_SEXPR writer;
            writer.CreateLibrary( native );
            writer.SaveSymbol( native, std::make_unique<LIB_SYMBOL>( wxS( "Part" ) ) );
            const std::string officialBefore = KI_TEST::LoadStringData( official );
            const std::string nativeBefore = KI_TEST::LoadStringData( native );
            LIBRARY_MANAGER& libraries = Pgm().GetLibraryManager();
            libraries.LoadProjectTables( project.DirStr(), { LIBRARY_TABLE_TYPE::SYMBOL } );
            auto table = libraries.Table( LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_SCOPE::PROJECT );
            BOOST_REQUIRE( table );
            LIBRARY_TABLE_ROW& row = ( *table )->InsertRow();
            row.SetNickname( wxS( "Parts" ) );
            row.SetType( wxS( "KiCad" ) );
            row.SetURI( official );
            BOOST_REQUIRE( ( *table )->Save() );
            libraries.LoadProjectTables( project.DirStr(), { LIBRARY_TABLE_TYPE::SYMBOL } );

            SYMBOL_LIBRARY_MANAGER libraryManager( project.Project() );

            if( buffered )
            {
                LIB_SYMBOL* symbol = libraryManager.GetBufferedSymbol( wxS( "Part" ), wxS( "Parts" ) );
                BOOST_REQUIRE( symbol );
                symbol->GetValueField().SetText( wxS( "Edited" ) );
                libraryManager.SetSymbolModified( wxS( "Part" ), wxS( "Parts" ) );
            }

            wxLogNull quiet;
            BOOST_CHECK( !libraryManager.SaveLibrary( wxS( "Parts" ), native, SCH_IO_MGR::SCH_KICAD ) );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( official ), officialBefore );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( native ), nativeBefore );

            if( buffered )
            {
                BOOST_CHECK( libraryManager.IsLibraryModified( wxS( "Parts" ) ) );
                BOOST_CHECK_EQUAL( libraryManager.GetBufferedSymbol( wxS( "Part" ), wxS( "Parts" ) )
                                          ->GetValueField().GetText(), wxS( "Edited" ) );
            }

            BOOST_REQUIRE( libraryManager.SaveLibrary( wxS( "Parts" ), fresh, SCH_IO_MGR::SCH_KICAD ) );
            SCH_IO_KICAD_SEXPR reopened;
            LIB_SYMBOL* saved = reopened.LoadSymbol( fresh, wxS( "Part" ) );
            BOOST_REQUIRE( saved );

            if( buffered )
                BOOST_CHECK_EQUAL( saved->GetValueField().GetText(), wxS( "Edited" ) );

            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( official ), officialBefore );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( native ), nativeBefore );

            LIB_BUFFER buffer( wxS( "Parts" ) );
            buffer.Load( official );
            auto edited = buffer.GetBuffer( wxS( "Part" ) );
            BOOST_REQUIRE( edited );
            edited->GetSymbol().GetValueField().SetText( wxS( "Buffer edit" ) );
            edited->GetScreen()->SetContentModified( true );
            SCH_IO_KICAD_SEXPR bufferWriter;
            BOOST_CHECK( !buffer.SaveLibrary( native, &bufferWriter, false ) );
            BOOST_CHECK( buffer.IsModified() );
            BOOST_CHECK_EQUAL( edited->GetSymbol().GetValueField().GetText(), wxS( "Buffer edit" ) );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( official ), officialBefore );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( native ), nativeBefore );
        }
    }
}


BOOST_AUTO_TEST_CASE( AutosavePayloadUsesNativeNamesAndRoot )
{
    // A generated autosave path selects the project profile even if a caller left a
    // legacy document name in memory. It never emits an official-format snapshot.
    sheet->GetScreen()->SetFileName( directory.ChildPathStr( "root.kicad_sch" ) );
    sheet->GetScreen()->SetContentModified( true );
    std::vector<HISTORY_FILE_DATA> data;
    schematic->SaveToHistory( settings.Prj().GetProjectPath(), data );
    BOOST_REQUIRE_EQUAL( data.size(), 1 );
    BOOST_CHECK_EQUAL( wxFileName( data[0].relativePath ).GetExt(), "kemi_sch" );
    BOOST_CHECK( data[0].content.find( "(kemi_sch" ) == 0 );
    BOOST_CHECK( data[0].relativePath.Find( ".kicad_" ) == wxNOT_FOUND );
    BOOST_CHECK( !wxFileExists( directory.ChildPathStr( "root.kicad_sch" ) ) );
}


BOOST_AUTO_TEST_CASE( ManagerSaveAsRenamesManagedNativeLibrariesAndSheetReferences )
{
    const wxString sourceDir = directory.CreateChildDirStr( wxS( "save-as-source" ) );
    const wxString outputDir = directory.CreateChildDirStr( wxS( "save-as-output" ) );
    const wxString vendor = wxFileName( sourceDir, wxS( "vendor" ) ).GetFullPath();
    const wxString copiedVendor = wxFileName( outputDir, wxS( "vendor" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( vendor ) );
    BOOST_REQUIRE( wxFileName::Mkdir( copiedVendor ) );
    const wxString nestedTable = wxFileName( vendor, wxS( "sym-lib-table" ) ).GetFullPath();
    auto write = []( const wxString& aPath, const wxString& aContent )
    {
        wxFFile file( aPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aContent, wxConvUTF8 ) );
        BOOST_REQUIRE( file.Close() );
    };
    write( wxFileName( sourceDir, wxS( "source.kemi_pro" ) ).GetFullPath(), wxS( "{}" ) );
    const wxString sourceLib = wxFileName( sourceDir, wxS( "source.kemi_sym" ) ).GetFullPath();
    SCH_IO_KICAD_SEXPR writer;
    writer.CreateLibrary( sourceLib );
    writer.SaveSymbol( sourceLib, std::make_unique<LIB_SYMBOL>( wxS( "Part" ) ) );
    const wxString pretty = wxFileName( sourceDir, wxS( "source.pretty" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( pretty ) );
    const wxString symTable = wxFileName( sourceDir, wxS( "sym-lib-table" ) ).GetFullPath();
    const wxString fpTable = wxFileName( sourceDir, wxS( "fp-lib-table" ) ).GetFullPath();
    write( symTable, wxS( "(sym_lib_table (version 7)"
            "(lib (name source)(type KiCad)(uri ${KIPRJMOD}/source.kemi_sym)"
            "(options kicad_import_cache=1)(descr \"\"))"
            "(lib (name kept_alias)(type KiCad)(uri ${KIPRJMOD}/./source.kemi_sym)"
            "(options \"\")(descr \"\"))"
            "(lib (name external)(type KiCad)(uri ${KIPRJMOD}/vendor/source.kemi_sym)"
            "(options kicad_import_cache=1)(descr \"\")))" ) );
    write( fpTable, wxS( "(fp_lib_table (version 7)"
            "(lib (name source)(type KiCad)(uri ${KIPRJMOD}/source.pretty)"
            "(options kicad_import_cache=1)(descr \"\")))" ) );

    write( nestedTable, wxS( "(sym_lib_table\r\n  (version 7)\r\n"
            "  (lib (name source)(type KiCad)(uri ${KIPRJMOD}/vendor/unmanaged.kemi_sym)"
            "(options \"\")(descr \"third-party formatting\")))\r\n" ) );
    const std::string nestedBefore = KI_TEST::LoadStringData( nestedTable );

    const wxString contents = wxS( "(kemi_sch (format_version 1)"
            "(lib_symbols(symbol \"source:Part\")(symbol \"external:Part\"))"
            "(symbol(lib_id \"source:Part\")(property \"Footprint\" \"source:Pad\")"
            "(property \"Value\" \"source:keep\")(instances(project source)))"
            "(symbol(lib_id \"external:Part\")(property \"Footprint\" \"external:Pad\")))" );
    const wxString source = wxFileName( sourceDir, wxS( "source.kemi_sch" ) ).GetFullPath();
    const wxString child = wxFileName( sourceDir, wxS( "02-child.kemi_sch" ) ).GetFullPath();
    write( source, contents );
    write( child, contents );
    for( const wxString& tablePath : { symTable, fpTable } )
    {
        LIBRARY_TABLE parsed( wxFileName( tablePath ), LIBRARY_TABLE_SCOPE::PROJECT );
        BOOST_REQUIRE_MESSAGE( parsed.IsOk(), parsed.ErrorDescription() );
    }

    const DIRECTORY_SNAPSHOT before = snapshotDirectory( ToFsPath( sourceDir ) );
    wxString errors;

    for( const wxString& file : { sourceLib, symTable, source, child, nestedTable } )
        EESCHEMA_SaveFileAs( sourceDir, wxS( "source" ), outputDir, wxS( "copy" ), file, errors );

    BOOST_CHECK_MESSAGE( errors.empty(), errors );
    const wxString newLibrary = wxFileName( outputDir, wxS( "copy.kemi_sym" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileExists( newLibrary ) );
    BOOST_CHECK( !wxFileExists( wxFileName( outputDir, wxS( "source.kemi_sym" ) ).GetFullPath() ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( newLibrary ), KI_TEST::LoadStringData( sourceLib ) );
    SCH_IO_KICAD_SEXPR reader;
    BOOST_REQUIRE( reader.LoadSymbol( newLibrary, wxS( "Part" ) ) );

    LIBRARY_TABLE table( wxFileName( outputDir, wxS( "sym-lib-table" ) ),
                         LIBRARY_TABLE_SCOPE::PROJECT );
    BOOST_REQUIRE( table.IsOk() );
    auto* renamed = table.Row( wxS( "copy" ) ).value_or( nullptr );
    BOOST_REQUIRE( renamed );
    BOOST_CHECK_EQUAL( renamed->URI(), wxS( "${KIPRJMOD}/copy.kemi_sym" ) );
    BOOST_CHECK( !table.HasRow( wxS( "source" ) ) );
    auto* external = table.Row( wxS( "external" ) ).value_or( nullptr );
    BOOST_REQUIRE( external );
    BOOST_CHECK_EQUAL( external->URI(), wxS( "${KIPRJMOD}/vendor/source.kemi_sym" ) );
    auto* alias = table.Row( wxS( "kept_alias" ) ).value_or( nullptr );
    BOOST_REQUIRE( alias );
    BOOST_CHECK_EQUAL( alias->URI(), wxS( "${KIPRJMOD}/copy.kemi_sym" ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData(
            wxFileName( copiedVendor, wxS( "sym-lib-table" ) ).GetFullPath() ), nestedBefore );

    for( const wxString& name : { wxString( "copy.kemi_sch" ), wxString( "02-child.kemi_sch" ) } )
    {
        const std::string copied = KI_TEST::LoadStringData( wxFileName( outputDir, name ).GetFullPath() );
        BOOST_CHECK( copied.find( "(symbol \"copy:Part\"" ) != std::string::npos );
        BOOST_CHECK( copied.find( "(lib_id \"copy:Part\")" ) != std::string::npos );
        BOOST_CHECK( copied.find( "(property \"Footprint\" \"copy:Pad\"" ) != std::string::npos );
        BOOST_CHECK( copied.find( "(lib_id \"external:Part\")" ) != std::string::npos );
        BOOST_CHECK( copied.find( "(property \"Footprint\" \"external:Pad\"" ) != std::string::npos );
        BOOST_CHECK( copied.find( "(property \"Value\" \"source:keep\"" ) != std::string::npos );
    }

    BOOST_CHECK( snapshotDirectory( ToFsPath( sourceDir ) ) == before );
}


BOOST_AUTO_TEST_CASE( ManagerSaveAsPreservesUnmanagedAndOfficialSymbolLibraries )
{
    for( bool native : { false, true } )
    {
        const wxString sourceDir = directory.CreateChildDirStr( native ? wxS( "unmanaged" )
                                                                      : wxS( "official" ) );
        const wxString outputDir = directory.CreateChildDirStr( native ? wxS( "unmanaged-copy" )
                                                                      : wxS( "official-copy" ) );
        const wxString extension = native ? wxS( "kemi_sym" ) : wxS( "kicad_sym" );
        const wxString source = wxFileName( sourceDir, wxS( "source" ), extension ).GetFullPath();
        SCH_IO_KICAD_SEXPR writer;
        if( native )
        {
            writer.CreateLibrary( source );
            writer.SaveSymbol( source, std::make_unique<LIB_SYMBOL>( wxS( "Part" ) ) );
        }
        else
        {
            wxFFile fixture( source, wxS( "wb" ) );
            BOOST_REQUIRE( fixture.IsOpened() );
            BOOST_REQUIRE( fixture.Write( wxS( "(kicad_symbol_lib (version 20260830) (generator eeschema))" ) ) );
        }
        const wxString tablePath = wxFileName( sourceDir, wxS( "sym-lib-table" ) ).GetFullPath();
        {
            wxFFile file( tablePath, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            const wxString table = wxS( "(sym_lib_table (version 7)(lib (name source)(type KiCad)"
                    "(uri ${KIPRJMOD}/source." ) + extension
                    + wxS( ")(options \"\")(descr \"\")))" );
            BOOST_REQUIRE( file.Write( table, wxConvUTF8 ) );
            BOOST_REQUIRE( file.Close() );
        }
        if( native )
        {
            wxFFile project( wxFileName( sourceDir, wxS( "source.kemi_pro" ) ).GetFullPath(), wxS( "wb" ) );
            BOOST_REQUIRE( project.IsOpened() );
            BOOST_REQUIRE( project.Write( wxS( "{}" ), wxConvUTF8 ) );
        }
        wxString errors;
        EESCHEMA_SaveFileAs( sourceDir, wxS( "source" ), outputDir, wxS( "copy" ), source, errors );
        EESCHEMA_SaveFileAs( sourceDir, wxS( "source" ), outputDir, wxS( "copy" ), tablePath, errors );
        BOOST_CHECK_MESSAGE( errors.empty(), errors );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData(
                wxFileName( outputDir, wxS( "source" ), extension ).GetFullPath() ),
                KI_TEST::LoadStringData( source ) );
        BOOST_CHECK( !wxFileExists( wxFileName( outputDir, wxS( "copy" ), extension ).GetFullPath() ) );
        LIBRARY_TABLE table( wxFileName( outputDir, wxS( "sym-lib-table" ) ),
                             LIBRARY_TABLE_SCOPE::PROJECT );
        BOOST_REQUIRE( table.IsOk() );
        BOOST_CHECK( table.HasRow( wxS( "source" ) ) );
        BOOST_CHECK( !table.HasRow( wxS( "copy" ) ) );
    }
}


BOOST_AUTO_TEST_CASE( ManagerSaveAsAcceptsUppercaseSchematicExtension )
{
    SETTINGS_MANAGER settings;
    KI_TEST::SCOPED_TEMP_PROJECT project( settings, wxS( "kemi-manager-uppercase-sch" ), wxS( "source" ) );
    KI_TEST::SCOPED_TEMP_DIR output( wxS( "kemi-manager-uppercase-output" ) );
    SCHEMATIC schematic( &project.Project() );
    SCH_SHEET* sheet = schematic.GetTopLevelSheet( 0 );
    BOOST_REQUIRE( sheet );
    const wxString source = wxFileName( project.DirStr(), wxS( "source.KEMI_SCH" ) ).GetFullPath();
    sheet->SetFileName( source );
    sheet->GetScreen()->SetFileName( source );
    schematic.RefreshHierarchy();
    SCH_IO_KICAD_SEXPR io;
    io.SaveSchematicFile( source, sheet, &schematic );
    wxString errors;
    EESCHEMA_SaveFileAs( project.DirStr(), wxS( "source" ), output.PathStr(), wxS( "copy" ),
                         source, errors );
    BOOST_CHECK( errors.empty() );
    const wxString target = output.ChildPathStr( wxS( "copy.KEMI_SCH" ) );
    BOOST_REQUIRE( wxFileExists( target ) );
    SCHEMATIC restored( &project.Project() );
    std::unique_ptr<SCH_SHEET> loaded( io.LoadSchematicFile( target, &restored ) );
    BOOST_REQUIRE( loaded );
    BOOST_REQUIRE( loaded->GetScreen() );
    BOOST_CHECK( loaded->GetScreen()->IsKemiOnly() );
}

BOOST_AUTO_TEST_SUITE_END()



// Phase-6 P1: library provenance alone must prevent embedding native symbols in official files.
BOOST_AUTO_TEST_CASE( KemiSymbolRestrictsLegacySchematicOutput )
{
    SETTINGS_MANAGER settings;
    KI_TEST::SCOPED_TEMP_PROJECT project( settings, wxS( "kemi-symbol-output" ), wxS( "legacy" ) );
    SCHEMATIC schematic( &project.Project() );
    SCH_SHEET* sheet = schematic.GetTopLevelSheet( 0 );
    BOOST_REQUIRE( sheet );
    const wxString official = wxFileName( project.DirStr(), wxS( "root.kicad_sch" ) ).GetFullPath();
    sheet->SetFileName( official );
    sheet->GetScreen()->SetFileName( official );
    schematic.RefreshHierarchy();
    BOOST_REQUIRE( schematic.IsKemiOnly() );

    auto symbol = std::make_unique<LIB_SYMBOL>( wxS( "Native" ) );
    symbol->SetKemiOnly();
    sheet->GetScreen()->AddLibSymbol( wxS( "Native" ), std::move( symbol ) );
    BOOST_REQUIRE( schematic.IsKemiOnly() );

    SCH_IO_KICAD_SEXPR writer;
    BOOST_CHECK_THROW( writer.SaveSchematicFile( official, sheet, &schematic ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( official ) );
    BOOST_CHECK( !SCH_API_SAVE::SaveSchematicCopy( schematic, project.Project(), official, true ) );
    BOOST_CHECK( !wxFileExists( official ) );
    const wxString native = wxFileName( project.DirStr(), wxS( "root.kemi_sch" ) ).GetFullPath();
    BOOST_CHECK_NO_THROW( writer.SaveSchematicFile( native, sheet, &schematic ) );
    BOOST_CHECK( KI_TEST::LoadStringData( native ).starts_with( "(kemi_sch" ) );
}


// Phase-6 P1: a native JSON project marker remains binding under an official extension.
BOOST_AUTO_TEST_CASE( RenamedKemiProjectRestrictsLegacySchematicOutput )
{
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "kemi-renamed-project-output" ) );
    const wxString nativeProject = directory.ChildPathStr( wxS( "source.kemi_pro" ) );
    PROJECT seedProject;
    PROJECT_FILE seedFile( nativeProject );
    seedFile.SetProject( &seedProject );
    BOOST_REQUIRE( seedFile.SaveToFile( wxEmptyString, true ) );
    const wxString renamed = directory.ChildPathStr( wxS( "renamed.kicad_pro" ) );
    BOOST_REQUIRE( wxCopyFile( nativeProject, renamed ) );

    SETTINGS_MANAGER settings;
    BOOST_REQUIRE( settings.LoadProject( renamed, true ) );
    SCHEMATIC schematic( &settings.Prj() );
    SCH_SHEET* sheet = schematic.GetTopLevelSheet( 0 );
    BOOST_REQUIRE( sheet );
    const wxString official = directory.ChildPathStr( wxS( "root.kicad_sch" ) );
    sheet->SetFileName( official );
    sheet->GetScreen()->SetFileName( official );
    schematic.RefreshHierarchy();
    BOOST_REQUIRE( settings.Prj().GetProjectFile().IsKemiOnly() );
    BOOST_REQUIRE( !sheet->GetScreen()->IsKemiOnly() );
    BOOST_CHECK( schematic.IsKemiOnly() );

    SCH_IO_KICAD_SEXPR writer;
    BOOST_CHECK_THROW( writer.SaveSchematicFile( official, sheet, &schematic ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( official ) );
    BOOST_CHECK_NO_THROW( writer.SaveSchematicFile( directory.ChildPathStr( wxS( "root.kemi_sch" ) ),
                                                   sheet, &schematic ) );
}

BOOST_AUTO_TEST_CASE( OfficialSchematicFirstSaveCreatesNativeSiblingAndPreservesSource )
{
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "kemi-first-sch-save" ) );
    const wxString fixture = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() )
                             + wxS( "/NoConnectOnPin.kicad_sch" );
    const wxString source = directory.ChildPathStr( wxS( "source.kicad_sch" ) );
    const wxString target = directory.ChildPathStr( wxS( "source.kemi_sch" ) );
    BOOST_REQUIRE( wxCopyFile( fixture, source ) );
    const std::string before = KI_TEST::LoadStringData( source );
    PROJECT project;
    SCHEMATIC schematic( &project );
    SCH_IO_KICAD_SEXPR io;
    SCH_SHEET* loaded = io.LoadSchematicFile( source, &schematic );
    BOOST_REQUIRE( loaded );
    schematic.SetTopLevelSheets( { loaded } );
    BOOST_REQUIRE( SCH_API_SAVE::SaveSchematic( schematic, project ) );
    BOOST_CHECK_EQUAL( schematic.RootScreen()->GetFileName(), target );
    BOOST_CHECK( KI_TEST::LoadStringData( target ).starts_with( "(kemi_sch" ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
    const std::string nativeBefore = KI_TEST::LoadStringData( target );

    SCHEMATIC conflicting( &project );
    loaded = io.LoadSchematicFile( source, &conflicting );
    BOOST_REQUIRE( loaded );
    conflicting.SetTopLevelSheets( { loaded } );
    BOOST_CHECK( !SCH_API_SAVE::SaveSchematic( conflicting, project ) );
    BOOST_CHECK( !SCH_API_SAVE::SaveSchematicAs( conflicting, project, target ) );
    BOOST_CHECK( !SCH_API_SAVE::SaveSchematicCopy( conflicting, project, target, false ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( target ), nativeBefore );
    BOOST_CHECK_EQUAL( conflicting.RootScreen()->GetFileName(), source );

    // A failed temporary write must leave both source and existing native output intact.
    const wxString impossible = wxFileName( source, wxS( "blocked.kemi_sch" ) ).GetFullPath();
    BOOST_CHECK_THROW( io.SaveSchematicFile( impossible, conflicting.GetTopLevelSheet(), &conflicting ), IO_ERROR );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( target ), nativeBefore );
}


BOOST_AUTO_TEST_CASE( ReadOnlyOfficialSchematicSavesToWritableNativeSibling )
{
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "kemi-readonly-sch-save" ) );
    const wxString fixture = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() )
                             + wxS( "/NoConnectOnPin.kicad_sch" );
    const wxString source = directory.ChildPathStr( wxS( "source.kicad_sch" ) );
    const wxString target = directory.ChildPathStr( wxS( "source.kemi_sch" ) );
    BOOST_REQUIRE( wxCopyFile( fixture, source ) );
    const std::string before = KI_TEST::LoadStringData( source );
    const std::filesystem::path sourcePath = directory.Path() / "source.kicad_sch";
    const auto sourceTime = std::filesystem::last_write_time( sourcePath );

    // Restore permissions even if an assertion aborts, so Windows can clean up the fixture.
    struct PERMISSIONS_RESTORE
    {
        std::filesystem::path path;
        std::filesystem::perms permissions;

        ~PERMISSIONS_RESTORE()
        {
            std::error_code error;
            std::filesystem::permissions( path, permissions, error );
        }
    } restore{ sourcePath, std::filesystem::status( sourcePath ).permissions() };

    std::filesystem::permissions( sourcePath,
            std::filesystem::perms::owner_write | std::filesystem::perms::group_write
                    | std::filesystem::perms::others_write,
            std::filesystem::perm_options::remove );
    const auto sourcePermissions = std::filesystem::status( sourcePath ).permissions();
    BOOST_REQUIRE( ( sourcePermissions & std::filesystem::perms::owner_write )
                   == std::filesystem::perms::none );

    PROJECT project;
    SCHEMATIC schematic( &project );
    SCH_IO_KICAD_SEXPR io;
    SCH_SHEET* loaded = io.LoadSchematicFile( source, &schematic );
    BOOST_REQUIRE( loaded );
    schematic.SetTopLevelSheets( { loaded } );
    loaded->GetScreen()->Append( new SCH_TEXT( VECTOR2I( 1000, 2000 ), wxS( "read-only source edit" ) ) );
    loaded->GetScreen()->SetContentModified();

    BOOST_REQUIRE( SCH_API_SAVE::SaveSchematic( schematic, project ) );
    BOOST_CHECK_EQUAL( schematic.RootScreen()->GetFileName(), target );
    BOOST_CHECK( !schematic.RootScreen()->IsContentModified() );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
    BOOST_CHECK( std::filesystem::last_write_time( sourcePath ) == sourceTime );
    BOOST_CHECK( std::filesystem::status( sourcePath ).permissions() == sourcePermissions );
    const std::string saved = KI_TEST::LoadStringData( target );
    BOOST_CHECK( saved.starts_with( "(kemi_sch" ) );
    BOOST_CHECK( saved.find( "read-only source edit" ) != std::string::npos );

    SCHEMATIC reopened( &project );
    loaded = io.LoadSchematicFile( target, &reopened );
    BOOST_REQUIRE( loaded );
    reopened.SetTopLevelSheets( { loaded } );
    BOOST_CHECK( !reopened.RootScreen()->HasLoadError() );
}


BOOST_AUTO_TEST_CASE( SaveAsChildConflictRestoresOriginalRootAndDirtyFlags )
{
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "sch-save-rollback" ) );
    PROJECT project;
    SCHEMATIC schematic( &project );
    SCH_SHEET* root = schematic.GetTopLevelSheet();
    const wxString original = directory.ChildPathStr( wxS( "root.kicad_sch" ) );
    const wxString rootReference = wxS( "root.kicad_sch" );
    root->SetFileName( rootReference );
    root->GetScreen()->SetFileName( original );
    root->GetScreen()->SetContentModified();
    SCH_SHEET* child = new SCH_SHEET( &schematic );
    child->SetFileName( wxS( "child.kicad_sch" ) );
    child->SetScreen( new SCH_SCREEN( &schematic ) );
    child->GetScreen()->SetFileName( directory.ChildPathStr( wxS( "child.kicad_sch" ) ) );
    child->GetScreen()->SetContentModified();
    root->GetScreen()->Append( child );
    schematic.RefreshHierarchy();
    const wxString collision = directory.ChildPathStr( wxS( "child.kemi_sch" ) );
    {
        wxFFile file( collision, wxS( "wb" ) );
        BOOST_REQUIRE( file.Write( wxS( "sentinel" ) ) );
    }
    BOOST_CHECK( !SCH_API_SAVE::SaveSchematicAs( schematic, project,
            directory.ChildPathStr( wxS( "renamed.kemi_sch" ) ) ) );
    BOOST_CHECK_EQUAL( root->GetScreen()->GetFileName(), original );
    BOOST_CHECK_EQUAL( root->GetFileName(), rootReference );
    BOOST_CHECK_EQUAL( child->GetFileName(), wxS( "child.kicad_sch" ) );
    BOOST_CHECK( root->GetScreen()->IsContentModified() );
    BOOST_CHECK( child->GetScreen()->IsContentModified() );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( collision ), "sentinel" );
}


BOOST_AUTO_TEST_CASE( SharedHierarchySnapshotRestoresReferencesAndLateSaveFailure )
{
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "sch-shared-rollback" ) );
    PROJECT project;
    SCHEMATIC schematic( &project );
    SCH_SHEET* root = schematic.GetTopLevelSheet();
    root->SetFileName( wxS( "root.kemi_sch" ) );
    root->GetScreen()->SetFileName( directory.ChildPathStr( wxS( "root.kemi_sch" ) ) );
    root->GetScreen()->SetContentModified();
    SCH_SCREEN* shared = new SCH_SCREEN( &schematic );
    shared->SetFileName( directory.ChildPathStr( wxS( "shared.kemi_sch" ) ) );
    shared->SetContentModified();
    for( int i = 0; i < 2; ++i )
    {
        SCH_SHEET* parent = new SCH_SHEET( &schematic );
        parent->SetFileName( wxS( "shared.kemi_sch" ) );
        parent->SetScreen( shared );
        root->GetScreen()->Append( parent );
    }
    SCH_SHEET* leaf = new SCH_SHEET( &schematic );
    leaf->SetFileName( wxS( "leaf.kicad_sch" ) );
    leaf->SetScreen( new SCH_SCREEN( &schematic ) );
    leaf->GetScreen()->SetFileName( directory.ChildPathStr( wxS( "leaf.kicad_sch" ) ) );
    leaf->GetScreen()->SetContentModified();
    shared->Append( leaf );
    schematic.RefreshHierarchy();
    {
        // The same leaf occurs in two paths. Capture it once, before Save Copy preparation.
        SCH_SAVE_STATE state( schematic );
        for( const SCH_SHEET_PATH& path : schematic.Hierarchy() )
            path.Last()->SetFileName( wxS( "copy.kemi_sch" ) );
        root->GetScreen()->SetContentModified( false );
        shared->SetContentModified( false );
    }
    BOOST_CHECK_EQUAL( leaf->GetFileName(), wxS( "leaf.kicad_sch" ) );
    BOOST_CHECK( root->GetScreen()->IsContentModified() );
    BOOST_CHECK( shared->IsContentModified() );

    // A native filename occupied by a directory fails during writing, after earlier pages save.
    const wxString blocked = directory.CreateChildDirStr( wxS( "blocked.kemi_sch" ) );
    leaf->GetScreen()->SetFileName( blocked );
    const wxString rootFilename = root->GetScreen()->GetFileName();
    BOOST_CHECK( !SCH_API_SAVE::SaveSchematic( schematic, project ) );
    BOOST_CHECK_EQUAL( root->GetScreen()->GetFileName(), rootFilename );
    BOOST_CHECK_EQUAL( leaf->GetFileName(), wxS( "leaf.kicad_sch" ) );
    BOOST_CHECK( root->GetScreen()->IsContentModified() );
    BOOST_CHECK( shared->IsContentModified() );
    BOOST_CHECK( leaf->GetScreen()->IsContentModified() );
}


BOOST_AUTO_TEST_CASE( DesignBlockConvertsOfficialSchematicAndReopensNativeMember )
{
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "kemi-sch-design-block" ) );
    const wxString fixture = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() )
                             + wxS( "/NoConnectOnPin.kicad_sch" );
    const wxString source = directory.ChildPathStr( wxS( "source.kicad_sch" ) );
    const wxString library = directory.CreateChildDirStr( wxS( "blocks.kicad_blocks" ) );
    BOOST_REQUIRE( wxCopyFile( fixture, source ) );
    const std::string before = KI_TEST::LoadStringData( source );
    int conversions = 0;
    DESIGN_BLOCK_IO blocks( [&]( KEMI_FORMAT::DOC aDoc, const wxString& aSource, const wxString& aTarget )
    {
        BOOST_CHECK( aDoc == KEMI_FORMAT::DOC::SCHEMATIC );
        ++conversions;
        PROJECT project;
        SCHEMATIC schematic( &project );
        SCH_IO_KICAD_SEXPR io;
        SCH_SHEET* sheet = io.LoadSchematicFile( aSource, &schematic );
        BOOST_REQUIRE( sheet );
        schematic.SetTopLevelSheets( { sheet } );
        io.SaveSchematicFile( aTarget, sheet, &schematic );
        return true;
    } );
    DESIGN_BLOCK block;
    block.SetLibId( LIB_ID( wxS( "Blocks" ), wxS( "Imported" ) ) );
    block.SetSchematicFile( source );
    blocks.DesignBlockSave( library, &block );
    BOOST_CHECK_EQUAL( conversions, 1 );
    std::unique_ptr<DESIGN_BLOCK> reopened( blocks.DesignBlockLoad( library, wxS( "Imported" ) ) );
    BOOST_REQUIRE( reopened );
    BOOST_CHECK_EQUAL( wxFileName( reopened->GetSchematicFile() ).GetExt(), wxS( "kemi_sch" ) );
    BOOST_CHECK( KI_TEST::LoadStringData( reopened->GetSchematicFile() ).starts_with( "(kemi_sch" ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );

    // Metadata saves preserve the native document bytes and never revisit the import source.
    const std::string native = KI_TEST::LoadStringData( reopened->GetSchematicFile() );
    blocks.DesignBlockSave( library, reopened.get() );
    BOOST_CHECK_EQUAL( conversions, 1 );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( reopened->GetSchematicFile() ), native );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
}


namespace
{

class AUTOMATIC_TEST_RESCUER : public SYMBOL_LIB_TABLE_RESCUER
{
public:
    AUTOMATIC_TEST_RESCUER( PROJECT& aProject, SCHEMATIC& aSchematic, LIB_SYMBOL* aCached ) :
            SYMBOL_LIB_TABLE_RESCUER( aProject, &aSchematic, nullptr,
                                      EDA_DRAW_PANEL_GAL::GAL_TYPE_NONE ),
            m_cached( aCached )
    {}

    void FindCandidates() override
    {
        m_all_candidates.push_back( new RESCUE_SYMBOL_LIB_TABLE_CANDIDATE(
                LIB_ID( wxS( "Original" ), wxS( "Part" ) ),
                LIB_ID( wxS( "source-rescue" ), wxS( "Part-Original" ) ), m_cached, nullptr ) );
    }

    void InvokeDialog( wxWindow*, bool ) override
    {
        m_chosen_candidates.push_back( &m_all_candidates.front() );
    }

private:
    LIB_SYMBOL* m_cached;
};

class AUTOMATIC_LEGACY_TEST_RESCUER : public LEGACY_RESCUER
{
public:
    AUTOMATIC_LEGACY_TEST_RESCUER( PROJECT& aProject, SCHEMATIC* aSchematic ) :
            LEGACY_RESCUER( aProject, aSchematic, nullptr, EDA_DRAW_PANEL_GAL::GAL_TYPE_NONE ),
            m_cached( wxS( "Part" ) )
    {}

    void FindCandidates() override
    {
        m_all_candidates.push_back( new RESCUE_CASE_CANDIDATE(
                wxS( "part" ), wxS( "Part" ), &m_cached, 1, 1 ) );
    }

    void InvokeDialog( wxWindow*, bool ) override
    {
        m_chosen_candidates.push_back( &m_all_candidates.front() );
    }

private:
    LIB_SYMBOL m_cached;
};

} // namespace


BOOST_AUTO_TEST_CASE( LegacyRescueRejectsPersistentOutputAndRestoresUnqualifiedIds )
{
    for( bool existingLibrary : { false, true } )
    {
        KI_TEST::SCOPED_TEMP_DIR temp( "kemi-legacy-rescue" );
        const wxString source = temp.ChildPathStr( "source.sch" );
        const wxString library = temp.ChildPathStr( "source-rescue.lib" );
        {
            wxFFile file( source, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write( wxS( "legacy schematic source sentinel" ) ) );
        }

        if( existingLibrary )
        {
            wxFFile file( library, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write( wxS( "legacy rescue library sentinel" ) ) );
        }

        const auto before = snapshotDirectory( temp.Path() );
        sheet->GetScreen()->SetFileName( source );
        auto* symbol = new SCH_SYMBOL();
        const LIB_ID originalId( wxEmptyString, wxS( "part" ) );
        symbol->SetLibId( originalId );
        sheet->GetScreen()->Append( symbol );
        schematic->RefreshHierarchy();

        settings.Prj().SetElem( PROJECT::ELEM::LEGACY_SYMBOL_LIBS, new LEGACY_SYMBOL_LIBS() );
        AUTOMATIC_LEGACY_TEST_RESCUER rescuer( settings.Prj(), schematic.get() );
        wxLogNull quiet;
        BOOST_CHECK( !RESCUER::RescueProject( nullptr, rescuer, false ) );
        BOOST_CHECK( symbol->GetLibId() == originalId );
        BOOST_CHECK( snapshotDirectory( temp.Path() ) == before );
    }
}


BOOST_AUTO_TEST_CASE( RescueMigratesOfficialLibraryAndRollsBackFailures )
{
    for( const wxString& scenario : { wxS( "success" ), wxS( "collision" ), wxS( "table-failure" ) } )
    {
        BOOST_TEST_CONTEXT( scenario )
        {
            SETTINGS_MANAGER& settings = Pgm().GetSettingsManager();
            KI_TEST::SCOPED_TEMP_PROJECT project( settings, wxS( "kemi-rescue" ), wxS( "source" ) );
            const wxString official = wxFileName( project.DirStr(), wxS( "source-rescue.kicad_sym" ) ).GetFullPath();
            const wxString native = wxFileName( project.DirStr(), wxS( "source-rescue.kemi_sym" ) ).GetFullPath();
            const wxString tablePath = wxFileName( project.DirStr(), wxS( "sym-lib-table" ) ).GetFullPath();
            {
                wxFFile library( official, wxS( "wb" ) );
                BOOST_REQUIRE( library.IsOpened() );
                BOOST_REQUIRE( library.Write( wxS( "(kicad_symbol_lib (version 20241209)"
                        " (generator kicad_symbol_editor) (symbol \"Part\") (symbol \"Keep\"))" ), wxConvUTF8 ) );
                wxFFile table( tablePath, wxS( "wb" ) );
                BOOST_REQUIRE( table.IsOpened() );
                BOOST_REQUIRE( table.Write( wxS( "(sym_lib_table (version 7)"
                        " (lib (name \"source-rescue\") (type \"KiCad\")"
                        " (uri \"${KIPRJMOD}/source-rescue.kicad_sym\") (options \"\") (descr \"\")))" ),
                                            wxConvUTF8 ) );
            }

            const std::string sourceBefore = KI_TEST::LoadStringData( official );
            const std::string tableBefore = KI_TEST::LoadStringData( tablePath );
            LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();
            manager.LoadProjectTables( project.DirStr(), { LIBRARY_TABLE_TYPE::SYMBOL } );
            SCHEMATIC schematic( &project.Project() );
            SCH_SHEET* root = schematic.GetTopLevelSheet( 0 );
            BOOST_REQUIRE( root );
            root->SetFileName( wxS( "source.kicad_sch" ) );
            root->GetScreen()->SetFileName( wxFileName( project.DirStr(), wxS( "source.kicad_sch" ) ).GetFullPath() );
            schematic.RefreshHierarchy();
            SCH_IO_KICAD_SEXPR reader;
            LIB_SYMBOL* cached = reader.LoadSymbol( official, wxS( "Part" ) );
            BOOST_REQUIRE( cached );
            auto* symbol = new SCH_SYMBOL();
            const LIB_ID originalId( wxS( "Original" ), wxS( "Part" ) );
            symbol->SetLibId( originalId );
            symbol->SetLibSymbol( new LIB_SYMBOL( *cached ) );
            root->GetScreen()->Append( symbol );

            if( scenario == wxS( "collision" ) )
            {
                wxFFile conflict( native, wxS( "wb" ) );
                BOOST_REQUIRE( conflict.IsOpened() );
                BOOST_REQUIRE( conflict.Write( wxS( "unrelated native target" ), wxConvUTF8 ) );
            }
            else if( scenario == wxS( "table-failure" ) )
            {
                auto table = manager.Table( LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_SCOPE::PROJECT );
                BOOST_REQUIRE( table );
                ( *table )->SetPath( wxFileName( project.DirStr() + wxFILE_SEP_PATH + wxS( "missing" ),
                                                 wxS( "sym-lib-table" ) ).GetFullPath() );
            }

            AUTOMATIC_TEST_RESCUER rescuer( project.Project(), schematic, cached );
            wxLogNull quiet;
            const bool success = RESCUER::RescueProject( nullptr, rescuer, false );
            BOOST_CHECK_EQUAL( success, scenario == wxS( "success" ) );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( official ), sourceBefore );

            if( success )
            {
                SCH_IO_KICAD_SEXPR reopened;
                BOOST_REQUIRE( reopened.LoadSymbol( native, wxS( "Part-Original" ) ) );
                BOOST_REQUIRE( reopened.LoadSymbol( native, wxS( "Keep" ) ) );
                BOOST_CHECK_EQUAL( symbol->GetLibId().Format(), wxS( "source-rescue:Part-Original" ) );
                BOOST_CHECK( KI_TEST::LoadStringData( native ).starts_with( "(kemi_symbol_lib" ) );
                LIBRARY_TABLE saved( wxFileName( tablePath ), LIBRARY_TABLE_SCOPE::PROJECT );
                auto row = saved.Row( wxS( "source-rescue" ) );
                BOOST_REQUIRE( row );
                BOOST_CHECK_EQUAL( ( *row )->URI(), wxS( "${KIPRJMOD}/source-rescue.kemi_sym" ) );
            }
            else
            {
                BOOST_CHECK( symbol->GetLibId() == originalId );
                BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( tablePath ), tableBefore );
                auto table = manager.Table( LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_SCOPE::PROJECT );
                BOOST_REQUIRE( table );
                auto row = ( *table )->Row( wxS( "source-rescue" ) );
                BOOST_REQUIRE( row );
                BOOST_CHECK_EQUAL( ( *row )->URI(), wxS( "${KIPRJMOD}/source-rescue.kicad_sym" ) );

                if( scenario == wxS( "collision" ) )
                    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( native ), "unrelated native target" );
                else if( scenario == wxS( "table-failure" ) )
                {
                    BOOST_CHECK( !wxFileExists( native ) );
                    ( *table )->SetPath( tablePath );
                    AUTOMATIC_TEST_RESCUER retry( project.Project(), schematic, cached );
                    BOOST_REQUIRE( RESCUER::RescueProject( nullptr, retry, false ) );
                    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( official ), sourceBefore );
                    SCH_IO_KICAD_SEXPR reopened;
                    BOOST_REQUIRE( reopened.LoadSymbol( native, wxS( "Part-Original" ) ) );
                    BOOST_REQUIRE( reopened.LoadSymbol( native, wxS( "Keep" ) ) );
                    BOOST_CHECK_EQUAL( symbol->GetLibId().Format(), wxS( "source-rescue:Part-Original" ) );
                    LIBRARY_TABLE saved( wxFileName( tablePath ), LIBRARY_TABLE_SCOPE::PROJECT );
                    auto savedRow = saved.Row( wxS( "source-rescue" ) );
                    BOOST_REQUIRE( savedRow );
                    BOOST_CHECK_EQUAL( ( *savedRow )->URI(), wxS( "${KIPRJMOD}/source-rescue.kemi_sym" ) );
                }
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( EmptyRescueLibraryUsesNativeContainer )
{
    SETTINGS_MANAGER& settings = Pgm().GetSettingsManager();
    KI_TEST::SCOPED_TEMP_PROJECT project( settings, wxS( "kemi-empty-rescue" ), wxS( "source" ) );
    Pgm().GetLibraryManager().LoadProjectTables( project.DirStr(), { LIBRARY_TABLE_TYPE::SYMBOL } );
    const wxString schematicDir = wxFileName( project.DirStr(), wxS( "sheets" ) ).GetFullPath();
    BOOST_REQUIRE( wxMkdir( schematicDir ) );
    SCHEMATIC schematic( &project.Project() );
    SCH_SHEET* root = schematic.GetTopLevelSheet( 0 );
    BOOST_REQUIRE( root );
    root->SetFileName( wxS( "source.kicad_sch" ) );
    root->GetScreen()->SetFileName( wxFileName( schematicDir, wxS( "source.kicad_sch" ) ).GetFullPath() );
    schematic.RefreshHierarchy();
    SYMBOL_LIB_TABLE_RESCUER rescuer( project.Project(), &schematic, nullptr,
                                      EDA_DRAW_PANEL_GAL::GAL_TYPE_NONE );
    rescuer.OpenRescueLibrary();
    BOOST_REQUIRE( rescuer.WriteRescueLibrary( nullptr ) );
    const wxString native = wxFileName( schematicDir, wxS( "source-rescue.kemi_sym" ) ).GetFullPath();
    SCH_IO_KICAD_SEXPR reader;
    wxArrayString names;
    reader.EnumerateSymbolLib( names, native );
    BOOST_CHECK( names.empty() );
    auto table = Pgm().GetLibraryManager().Table( LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_SCOPE::PROJECT );
    BOOST_REQUIRE( table );
    auto row = ( *table )->Row( wxS( "source-rescue" ) );
    BOOST_REQUIRE( row );
    BOOST_CHECK_EQUAL( ( *row )->URI(), wxS( "${KIPRJMOD}/sheets/source-rescue.kemi_sym" ) );
    BOOST_CHECK( KI_TEST::LoadStringData( native ).starts_with( "(kemi_symbol_lib" ) );
}
