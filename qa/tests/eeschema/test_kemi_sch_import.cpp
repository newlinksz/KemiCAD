/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <cli/exit_codes.h>
#include <drawing_sheet/ds_data_model.h>
#include <eeschema_helpers.h>
#include <eeschema_jobs_handler.h>
#include <gestfich.h>
#include <jobs/job_sch_import.h>
#include <kemi_format_profile.h>
#include <ki_exception.h>
#include <lib_symbol.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <libraries/symbol_library_adapter.h>
#include <pgm_base.h>
#include <project.h>
#include <project/project_file.h>
#include <project_sch.h>
#include <reporter.h>
#include <save_project_utils.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_io/eagle/sch_io_eagle.h>
#include <sch_io/pads/sch_io_pads.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <schematic.h>
#include <settings/settings_manager.h>
#include <symbol_import_reconciler.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <wx/filefn.h>

namespace
{
using DIRECTORY_SNAPSHOT = std::map<std::filesystem::path, std::string>;


std::string readBytes( const std::filesystem::path& aPath )
{
    std::ifstream file( aPath, std::ios::binary );
    BOOST_REQUIRE( file.good() );
    return { std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() };
}


DIRECTORY_SNAPSHOT snapshotDirectory( const wxString& aDirectory )
{
    const std::filesystem::path directory = ToFsPath( aDirectory );
    DIRECTORY_SNAPSHOT snapshot;

    for( const auto& entry : std::filesystem::recursive_directory_iterator( directory ) )
    {
        if( entry.is_regular_file() )
            snapshot.emplace( entry.path().lexically_relative( directory ), readBytes( entry.path() ) );
    }

    return snapshot;
}


void checkNoOfficialFiles( const wxString& aDirectory )
{
    for( const auto& entry : std::filesystem::recursive_directory_iterator( ToFsPath( aDirectory ) ) )
    {
        // Inspect the whole suffix so a staging name such as .kicad_sym.tmp also fails.
        if( entry.is_regular_file() )
        {
            BOOST_CHECK_MESSAGE( entry.path().filename().string().find( ".kicad_" ) == std::string::npos,
                                 "Official-format output: " << entry.path() );
        }
    }
}


void checkKemiDocument( const wxString& aPath, KEMI_FORMAT::DOC aDoc )
{
    const std::string bytes = readBytes( ToFsPath( aPath ) );
    const std::string root = "(" + KEMI_FORMAT::RootToken( aDoc, KEMI_FORMAT::FLAVOR::KEMI );
    const std::string version = "(format_version " + std::to_string( KEMI_FORMAT::FORMAT_VERSION ) + ")";
    BOOST_CHECK_EQUAL( bytes.find( root ), 0u );
    BOOST_CHECK( bytes.find( version ) != std::string::npos );
}


struct SCOPED_DRAWING_SHEET
{
    SCOPED_DRAWING_SHEET() { DS_DATA_MODEL::SetAltInstance( &model ); }
    ~SCOPED_DRAWING_SHEET() { DS_DATA_MODEL::SetAltInstance( previous ); }

    DS_DATA_MODEL* previous = &DS_DATA_MODEL::GetTheInstance();
    DS_DATA_MODEL  model;
};


struct KEMI_SCH_IMPORT_FIXTURE
{
    KEMI_SCH_IMPORT_FIXTURE() :
            m_directory( "kemi-sch-import" ),
            m_sourceDirectory( m_directory.CreateChildDirStr( "source" ) ),
            m_outputDirectory( m_directory.CreateChildDirStr( "output" ) ),
            m_settings( Pgm().GetSettingsManager() )
    {
        // A transient native project gives the reconciler a destination without writing JSON.
        const wxString projectPath = wxFileName( m_outputDirectory, "import.kemi_pro" ).GetFullPath();
        m_settings.LoadProject( projectPath, true );
        m_project = m_settings.GetProject( projectPath );
        BOOST_REQUIRE( m_project );
        Pgm().GetLibraryManager().LoadProjectTables( m_outputDirectory );
    }

    ~KEMI_SCH_IMPORT_FIXTURE()
    {
        if( m_project )
            m_settings.UnloadProject( m_project, false );
    }

    wxString stagePads( const wxString& aExtension, const wxString& aDirectory )
    {
        const wxString sample = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() )
                                + "/plugins/pads/binary/multisheet_connectivity." + aExtension;
        const wxString source = wxFileName( aDirectory, "multisheet." + aExtension ).GetFullPath();
        BOOST_REQUIRE( wxCopyFile( sample, source ) );
        return source;
    }

