/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 */

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <board.h>
#include <board_loader.h>
#include <design_block.h>
#include <design_block_io.h>
#include <diff_merge/diff_doc_kind.h>
#include <json_common.h>
#include <pcbnew_utils/board_file_utils.h>
#include <cli/exit_codes.h>
#include <footprint.h>
#include <jobs/job_pcb_downgrade.h>
#include <kemi_format_profile.h>
#include <pcbnew_save_file_as.h>
#include <libraries/library_table.h>
#include <kicad_clipboard.h>
#include <local_history.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <pcb_io/kicad_sexpr/writers/pcb_writer_v9.h>
#include <pcb_io/kicad_sexpr/writers/pcb_writer_v10.h>
#include <pcbnew_jobs_handler.h>
#include <project.h>
#include <reporter.h>
#include <settings/settings_manager.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/log.h>

#include <filesystem>
#include <map>
#include <vector>


namespace
{
std::string readBytes( const wxString& aPath )
{
    wxFFile file( aPath, wxS( "rb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    std::string bytes( static_cast<size_t>( file.Length() ), '\0' );
    BOOST_REQUIRE_EQUAL( file.Read( bytes.data(), bytes.size() ), bytes.size() );
    return bytes;
}
}


BOOST_AUTO_TEST_SUITE( KemiPcbPersistentOutput )

BOOST_AUTO_TEST_CASE( RenamedNativeBoardRejectsOfficialWritesBeforeOpeningOutput )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_persistent_board" ) );
    PCB_IO_KICAD_SEXPR io;
    BOARD source;
    const wxString native = dir.ChildPathStr( wxS( "source.kemi_pcb" ) );
    const wxString official = dir.ChildPathStr( wxS( "rejected.kicad_pcb" ) );
    io.SaveBoard( native, source );
    std::unique_ptr<BOARD> board = io.LoadBoard( native );
    BOOST_REQUIRE( board );
    board->SetFileName( official );
    BOOST_REQUIRE( board->IsKemiOnly() );

    BOOST_CHECK_THROW( io.SaveBoard( official, *board ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( official ) );
    BOOST_CHECK_THROW( PCB_WRITER_V9().SaveBoard( official, board.get() ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( official ) );
    BOOST_CHECK_THROW( PCB_WRITER_V10().SaveBoard( official, board.get() ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( official ) );

    STRING_FORMATTER formatter;
    BOOST_CHECK_THROW( io.FormatBoardToFormatter( &formatter, board.get(), nullptr,
                                                 KEMI_FORMAT::FLAVOR::LEGACY ), IO_ERROR );
    BOOST_CHECK( formatter.GetString().empty() );

    const wxString temporary = dir.ChildPathStr( wxS( "snapshot.tmp" ) );
    io.SaveBoard( temporary, *board );
    BOOST_CHECK( readBytes( temporary ).starts_with( "(kemi_pcb" ) );
    BOOST_CHECK( readBytes( native ).starts_with( "(kemi_pcb" ) );
}


BOOST_AUTO_TEST_CASE( NativeFootprintCloneAndCacheKeepKemiProfile )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_persistent_footprint" ) );
    PCB_IO_KICAD_SEXPR io;
    FOOTPRINT source( nullptr );
    source.SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
    const wxString native = dir.ChildPathStr( wxS( "A.kemi_mod" ) );
    io.FootprintSave( native, &source );
    wxString name;
    std::unique_ptr<FOOTPRINT> loaded = io.ImportFootprint( native, name );
    BOOST_REQUIRE( loaded );
    FOOTPRINT clone( *loaded );
    BOOST_REQUIRE( clone.IsKemiOnly() );
    const wxString official = dir.ChildPathStr( wxS( "rejected.kicad_mod" ) );
    BOOST_CHECK_THROW( io.FootprintSave( official, &clone ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( official ) );
    BOOST_CHECK_THROW( PCB_WRITER_V9().SaveFootprintFile( official, &clone ), IO_ERROR );
    BOOST_CHECK_THROW( PCB_WRITER_V10().SaveFootprintFile( official, &clone ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( official ) );

    const wxString library = dir.CreateChildDirStr( wxS( "cache.pretty" ) );
    io.FootprintSave( library, &clone );
    BOOST_CHECK( wxFileExists( wxFileName( library, wxS( "A.kemi_mod" ) ).GetFullPath() ) );
    BOOST_CHECK( !wxFileExists( wxFileName( library, wxS( "A.kicad_mod" ) ).GetFullPath() ) );
}


BOOST_AUTO_TEST_CASE( DowngradeJobRejectsNativeExtensionAndDisguisedNativeRoot )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_downgrade_job" ) );
    PCB_IO_KICAD_SEXPR io;
    BOARD source;
    const wxString native = dir.ChildPathStr( wxS( "source.kemi_pcb" ) );
    const wxString disguised = dir.ChildPathStr( wxS( "disguised.kicad_pcb" ) );
    const wxString output = dir.ChildPathStr( wxS( "rejected.kicad_pcb" ) );
    io.SaveBoard( native, source );
    BOOST_REQUIRE( wxCopyFile( native, disguised ) );

    BOARD legacyBoard;
    const wxString legacy = dir.ChildPathStr( wxS( "legacy.kicad_pcb" ) );
    {
        wxFFile file( legacy, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(kicad_pcb (version 20260901) (generator pcbnew) (general))" ) ) );
    }
    wxFFile markedProject( dir.ChildPathStr( wxS( "legacy.kicad_pro" ) ), wxS( "wb" ) );
    BOOST_REQUIRE( markedProject.IsOpened() );
    BOOST_REQUIRE( markedProject.Write(
            wxS( "{\"kemicad\":{\"format_version\":1,\"document_type\":\"project\"}}" ), wxConvUTF8 ) );
    BOOST_REQUIRE( markedProject.Close() );

    for( const wxString& input : { native, disguised, legacy } )
    {
        wxArrayString before;
        wxDir::GetAllFiles( dir.PathStr(), &before );
        before.Sort();
        JOB_PCB_DOWNGRADE job;
        job.m_filename = input;
        job.m_outputFile = output;
        WX_STRING_REPORTER reporter;
        PCBNEW_JOBS_HANDLER handler( nullptr );
        handler.SetReporter( &reporter );
        BOOST_CHECK_NE( handler.JobDowngrade( &job ), CLI::EXIT_CODES::SUCCESS );
        BOOST_CHECK( !wxFileExists( output ) );
        BOOST_CHECK( reporter.HasMessage() );

        wxArrayString after;
        wxDir::GetAllFiles( dir.PathStr(), &after );
        after.Sort();
        BOOST_REQUIRE_EQUAL( after.size(), before.size() );

        for( size_t i = 0; i < before.size(); ++i )
            BOOST_CHECK_EQUAL( after[i], before[i] );
    }
}


BOOST_AUTO_TEST_CASE( AutosavePayloadRetainsNativeNameAfterBoardRename )
{
    SETTINGS_MANAGER manager;
    KI_TEST::SCOPED_TEMP_PROJECT project( manager, wxS( "kemi_persistent_autosave" ), wxS( "board" ) );
    BOARD board;
    board.SetProject( &project.Project() );
    board.SetFileName( wxFileName( project.DirStr(), wxS( "board.kemi_pcb" ) ).GetFullPath() );
    board.SetFileName( wxFileName( project.DirStr(), wxS( "board.kicad_pcb" ) ).GetFullPath() );

    std::vector<HISTORY_FILE_DATA> files;
    board.SaveToHistory( project.Project().GetProjectPath(), files );
    BOOST_REQUIRE_EQUAL( files.size(), 1U );
    BOOST_CHECK_EQUAL( files.front().relativePath, wxS( "board.kemi_pcb" ) );
    BOOST_CHECK( files.front().content.starts_with( "(kemi_pcb" ) );
    BOOST_CHECK( !wxFileExists( board.GetFileName() ) );
    board.ClearProject();
}


BOOST_AUTO_TEST_CASE( ConvertedFootprintDeleteAndRenameRefuseToExposeOfficialSource )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-footprint-delete" ) );
    const wxString library = dir.CreateChildDirStr( wxS( "source.pretty" ) );
    const wxString official = wxFileName( library, wxS( "A.kicad_mod" ) ).GetFullPath();
    {
        wxFFile file( official, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(footprint A (version 20260901) (generator pcbnew) (layer F.Cu))" ) ) );
    }

    const std::string sourceBytes = readBytes( official );
    PCB_IO_KICAD_SEXPR io;
    std::unique_ptr<FOOTPRINT> imported = io.FootprintLoad( library, wxS( "A" ) );
    BOOST_REQUIRE( imported );
    FOOTPRINT edited( *imported );
    io.FootprintSave( library, &edited );
    const wxString native = wxFileName( library, wxS( "A.kemi_mod" ) ).GetFullPath();
    const std::string nativeBytes = readBytes( native );
    BOOST_CHECK_THROW( io.FootprintDelete( library, wxS( "A" ) ), IO_ERROR );

    // The rename workflow saves the new name then removes the old member. The removal must
    // fail explicitly instead of reporting success and resurrecting the official member.
    edited.SetFPID( LIB_ID( wxEmptyString, wxS( "Renamed" ) ) );
    io.FootprintSave( library, &edited );
    BOOST_CHECK_THROW( io.FootprintDelete( library, wxS( "A" ) ), IO_ERROR );
    PCB_IO_KICAD_SEXPR reopened;
    wxArrayString names;
    reopened.FootprintEnumerate( names, library, false );
    BOOST_CHECK( names.Index( wxS( "A" ) ) != wxNOT_FOUND );
    BOOST_REQUIRE( reopened.FootprintLoad( library, wxS( "A" ) ) );
    BOOST_CHECK( reopened.FootprintLoad( library, wxS( "A" ) )->IsKemiOnly() );
    BOOST_CHECK_EQUAL( readBytes( native ), nativeBytes );
    BOOST_CHECK_EQUAL( readBytes( official ), sourceBytes );

    // Native-only members retain normal deletion semantics after reopening.
    BOOST_CHECK_NO_THROW( reopened.FootprintDelete( library, wxS( "Renamed" ) ) );
    PCB_IO_KICAD_SEXPR deleted;
    names.clear();
    deleted.FootprintEnumerate( names, library, false );
    BOOST_CHECK( names.Index( wxS( "Renamed" ) ) == wxNOT_FOUND );
}


BOOST_AUTO_TEST_CASE( LibraryDowngradeRejectsBeforeCopyingNativeMembers )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_library_downgrade_job" ) );
    const wxString library = dir.CreateChildDirStr( wxS( "native.pretty" ) );
    PCB_IO_KICAD_SEXPR io;
    FOOTPRINT footprint( nullptr );
    footprint.SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
    footprint.SetKemiOnly();
    io.FootprintSave( library, &footprint );
    const wxString native = wxFileName( library, wxS( "A.kemi_mod" ) ).GetFullPath();
    const std::string before = readBytes( native );
    const wxString outputDir = dir.CreateChildDirStr( wxS( "output" ) );
    const wxString boardFile = dir.ChildPathStr( wxS( "source.kicad_pcb" ) );
    BOARD board;
    {
        wxFFile file( boardFile, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(kicad_pcb (version 20260901) (generator pcbnew) (general))" ) ) );
    }

    JOB_PCB_DOWNGRADE job;
    job.m_filename = boardFile;
    job.m_libraryDir = library;
    job.m_outputFile = wxFileName( outputDir, wxS( "export.kicad_pcb" ) ).GetFullPath();
    WX_STRING_REPORTER reporter;
    PCBNEW_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_CHECK_EQUAL( handler.JobDowngrade( &job ), CLI::EXIT_CODES::ERR_INVALID_INPUT_FILE );
    BOOST_CHECK( reporter.HasMessage() );
    BOOST_CHECK( !wxFileExists( job.m_outputFile ) );
    BOOST_CHECK( !wxDirExists( wxFileName( outputDir, wxS( "native.pretty" ) ).GetFullPath() ) );
    BOOST_CHECK_EQUAL( readBytes( native ), before );

    // Official libraries are also import-only; constructing the fixture must not use a writer.
    const wxString officialLib = dir.CreateChildDirStr( wxS( "official.pretty" ) );
    const wxString official = wxFileName( officialLib, wxS( "A.kicad_mod" ) ).GetFullPath();
    {
        wxFFile file( official, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(footprint A (version 20260901) (generator pcbnew) (layer F.Cu))" ) ) );
    }
    const std::string officialBefore = readBytes( official );
    job.m_libraryDir = officialLib;
    reporter.Clear();
    BOOST_CHECK_EQUAL( handler.JobDowngrade( &job ), CLI::EXIT_CODES::ERR_INVALID_INPUT_FILE );
    BOOST_CHECK( !wxFileExists( job.m_outputFile ) );
    BOOST_CHECK_EQUAL( readBytes( official ), officialBefore );
    BOOST_CHECK_EQUAL( readBytes( native ), before );
}



BOOST_AUTO_TEST_CASE( ManagerSaveAsRenamesManagedNativeFootprintReferencesAndNickname )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_manager_cache_rename_pcb" ) );
    const wxString sourceDir = dir.CreateChildDirStr( wxS( "source" ) );
    const wxString outputDir = dir.CreateChildDirStr( wxS( "output" ) );
    const wxString vendor = wxFileName( sourceDir, wxS( "vendor" ) ).GetFullPath();
    const wxString copiedVendor = wxFileName( outputDir, wxS( "vendor" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( vendor ) );
    BOOST_REQUIRE( wxFileName::Mkdir( copiedVendor ) );
    const wxString nestedTable = wxFileName( vendor, wxS( "fp-lib-table" ) ).GetFullPath();
    auto write = []( const wxString& aPath, const wxString& aContents )
    {
        wxFFile file( aPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aContents, wxConvUTF8 ) );
        BOOST_REQUIRE( file.Close() );
    };
    write( wxFileName( sourceDir, wxS( "source.kemi_pro" ) ).GetFullPath(), wxS( "{}" ) );
    const wxString pretty = wxFileName( sourceDir, wxS( "source.pretty" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( pretty ) );
    const wxString tablePath = wxFileName( sourceDir, wxS( "fp-lib-table" ) ).GetFullPath();
    write( tablePath, wxS( "(fp_lib_table(version 7)"
            "(lib (name source)(type KiCad)(uri ${KIPRJMOD}/source.pretty)"
            "(options kicad_import_cache=1)(descr \"\"))"
            "(lib (name kept_alias)(type KiCad)(uri ${KIPRJMOD}/./source.pretty)"
            "(options \"\")(descr \"\"))"
            "(lib (name external)(type KiCad)(uri ${KIPRJMOD}/vendor/source.pretty)"
            "(options kicad_import_cache=1)(descr \"\")))" ) );
    write( nestedTable, wxS( "(fp_lib_table\r\n  (version 7)\r\n"
            "  (lib (name source)(type KiCad)(uri ${KIPRJMOD}/vendor/unmanaged.pretty)"
            "(options \"\")(descr \"third-party formatting\")))\r\n" ) );
    const std::string nestedBefore = readBytes( nestedTable );
    BOARD board;
    for( const wxString& nickname : { wxString( "source" ), wxString( "external" ) } )
    {
        auto* footprint = new FOOTPRINT( &board );
        footprint->SetFPID( LIB_ID( nickname, wxS( "Pad" ) ) );
        board.Add( footprint );
    }
    PCB_IO_KICAD_SEXPR writer;
    const wxString source = wxFileName( sourceDir, wxS( "source.kemi_pcb" ) ).GetFullPath();
    writer.SaveBoard( source, board );
    FOOTPRINT pad( nullptr );
    pad.SetFPID( LIB_ID( wxS( "source" ), wxS( "Pad" ) ) );
    const wxString module = wxFileName( pretty, wxS( "Pad.kemi_mod" ) ).GetFullPath();
    writer.FootprintSave( module, &pad );
    const std::string before = readBytes( source );
    LIBRARY_TABLE parsed( wxFileName( tablePath ), LIBRARY_TABLE_SCOPE::PROJECT );
    BOOST_REQUIRE_MESSAGE( parsed.IsOk(), parsed.ErrorDescription() );
    const std::string tableBefore = readBytes( tablePath );
    const wxString copiedPretty = wxFileName( outputDir, wxS( "copy.pretty" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( copiedPretty ) );
    wxString errors;

    for( const wxString& file : { source, module, tablePath, nestedTable } )
        PCBNEW_SaveFileAs( sourceDir, wxS( "source" ), outputDir, wxS( "copy" ), file, errors );

    BOOST_CHECK_MESSAGE( errors.empty(), errors );
    PCB_IO_KICAD_SEXPR reader;
    std::unique_ptr<BOARD> copied( reader.LoadBoard(
            wxFileName( outputDir, wxS( "copy.kemi_pcb" ) ).GetFullPath() ) );
    BOOST_REQUIRE( copied );
    bool renamed = false;
    bool external = false;
    for( FOOTPRINT* footprint : copied->Footprints() )
    {
        renamed |= footprint->GetFPID() == LIB_ID( wxS( "copy" ), wxS( "Pad" ) );
        external |= footprint->GetFPID() == LIB_ID( wxS( "external" ), wxS( "Pad" ) );
    }
    BOOST_CHECK( renamed );
    BOOST_CHECK( external );
    BOOST_CHECK_EQUAL( readBytes( wxFileName( copiedPretty, wxS( "Pad.kemi_mod" ) ).GetFullPath() ),
                       readBytes( module ) );
    LIBRARY_TABLE table( wxFileName( outputDir, wxS( "fp-lib-table" ) ),
                         LIBRARY_TABLE_SCOPE::PROJECT );
    BOOST_REQUIRE( table.IsOk() );
    auto* row = table.Row( wxS( "copy" ) ).value_or( nullptr );
    BOOST_REQUIRE( row );
    BOOST_CHECK_EQUAL( row->URI(), wxS( "${KIPRJMOD}/copy.pretty" ) );
    BOOST_CHECK( !table.HasRow( wxS( "source" ) ) );
    auto* externalRow = table.Row( wxS( "external" ) ).value_or( nullptr );
    BOOST_REQUIRE( externalRow );
    BOOST_CHECK_EQUAL( externalRow->URI(), wxS( "${KIPRJMOD}/vendor/source.pretty" ) );
    auto* alias = table.Row( wxS( "kept_alias" ) ).value_or( nullptr );
    BOOST_REQUIRE( alias );
    BOOST_CHECK_EQUAL( alias->URI(), wxS( "${KIPRJMOD}/copy.pretty" ) );
    BOOST_CHECK_EQUAL( readBytes( wxFileName( copiedVendor, wxS( "fp-lib-table" ) ).GetFullPath() ),
                       nestedBefore );
    BOOST_CHECK_EQUAL( readBytes( nestedTable ), nestedBefore );
    BOOST_CHECK_EQUAL( readBytes( source ), before );
    BOOST_CHECK_EQUAL( readBytes( tablePath ), tableBefore );
}


BOOST_AUTO_TEST_CASE( ManagerSaveAsAcceptsUppercaseBoardAndFootprintExtensions )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_manager_uppercase_pcb" ) );
    const wxString sourceDir = dir.CreateChildDirStr( wxS( "source" ) );
    const wxString outputDir = dir.CreateChildDirStr( wxS( "output" ) );
    PCB_IO_KICAD_SEXPR io;
    BOARD board;
    const wxString source = wxFileName( sourceDir, wxS( "source.KEMI_PCB" ) ).GetFullPath();
    io.SaveBoard( source, board );
    FOOTPRINT footprint( nullptr );
    footprint.SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
    const wxString module = wxFileName( sourceDir, wxS( "A.KEMI_MOD" ) ).GetFullPath();
    io.FootprintSave( module, &footprint );
    wxString errors;
    PCBNEW_SaveFileAs( sourceDir, wxS( "source" ), outputDir, wxS( "copy" ), source, errors );
    PCBNEW_SaveFileAs( sourceDir, wxS( "source" ), outputDir, wxS( "copy" ), module, errors );
    BOOST_CHECK( errors.empty() );
    const wxString target = wxFileName( outputDir, wxS( "copy.KEMI_PCB" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileExists( target ) );
    std::unique_ptr<BOARD> restored( io.LoadBoard( target, nullptr ) );
    BOOST_REQUIRE( restored );
    const wxString copiedModule = wxFileName( outputDir, wxS( "A.KEMI_MOD" ) ).GetFullPath();
    BOOST_CHECK_EQUAL( readBytes( copiedModule ), readBytes( module ) );
}


BOOST_AUTO_TEST_CASE( ClipboardPreservesNativeIdentityWithoutDesktopAccess )
{
    BOARD board;
    board.SetKemiOnly();
    FOOTPRINT* footprint = new FOOTPRINT( &board );
    footprint->SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
    board.Add( footprint );

    wxString text;
    CLIPBOARD_IO clipboard;
    clipboard.SetBoard( &board );
    clipboard.SetWriter( [&]( const wxString& aText ) { text = aText; } );
    clipboard.SetReader( [&]() { return text; } );
    PCB_SELECTION selection;
    selection.Add( footprint );
    clipboard.SaveSelection( selection, false );
    BOOST_CHECK( text.StartsWith( wxS( "(kemi_footprint" ) ) );
    std::unique_ptr<BOARD_ITEM> parsed( clipboard.Parse() );
    BOOST_REQUIRE( parsed );
    BOOST_REQUIRE_EQUAL( parsed->Type(), PCB_FOOTPRINT_T );
    BOOST_CHECK( static_cast<FOOTPRINT*>( parsed.get() )->IsKemiOnly() );

    CLIPBOARD_IO boardClipboard;
    boardClipboard.SetWriter( [&]( const wxString& aText ) { text = aText; } );
    boardClipboard.SetReader( [&]() { return text; } );
    boardClipboard.SaveBoard( wxEmptyString, board );
    BOOST_CHECK( text.StartsWith( wxS( "(kemi_pcb" ) ) );
    parsed.reset( boardClipboard.Parse() );
    BOOST_REQUIRE( parsed );
    BOOST_REQUIRE_EQUAL( parsed->Type(), PCB_T );
    BOOST_CHECK( static_cast<BOARD*>( parsed.get() )->IsKemiOnly() );
}

BOOST_AUTO_TEST_CASE( OfficialBoardSaveAndBlockConversionPreserveReadOnlySource )
{
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "kemi-first-board-save" ) );
    const wxString fixture = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() )
                             + wxS( "api_kitchen_sink.kicad_pcb" );
    const wxString source = directory.ChildPathStr( wxS( "source.kicad_pcb" ) );
    const wxString native = directory.ChildPathStr( wxS( "source.kemi_pcb" ) );
    BOOST_REQUIRE( wxCopyFile( fixture, source ) );
    const std::string before = readBytes( source );
    PCB_IO_KICAD_SEXPR io;
    std::unique_ptr<BOARD> board = io.LoadBoard( source );
    BOOST_REQUIRE( board );
    BOOST_CHECK_THROW( io.SaveBoard( source, *board ), IO_ERROR );
    wxString target = source;
    BOOST_REQUIRE( BOARD_LOADER::SaveBoard( target, *board ) );
    BOOST_CHECK_EQUAL( target, native );
    BOOST_CHECK( readBytes( native ).starts_with( "(kemi_pcb" ) );
    BOOST_CHECK_EQUAL( readBytes( source ), before );
    const std::string nativeBefore = readBytes( native );
    target = source;
    {
        wxLogNull silence;
        BOOST_CHECK( !BOARD_LOADER::SaveBoard( target, *board ) );
    }
    BOOST_CHECK_EQUAL( readBytes( native ), nativeBefore );
    BOOST_CHECK_EQUAL( readBytes( source ), before );

    const wxString impossible = wxFileName( source, wxS( "blocked.kemi_pcb" ) ).GetFullPath();
    BOOST_CHECK_THROW( io.SaveBoard( impossible, *board ), IO_ERROR );
    BOOST_CHECK_EQUAL( readBytes( source ), before );

    DESIGN_BLOCK_IO blocks( [&]( KEMI_FORMAT::DOC aDoc, const wxString& aSource, const wxString& aTarget )
    {
        BOOST_CHECK( aDoc == KEMI_FORMAT::DOC::PCB );
        std::unique_ptr<BOARD> member = io.LoadBoard( aSource );
        BOOST_REQUIRE( member );
        io.SaveBoard( aTarget, *member );
        return true;
    } );
    const wxString library = directory.CreateChildDirStr( wxS( "blocks.kicad_blocks" ) );
    DESIGN_BLOCK block;
    block.SetLibId( LIB_ID( wxS( "Blocks" ), wxS( "Imported" ) ) );
    block.SetBoardFile( source );
    blocks.DesignBlockSave( library, &block );
    std::unique_ptr<DESIGN_BLOCK> reopened( blocks.DesignBlockLoad( library, wxS( "Imported" ) ) );
    BOOST_REQUIRE( reopened );
    BOOST_CHECK_EQUAL( wxFileName( reopened->GetBoardFile() ).GetExt(), wxS( "kemi_pcb" ) );
    BOOST_CHECK( readBytes( reopened->GetBoardFile() ).starts_with( "(kemi_pcb" ) );
    BOOST_CHECK_EQUAL( readBytes( source ), before );
}


BOOST_AUTO_TEST_CASE( MergeProjectPropertiesAndRulesWriteNativeSiblingsOnly )
{
    for( bool rulesOnly : { false, true } )
    {
        KI_TEST::SCOPED_TEMP_DIR directory( wxS( "kemi-merge-companions" ) );
        const wxString fixture = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() )
                                 + wxS( "api_kitchen_sink.kicad_pcb" );
        std::map<wxString, std::string> sources;
        std::vector<wxString> boards;

        for( const wxString& name : { wxS( "ancestor" ), wxS( "ours" ), wxS( "theirs" ) } )
        {
            const wxString board = directory.ChildPathStr( name + wxS( ".kicad_pcb" ) );
            BOOST_REQUIRE( wxCopyFile( fixture, board ) );
            boards.push_back( board );
            sources.emplace( board, readBytes( board ) );
            const wxString project = directory.ChildPathStr( name + wxS( ".kicad_pro" ) );
            nlohmann::json data = { { "meta", { { "version", 3 } } },
                    { "pcbnew", { { "page_layout_descr_file",
                            !rulesOnly && name == wxS( "ours" ) ? "changed.kemi_wks" : "" } } } };
            {
                wxFFile file( project, wxS( "wb" ) );
                BOOST_REQUIRE( file.IsOpened() );
                BOOST_REQUIRE( file.Write( wxString::FromUTF8( data.dump() ) ) );
            }
            sources.emplace( project, readBytes( project ) );

            if( rulesOnly )
            {
                const wxString rules = directory.ChildPathStr( name + wxS( ".kicad_dru" ) );
                wxFFile file( rules, wxS( "wb" ) );
                BOOST_REQUIRE( file.IsOpened() );
                BOOST_REQUIRE( file.Write( name == wxS( "ours" )
                        ? wxS( "(version 2)\n(rule \"merged\" (constraint clearance (min 0.25mm)))\n" )
                        : wxS( "(version 2)\n" ) ) );
                file.Close();
                sources.emplace( rules, readBytes( rules ) );
            }
        }

        const wxString output = directory.ChildPathStr( "ancestor.kemi_pcb" );
        WX_STRING_REPORTER reporter;
        PCBNEW_JOBS_HANDLER handler( nullptr );
        // A forbidden companion link must reject the entire output set before the board is written.
        const wxString blockedBoard = directory.ChildPathStr( "blocked.kemi_pcb" );
        const wxString linkedCompanion = directory.ChildPathStr( rulesOnly ? "blocked.kemi_dru" : "blocked.kemi_pro" );
        const wxString officialCompanion = directory.ChildPathStr( rulesOnly ? "ancestor.kicad_dru" : "ancestor.kicad_pro" );
        std::error_code linkError;
        std::filesystem::create_symlink( std::filesystem::path( officialCompanion.wc_str() ),
                                        std::filesystem::path( linkedCompanion.wc_str() ), linkError );

        if( !linkError )
        {
            BOOST_CHECK_EQUAL( handler.RunMerge( KICAD_DIFF::DOC_KIND::PCB, boards[0], boards[1], boards[2],
                                                 blockedBoard, false, false, &reporter ),
                               CLI::EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT );
            BOOST_CHECK( !wxFileExists( blockedBoard ) );
        }
        else
        {
            BOOST_TEST_MESSAGE( "Companion symlink creation unavailable: " << linkError.message() );
        }

        BOOST_REQUIRE_EQUAL( handler.RunMerge( KICAD_DIFF::DOC_KIND::PCB, boards[0], boards[1], boards[2],
                                               output, false, false, &reporter ), CLI::EXIT_CODES::SUCCESS );
        BOOST_CHECK( readBytes( output ).starts_with( "(kemi_pcb" ) );

        if( rulesOnly )
        {
            const std::string rules = readBytes( directory.ChildPathStr( "ancestor.kemi_dru" ) );
            BOOST_CHECK( rules.starts_with( "(kemi_dru" ) );
            BOOST_CHECK( rules.find( "merged" ) != std::string::npos );
        }
        else
        {
            const wxString project = directory.ChildPathStr( "ancestor.kemi_pro" );
            const nlohmann::json data = nlohmann::json::parse( readBytes( project ) );
            BOOST_CHECK( KEMI_FORMAT::ValidateJsonMarker( data, KEMI_FORMAT::DOC::PROJECT, project ).IsEmpty() );
            BOOST_CHECK_EQUAL( data["pcbnew"]["page_layout_descr_file"], "changed.kemi_wks" );
        }

        for( const auto& [path, bytes] : sources )
            BOOST_CHECK_EQUAL( readBytes( path ), bytes );
    }
}


BOOST_AUTO_TEST_SUITE_END()