    void checkCache( const SYMBOL_IMPORT_RECONCILE_RESULT& aResult )
    {
        BOOST_REQUIRE( !aResult.m_cacheNickname.IsEmpty() );
        BOOST_CHECK_GT( aResult.m_savedToCache, 0 );
        const wxString extension = KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::SYMBOL_LIB,
                                                    KEMI_FORMAT::FLAVOR::KEMI );
        const wxFileName library( m_outputDirectory, aResult.m_cacheNickname, extension );
        BOOST_REQUIRE( library.FileExists() );
        checkKemiDocument( library.GetFullPath(), KEMI_FORMAT::DOC::SYMBOL_LIB );
        BOOST_CHECK( !wxFileExists( library.GetFullPath() + ".tmp" ) );
        SCH_IO_KICAD_SEXPR reader;
        wxArrayString names;
        reader.EnumerateSymbolLib( names, library.GetFullPath() );
        BOOST_CHECK_GT( names.size(), 0u );

        SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( m_project );
        BOOST_REQUIRE( adapter );
        LIBRARY_TABLE* table = adapter->ProjectTable().value_or( nullptr );
        BOOST_REQUIRE( table );
        LIBRARY_TABLE_ROW* row = table->Row( aResult.m_cacheNickname ).value_or( nullptr );
        BOOST_REQUIRE( row );
        BOOST_CHECK_EQUAL( row->Type(), "KiCad" );
        BOOST_CHECK_EQUAL( row->URI(), "${KIPRJMOD}/" + library.GetFullName() );
    }

    void importAndRoundTrip( const wxString& aExtension, bool aAppend )
    {
        using namespace KEMI_FORMAT;

        const wxString source = stagePads( aExtension, m_sourceDirectory );
        const DIRECTORY_SNAPSHOT before = snapshotDirectory( m_sourceDirectory );
        KEMI_ONLY_OUTPUT_POLICY policy( m_outputDirectory );
        const OUTPUT_TARGET rootTarget = policy.Target( DOC::SCHEMATIC, "converted" );
        auto schematic = std::make_unique<SCHEMATIC>( m_project );
        SCH_SHEET* appendSheet = nullptr;

        if( aAppend )
        {
            appendSheet = schematic->GetTopLevelSheet();
            appendSheet->SetFileName( rootTarget.FinalPath() );
            appendSheet->GetScreen()->SetFileName( rootTarget.FinalPath() );
        }

        SCH_IO_PADS importer;
        importer.SetImportOutputPolicy( &policy );
        BOOST_REQUIRE( importer.LoadSchematicFile( source, schematic.get(), appendSheet ) );
        schematic->RefreshHierarchy();
        const size_t sheetCount = schematic->Hierarchy().size();
        BOOST_REQUIRE_EQUAL( sheetCount, aExtension == "sch" && !aAppend ? 2u : 3u );

        // Assert names before relocation: the save helper must not hide a legacy importer suffix.
        for( const SCH_SHEET_PATH& path : schematic->Hierarchy() )
        {
            SCH_SHEET* sheet = path.Last();

            if( sheet->GetScreen() != schematic->RootScreen() )
                BOOST_CHECK( IsKemiExt( DOC::SCHEMATIC, wxFileName( sheet->GetFileName() ).GetExt() ) );
        }

        BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == before );
        checkNoOfficialFiles( m_outputDirectory );
        PrepareSchematicImportFiles( *schematic, wxFileName( source ), rootTarget );

        WX_STRING_REPORTER reporter;
        const SYMBOL_IMPORT_RECONCILE_RESULT result = ReconcileImportedSymbols(
                importer, *schematic, *m_project, rootTarget.FinalPath(), nullptr, reporter, &policy );
        checkCache( result );
        checkNoOfficialFiles( m_outputDirectory );

        SCH_SCREENS screens( schematic->Root() );
        SCH_IO_KICAD_SEXPR writer;
        size_t written = 0;

        for( size_t i = 0; i < screens.GetCount(); ++i )
        {
            SCH_SHEET* sheet = screens.GetSheet( i );

            if( sheet->IsVirtualRootSheet() )
                continue;

            const wxString path = screens.GetScreen( i )->GetFileName();
            policy.Write( DOC::SCHEMATIC, path,
                    [&]( const wxString& aStorage, const OUTPUT_TARGET& aTarget )
                    {
                        writer.SaveSchematicFile( aStorage, sheet, schematic.get(), aTarget );
                        return true;
                    } );
            checkKemiDocument( path, DOC::SCHEMATIC );
            checkNoOfficialFiles( m_outputDirectory );
            ++written;
        }

        BOOST_CHECK_EQUAL( written, sheetCount );
        BOOST_CHECK_EQUAL( policy.Manifest().size(), sheetCount );
        BOOST_REQUIRE( wxFileExists( rootTarget.FinalPath() ) );
        std::vector<wxString> topLevelFiles;

        for( SCH_SHEET* sheet : schematic->GetTopLevelSheets() )
            topLevelFiles.push_back( sheet->GetScreen()->GetFileName() );

        // Flat binary imports have several roots; reload their files with the same native loader.
        schematic->Reset();
        SCH_IO_KICAD_SEXPR reader;
        std::vector<SCH_SHEET*> loaded;

        for( const wxString& path : topLevelFiles )
        {
            SCH_SHEET* sheet = reader.LoadSchematicFile( path, schematic.get() );
            BOOST_REQUIRE( sheet );
            BOOST_CHECK( reader.GetError().IsEmpty() );
            loaded.push_back( sheet );
        }

        schematic->SetTopLevelSheets( loaded );
        schematic->RefreshHierarchy();
        BOOST_CHECK_EQUAL( schematic->Hierarchy().size(), sheetCount );

        for( const SCH_SHEET_PATH& path : schematic->Hierarchy() )
        {
            BOOST_REQUIRE( path.LastScreen() );
            BOOST_CHECK( !path.LastScreen()->HasLoadError() );
            BOOST_CHECK( IsKemiExt( DOC::SCHEMATIC, wxFileName( path.LastScreen()->GetFileName() ).GetExt() ) );
        }

        BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == before );
        checkNoOfficialFiles( m_outputDirectory );
    }

    KI_TEST::SCOPED_TEMP_DIR m_directory;
    wxString                 m_sourceDirectory;
    wxString                 m_outputDirectory;
    SETTINGS_MANAGER&        m_settings;
    PROJECT*                 m_project = nullptr;
};
} // namespace


BOOST_FIXTURE_TEST_SUITE( KemiSchImport, KEMI_SCH_IMPORT_FIXTURE )

BOOST_AUTO_TEST_CASE( EagleMigratesLegacyLibraryOnlyAfterSuccessfulImport )
{
    const wxString nickname = m_project->GetProjectName() + wxS( "-eagle-import" );
    const wxString legacy = wxFileName( m_sourceDirectory, wxS( "old.kicad_sym" ) ).GetFullPath();
    {
        std::ofstream file( ToFsPath( legacy ), std::ios::binary );
        file << "(kicad_symbol_lib (version 20260830) (generator eeschema)"
                "(symbol \"OldSheetOnly\" (property \"Reference\" \"U\" (at 0 0 0)"
                "(effects (font (size 1 1))))"
                "(property \"Value\" \"OldSheetOnly\" (at 0 0 0) (effects (font (size 1 1))))))";
        BOOST_REQUIRE( file.good() );
    }
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( m_project );
    BOOST_REQUIRE( adapter );
    LIBRARY_TABLE* table = adapter->ProjectTable().value_or( nullptr );
    BOOST_REQUIRE( table );
    LIBRARY_TABLE_ROW& row = table->InsertRow();
    row.SetNickname( nickname );
    row.SetType( wxS( "KiCad" ) );
    row.SetURI( legacy );
    row.SetScope( LIBRARY_TABLE_SCOPE::PROJECT );
    BOOST_REQUIRE( table->Save().has_value() );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );
    const DIRECTORY_SNAPSHOT outputBefore = snapshotDirectory( m_outputDirectory );
    KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY policy( m_outputDirectory );

    // Valid XML without the required Eagle version: parsing fails after library preparation.
    const wxString invalid = wxFileName( m_sourceDirectory, wxS( "invalid.sch" ) ).GetFullPath();
    {
        std::ofstream file( ToFsPath( invalid ), std::ios::binary );
        file << "<?xml version=\"1.0\"?>\n<eagle></eagle>\n";
        BOOST_REQUIRE( file.good() );
    }

    {
        SCHEMATIC schematic( m_project );
        SCH_IO_EAGLE importer;
        importer.SetImportOutputPolicy( &policy );
        BOOST_CHECK_THROW( importer.LoadSchematicFile( invalid, &schematic ), IO_ERROR );
    }

    BOOST_REQUIRE( table->Row( nickname ).has_value() );
    BOOST_CHECK_EQUAL( ( *table->Row( nickname ) )->URI(), legacy );
    BOOST_CHECK( snapshotDirectory( m_outputDirectory ) == outputBefore );
    BOOST_REQUIRE( wxRemoveFile( invalid ) );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );

    const wxString source = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() )
                            + wxS( "/io/eagle/eagle-import-testfile.sch" );
    SCHEMATIC schematic( m_project );
    SCH_IO_EAGLE importer;
    importer.SetImportOutputPolicy( &policy );
    BOOST_REQUIRE( importer.LoadSchematicFile( source, &schematic ) );
    const wxString native = wxFileName( m_outputDirectory, nickname + wxS( ".kemi_sym" ) ).GetFullPath();
    checkKemiDocument( native, KEMI_FORMAT::DOC::SYMBOL_LIB );
    SCH_IO_KICAD_SEXPR reader;
    wxArrayString names;
    reader.EnumerateSymbolLib( names, native );
    BOOST_CHECK( names.Index( wxS( "OldSheetOnly" ) ) != wxNOT_FOUND );
    BOOST_CHECK_GT( names.size(), 1u );
    BOOST_REQUIRE( table->Row( nickname ).has_value() );
    BOOST_CHECK_EQUAL( ( *table->Row( nickname ) )->URI(),
                       wxS( "${KIPRJMOD}/" ) + wxFileName( native ).GetFullName() );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
    checkNoOfficialFiles( m_outputDirectory );
}

BOOST_AUTO_TEST_CASE( PadsAsciiHierarchyDirectKemi )
{
    importAndRoundTrip( "txt", false );
}


BOOST_AUTO_TEST_CASE( PadsBinaryHierarchyDirectKemi )
{
    importAndRoundTrip( "sch", true );
}


BOOST_AUTO_TEST_CASE( PadsBinaryTopLevelDirectKemi )
{
    importAndRoundTrip( "sch", false );
}


BOOST_AUTO_TEST_CASE( CliPadsAsciiDefaultOutputIsKemi )
{
    SCOPED_DRAWING_SHEET drawingSheet;
    m_project->GetProjectFile().m_IP2581Bom.schRevision = wxS( "stale" );
    // With no -o the CLI writes beside its input. Stage that input in the output directory.
    const wxString source = stagePads( "txt", m_outputDirectory );
    const std::string before = readBytes( ToFsPath( source ) );
    JOB_SCH_IMPORT job;
    job.m_inputFile = source;
    job.m_format = JOB_SCH_IMPORT::FORMAT::PADS;
    WX_STRING_REPORTER reporter;
    EESCHEMA_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );

    const wxString root = wxFileName( m_outputDirectory, "multisheet.kemi_sch" ).GetFullPath();
    BOOST_REQUIRE( wxFileExists( root ) );
    size_t schematicCount = 0;
    size_t cacheCount = 0;

    for( const auto& entry : std::filesystem::recursive_directory_iterator( ToFsPath( m_outputDirectory ) ) )
    {
        const wxString path = FromFsPath( entry.path() );

        if( entry.path().extension() == ".kemi_sch" )
        {
            checkKemiDocument( path, KEMI_FORMAT::DOC::SCHEMATIC );
            ++schematicCount;
        }
        else if( entry.path().extension() == ".kemi_sym" )
        {
            checkKemiDocument( path, KEMI_FORMAT::DOC::SYMBOL_LIB );
            ++cacheCount;
        }
    }

    BOOST_CHECK_EQUAL( schematicCount, 3u );
    BOOST_CHECK_EQUAL( cacheCount, 1u );
    BOOST_CHECK( m_project->GetProjectFile().m_IP2581Bom.schRevision.IsEmpty() );
    checkNoOfficialFiles( m_outputDirectory );
    BOOST_CHECK_EQUAL( readBytes( ToFsPath( source ) ), before );

    SCHEMATIC reloaded( m_project );
    SCH_IO_KICAD_SEXPR reader;
    SCH_SHEET* loaded = reader.LoadSchematicFile( root, &reloaded );
    BOOST_REQUIRE( loaded );
    reloaded.SetTopLevelSheets( { loaded } );
    reloaded.RefreshHierarchy();
    BOOST_CHECK( reader.GetError().IsEmpty() );
    BOOST_CHECK_EQUAL( reloaded.Hierarchy().size(), 3u );
}


BOOST_AUTO_TEST_CASE( CliPadsAsciiExplicitOutputPreservesSource )
{
    SCOPED_DRAWING_SHEET drawingSheet;
    const wxString staged = stagePads( "txt", m_sourceDirectory );
    const wxString source = wxFileName( m_sourceDirectory,
                                       "source-V7-20241210-1210.txt" ).GetFullPath();
    BOOST_REQUIRE( wxRenameFile( staged, source ) );
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( m_sourceDirectory );
    const wxString outputDirectory = m_directory.CreateChildDirStr( "kemi" );
    const wxString root = wxFileName( outputDirectory, "converted.kemi_sch" ).GetFullPath();
    JOB_SCH_IMPORT job;
    job.m_inputFile = source;
    job.m_format = JOB_SCH_IMPORT::FORMAT::PADS;
    job.SetConfiguredOutputPath( root );
    WX_STRING_REPORTER reporter;
    EESCHEMA_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );

    size_t schematicCount = 0;
    size_t cacheCount = 0;

    for( const auto& entry : std::filesystem::recursive_directory_iterator( ToFsPath( outputDirectory ) ) )
    {
        if( entry.path().extension() == ".kemi_sch" )
        {
            checkKemiDocument( FromFsPath( entry.path() ), KEMI_FORMAT::DOC::SCHEMATIC );
            BOOST_CHECK( entry.path().parent_path() == ToFsPath( outputDirectory ) );
            ++schematicCount;
        }
        else if( entry.path().extension() == ".kemi_sym" )
        {
            checkKemiDocument( FromFsPath( entry.path() ), KEMI_FORMAT::DOC::SYMBOL_LIB );
            BOOST_CHECK( entry.path().filename() == "converted.kemi_sym" );
            ++cacheCount;
        }
    }

    BOOST_CHECK_EQUAL( schematicCount, 3u );
    BOOST_CHECK_EQUAL( cacheCount, 1u );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == before );
    BOOST_CHECK( !wxDirExists( wxFileName( outputDirectory, "kemi" ).GetFullPath() ) );

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( m_project );
    BOOST_REQUIRE( adapter );
    LIBRARY_TABLE* table = adapter->ProjectTable().value_or( nullptr );
    BOOST_REQUIRE( table );
    LIBRARY_TABLE_ROW* row = table->Row( "converted" ).value_or( nullptr );
    BOOST_REQUIRE( row );
    wxString relativeCachePath;
    BOOST_REQUIRE( row->URI().StartsWith( "${KIPRJMOD}/", &relativeCachePath ) );
    wxFileName cachePath( relativeCachePath );
    BOOST_REQUIRE( cachePath.IsRelative() );
    BOOST_REQUIRE( cachePath.MakeAbsolute( m_project->GetProjectPath() ) );
    BOOST_CHECK( cachePath.SameAs( wxFileName( outputDirectory, "converted.kemi_sym" ) ) );

    checkNoOfficialFiles( outputDirectory );
    checkNoOfficialFiles( m_outputDirectory );

    SCHEMATIC reloaded( m_project );
    SCH_IO_KICAD_SEXPR reader;
    SCH_SHEET* loaded = reader.LoadSchematicFile( root, &reloaded );
    BOOST_REQUIRE( loaded );
    reloaded.SetTopLevelSheets( { loaded } );
    reloaded.RefreshHierarchy();
    BOOST_CHECK( reader.GetError().IsEmpty() );
    BOOST_CHECK_EQUAL( reloaded.Hierarchy().size(), 3u );
}


// GI-META02: CLI import must retain the same root revision as the editor's project save.
BOOST_AUTO_TEST_CASE( CliImportPropagatesRootRevisionWithoutSavingProject )
{
    SCOPED_DRAWING_SHEET drawingSheet;
    const wxString source = stagePads( "txt", m_sourceDirectory );
    std::string bytes = readBytes( ToFsPath( source ) );
    const std::string field = "\"Revision\" ";
    const size_t revision = bytes.find( field );
    BOOST_REQUIRE( revision != std::string::npos );
    bytes.insert( revision + field.size(), "R7" );

    {
        std::ofstream file( ToFsPath( source ), std::ios::binary | std::ios::trunc );
        file << bytes;
        BOOST_REQUIRE( file.good() );
    }

    const wxString projectPath = m_project->GetProjectFullName();
    BOOST_REQUIRE( !wxFileExists( projectPath ) );
    m_project->GetProjectFile().m_IP2581Bom.schRevision = wxS( "stale" );
    m_project->GetProjectFile().m_IP2581Bom.bomRev = wxS( "manual-bom" );
    JOB_SCH_IMPORT job;
    job.m_inputFile = source;
    job.m_format = JOB_SCH_IMPORT::FORMAT::PADS;
    job.SetConfiguredOutputPath( wxFileName( m_outputDirectory, wxS( "revision.kemi_sch" ) ).GetFullPath() );
    WX_STRING_REPORTER reporter;
    EESCHEMA_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );

    BOOST_CHECK_EQUAL( m_project->GetProjectFile().m_IP2581Bom.schRevision, wxS( "R7" ) );
    BOOST_CHECK_EQUAL( m_project->GetProjectFile().m_IP2581Bom.bomRev, wxS( "manual-bom" ) );
    BOOST_CHECK( !wxFileExists( projectPath ) );
    BOOST_CHECK_EQUAL( readBytes( ToFsPath( source ) ), bytes );

    BOOST_REQUIRE( m_settings.SaveProject( projectPath, m_project ) );
    PROJECT_FILE saved( projectPath );
    BOOST_REQUIRE( saved.LoadFromFile() );
    BOOST_CHECK_EQUAL( saved.m_IP2581Bom.schRevision, wxS( "R7" ) );
    BOOST_CHECK_EQUAL( saved.m_IP2581Bom.bomRev, wxS( "manual-bom" ) );
    checkNoOfficialFiles( m_outputDirectory );
}


BOOST_AUTO_TEST_CASE( StandaloneImportDoesNotWriteTransientProject )
{
    SCOPED_DRAWING_SHEET drawingSheet;
    BOOST_REQUIRE( m_settings.UnloadProject( m_project, false ) );
    m_project = nullptr;
    const wxString source = stagePads( "txt", m_sourceDirectory );
    const wxString outputDirectory = m_directory.CreateChildDirStr( "standalone" );
    JOB_SCH_IMPORT job;
    job.m_inputFile = source;
    job.m_format = JOB_SCH_IMPORT::FORMAT::PADS;
    job.SetConfiguredOutputPath( wxFileName( outputDirectory, wxS( "standalone.kemi_sch" ) ).GetFullPath() );
    WX_STRING_REPORTER reporter;
    EESCHEMA_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );
    BOOST_CHECK( !m_settings.IsProjectOpenNotDummy() );
    BOOST_CHECK( wxFileExists( wxFileName( outputDirectory, wxS( "standalone.kemi_sch" ) ).GetFullPath() ) );

    for( const auto& entry : std::filesystem::recursive_directory_iterator( ToFsPath( outputDirectory ) ) )
    {
        BOOST_CHECK( entry.path().extension() != ".kemi_pro" );
        BOOST_CHECK( entry.path().extension() != ".kemi_prl" );
    }

    checkNoOfficialFiles( outputDirectory );
}


BOOST_AUTO_TEST_CASE( CliRejectsOfficialOutputBeforeImport )
{
    const wxString source = stagePads( "txt", m_sourceDirectory );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );
    const DIRECTORY_SNAPSHOT outputBefore = snapshotDirectory( m_outputDirectory );

    for( const wxString& name : { wxString( "forbidden.kicad_sch" ),
                                 wxString( "forbidden.kicad_sym.tmp" ) } )
    {
        BOOST_TEST_CONTEXT( name )
        {
            JOB_SCH_IMPORT job;
            job.m_inputFile = source;
            job.m_format = JOB_SCH_IMPORT::FORMAT::PADS;
            job.SetConfiguredOutputPath( wxFileName( m_outputDirectory, name ).GetFullPath() );
            WX_STRING_REPORTER reporter;
            EESCHEMA_JOBS_HANDLER handler( nullptr );
            handler.SetReporter( &reporter );
            BOOST_CHECK_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::ERR_ARGS );
            BOOST_CHECK( snapshotDirectory( m_outputDirectory ) == outputBefore );
            BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( KemiSymbolLibraryImport )

BOOST_AUTO_TEST_CASE( LegacyLibraryConversionWritesKemiAndPreservesSource )
{
    KI_TEST::SCOPED_TEMP_DIR directory( "kemi-symbol-library-import" );
    const wxString sourceDirectory = directory.CreateChildDirStr( "source" );
    const wxString outputDirectory = directory.CreateChildDirStr( "output" );
    const wxString source = wxFileName( sourceDirectory, "4xxx.lib" ).GetFullPath();
    const wxString sample = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() )
                            + "/libs/4xxx.lib";
    BOOST_REQUIRE( wxCopyFile( sample, source ) );
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( sourceDirectory );
    KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY policy( outputDirectory );
    const wxString output = policy.Target( KEMI_FORMAT::DOC::SYMBOL_LIB,
                                           "converted" ).FinalPath();
    WX_STRING_REPORTER reporter;

    BOOST_REQUIRE( SCH_IO_MGR::ConvertLibrary( nullptr, source, output, &reporter, &policy ) );
    checkKemiDocument( output, KEMI_FORMAT::DOC::SYMBOL_LIB );
    checkNoOfficialFiles( outputDirectory );
    BOOST_CHECK( snapshotDirectory( sourceDirectory ) == before );

    IO_RELEASER<SCH_IO> legacyReader( SCH_IO_MGR::FindPlugin( SCH_IO_MGR::SCH_LEGACY ) );
    wxArrayString expected;
    legacyReader->EnumerateSymbolLib( expected, source );
    SCH_IO_KICAD_SEXPR reader;
    wxArrayString actual;
    reader.EnumerateSymbolLib( actual, output );
    BOOST_REQUIRE_GT( expected.size(), 0u );
    expected.Sort();
    actual.Sort();
    BOOST_CHECK_EQUAL_COLLECTIONS( actual.begin(), actual.end(), expected.begin(), expected.end() );
}


BOOST_AUTO_TEST_CASE( OfficialLibraryTargetsAreRejectedBeforeReadingSource )
{
    KI_TEST::SCOPED_TEMP_DIR directory( "kemi-symbol-library-reject" );
    const wxString outputDirectory = directory.CreateChildDirStr( "output" );
    const wxString source = wxFileName( outputDirectory, "missing.lib" ).GetFullPath();
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( outputDirectory );
    KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY policy( outputDirectory );

    for( const wxString& name : { wxString( "forbidden.kicad_sym" ),
                                 wxString( "forbidden.kicad_sym.tmp" ) } )
    {
        WX_STRING_REPORTER reporter;
        const wxString output = wxFileName( outputDirectory, name ).GetFullPath();
        BOOST_CHECK( !SCH_IO_MGR::ConvertLibrary( nullptr, source, output, &reporter, &policy ) );
        BOOST_CHECK( reporter.GetMessages().Contains( "KemiCAD-only output" ) );
        BOOST_CHECK( snapshotDirectory( outputDirectory ) == before );
    }
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( KemiSchFormatVersion )


BOOST_AUTO_TEST_CASE( KemiSchFormatVersionRejectsFutureAndMissing )
{
    using namespace KEMI_FORMAT;
    KI_TEST::SCOPED_TEMP_DIR directory( "kemi-sch-format-version" );
    SETTINGS_MANAGER manager;
    manager.LoadProject( "" );

    auto writeSch = [&]( const wxString& name, const std::string& body )
    {
        const wxString path = directory.ChildPathStr( name );
        std::ofstream out( ToFsPath( path ), std::ios::binary );
        BOOST_REQUIRE( out.good() );
        out << body;
        out.close();
        return path;
    };

    // Reject paths only need a header: parseHeader throws before the body is walked.
    const std::string hdrTail = "(version 20230121))\n";

    {
        SCHEMATIC schematic( &manager.Prj() );
        SCH_IO_KICAD_SEXPR reader;
        const wxString missing = writeSch( "missing.kemi_sch", "(kemi_sch " + hdrTail );
        BOOST_CHECK_THROW( reader.LoadSchematicFile( missing, &schematic ), IO_ERROR );
    }

    {
        SCHEMATIC schematic( &manager.Prj() );
        SCH_IO_KICAD_SEXPR reader;
        const wxString below = writeSch(
                "below.kemi_sch",
                "(kemi_sch (format_version " + std::to_string( MIN_FORMAT_VERSION - 1 )
                        + ") " + hdrTail );
        BOOST_CHECK_THROW( reader.LoadSchematicFile( below, &schematic ), IO_ERROR );
    }

    {
        SCHEMATIC schematic( &manager.Prj() );
        SCH_IO_KICAD_SEXPR reader;
        const wxString future = writeSch(
                "future.kemi_sch",
                "(kemi_sch (format_version " + std::to_string( FORMAT_VERSION + 1 )
                        + ") " + hdrTail );
        try
        {
            reader.LoadSchematicFile( future, &schematic );
            BOOST_FAIL( "future format_version should throw" );
        }
        catch( const IO_ERROR& err )
        {
            BOOST_CHECK( err.What().Lower().Contains( wxS( "upgrade" ) )
                         || err.What().Lower().Contains( wxS( "format_version" ) ) );
        }
    }

    {
        SCHEMATIC schematic( &manager.Prj() );
        SCH_IO_KICAD_SEXPR reader;
        const wxString badVer = writeSch(
                "badver.kemi_sch",
                "(kemi_sch (format_version \"x\") " + hdrTail );
        BOOST_CHECK_THROW( reader.LoadSchematicFile( badVer, &schematic ), IO_ERROR );
    }

    // Supported range: rewrite a known-good official schematic into .kemi_sch with
    // format_version so the full reader path is exercised without a synthetic body.
    const wxString official = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() )
                              + wxS( "/NoConnectOnPin.kicad_sch" );
    BOOST_REQUIRE_MESSAGE( wxFileExists( official ), official );
    std::string officialBytes = readBytes( ToFsPath( official ) );
    BOOST_REQUIRE( officialBytes.rfind( "(kicad_sch", 0 ) == 0
                   || officialBytes.find( "(kicad_sch" ) != std::string::npos );

    auto loadKemiVersion = [&]( int version, const char* name )
    {
        std::string body = officialBytes;
        const std::string needle = "(kicad_sch";
        const auto at = body.find( needle );
        BOOST_REQUIRE( at != std::string::npos );
        body.replace( at, needle.size(),
                      "(kemi_sch (format_version " + std::to_string( version ) + ")" );
        const wxString path = writeSch( name, body );
        SCHEMATIC schematic( &manager.Prj() );
        SCH_IO_KICAD_SEXPR reader;
        BOOST_CHECK_NO_THROW( reader.LoadSchematicFile( path, &schematic ) );
    };

    loadKemiVersion( FORMAT_VERSION, "ok.kemi_sch" );
    loadKemiVersion( MIN_FORMAT_VERSION, "old.kemi_sch" );

    // Official KiCad schematic without kemi marker remains loadable (compat).
    {
        SCHEMATIC schematic( &manager.Prj() );
        SCH_IO_KICAD_SEXPR reader;
        try
        {
            reader.LoadSchematicFile( official, &schematic );
        }
        catch( const IO_ERROR& err )
        {
            BOOST_FAIL( wxString::Format( wxS( "official .kicad_sch must remain loadable: %s" ),
                                          err.What() ) );
        }
    }
}



BOOST_AUTO_TEST_CASE( CliRejectsSiblingProjectMissingMarkerOrFutureVersion )
{
    using namespace KEMI_FORMAT;
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "kemi_cli_project_gate" ) );

    const wxString official = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() )
                              + wxS( "/NoConnectOnPin.kicad_sch" );
    BOOST_REQUIRE_MESSAGE( wxFileExists( official ), official );
    std::string body = readBytes( ToFsPath( official ) );
    const std::string needle = "(kicad_sch";
    const auto at = body.find( needle );
    BOOST_REQUIRE( at != std::string::npos );
    body.replace( at, needle.size(),
                  "(kemi_sch (format_version " + std::to_string( FORMAT_VERSION ) + ")" );

    const wxString schPath = directory.ChildPathStr( wxS( "gate.kemi_sch" ) );
    {
        std::ofstream out( ToFsPath( schPath ), std::ios::binary );
        BOOST_REQUIRE( out.good() );
        out << body;
    }

    auto writePro = [&]( const std::string& json )
    {
        const wxString proPath = directory.ChildPathStr( wxS( "gate.kemi_pro" ) );
        std::ofstream out( ToFsPath( proPath ), std::ios::binary );
        BOOST_REQUIRE( out.good() );
        out << json;
        return proPath;
    };

    auto expectReject = [&]( const char* label, const std::string& json, const wxString& needleMsg )
    {
        const wxString proPath = writePro( json );
        WX_STRING_REPORTER reporter;
        std::unique_ptr<SCHEMATIC> sch( EESCHEMA_HELPERS::LoadSchematic(
                schPath, /*aSetActive=*/false, /*aForceDefaultProject=*/false, nullptr,
                /*aCalculateConnectivity=*/false, &reporter ) );
        BOOST_CHECK_MESSAGE( !sch, label );
        const wxString msgs = reporter.GetMessages().Lower();
        BOOST_CHECK_MESSAGE( msgs.Contains( needleMsg.Lower() )
                             || msgs.Contains( wxS( "kemicad" ) ),
                             wxString::Format( wxS( "%s: reporter=%s" ), label, reporter.GetMessages() ) );

        // Rejected project must not remain registered for later CLI work.
        BOOST_CHECK( Pgm().GetSettingsManager().GetProject( proPath ) == nullptr );
    };

    expectReject( "missing marker", "{\"meta\":{\"version\":3}}\n", wxS( "marker" ) );
    expectReject( "future version",
                  std::string( "{\"meta\":{\"version\":3},\"kemicad\":{\"document_type\":\"project\","
                               "\"format_version\":" )
                          + std::to_string( FORMAT_VERSION + 1 ) + "}}\n",
                  wxS( "upgrade" ) );

    // No sidecar project: CLI may continue with the default project.
    wxRemoveFile( directory.ChildPathStr( wxS( "gate.kemi_pro" ) ) );
    WX_STRING_REPORTER okReporter;
    std::unique_ptr<SCHEMATIC> ok( EESCHEMA_HELPERS::LoadSchematic(
            schPath, false, false, nullptr, false, &okReporter ) );
    BOOST_REQUIRE_MESSAGE( ok, okReporter.GetMessages() );
}

BOOST_AUTO_TEST_SUITE_END()
