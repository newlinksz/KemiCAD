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
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <board.h>
#include <footprint.h>
#include <git/kigit_pcb_merge.h>
#include <local_history.h>
#include <kemi_format_profile.h>
#include <cli/exit_codes.h>
#include <jobs/job_export_pcb_gencad.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <pcbnew_jobs_handler.h>
#include <reporter.h>
#include <pgm_base.h>
#include <project.h>
#include <scoped_set_reset.h>
#include <settings/common_settings.h>
#include <settings/settings_manager.h>

#include <git2.h>
#include <wx/ffile.h>

namespace
{
void writeBytes( const wxString& aPath, const std::string& aBytes )
{
    wxFFile file( aPath, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE_EQUAL( file.Write( aBytes.data(), aBytes.size() ), aBytes.size() );
    BOOST_REQUIRE( file.Close() );
}


std::string readBytes( const wxString& aPath )
{
    wxFFile file( aPath, wxS( "rb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    std::string bytes( static_cast<size_t>( file.Length() ), '\0' );
    BOOST_REQUIRE_EQUAL( file.Read( bytes.data(), bytes.size() ), bytes.size() );
    return bytes;
}


std::string footprintText( bool aKemi, const std::string& aVersion = "(format_version 1)" )
{
    return std::string( aKemi ? "(kemi_footprint \"A\" " : "(footprint \"A\" " )
           + ( aKemi ? aVersion : "" ) + " (version 20261018) (layer \"F.Cu\"))\n";
}


struct LIBGIT2_SCOPE
{
    LIBGIT2_SCOPE() { git_libgit2_init(); }
    ~LIBGIT2_SCOPE() { git_libgit2_shutdown(); }
};
}


BOOST_AUTO_TEST_SUITE( KemiPcbFormat )


BOOST_AUTO_TEST_CASE( PcbFileRoundTrip )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_pcb_roundtrip" ) );
    PCB_IO_KICAD_SEXPR io;

    for( bool kemi : { false, true } )
    {
        wxString path = dir.ChildPathStr( kemi ? wxS( "board.kemi_pcb" ) : wxS( "board.kicad_pcb" ) );
        BOARD board;
        FOOTPRINT* footprint = new FOOTPRINT( &board );
        footprint->SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
        board.Add( footprint );

        if( kemi )
        {
            io.SaveBoard( path, board );
        }
        else
        {
            STRING_FORMATTER fixture;
            io.FormatBoardToFormatter( &fixture, &board, nullptr, KEMI_FORMAT::FLAVOR::LEGACY );
            writeBytes( path, fixture.GetString() );
        }
        std::string bytes = readBytes( path );
        BOOST_CHECK( bytes.starts_with( kemi ? "(kemi_pcb" : "(kicad_pcb" ) );
        BOOST_CHECK( bytes.find( "(footprint" ) != std::string::npos );
        BOOST_CHECK( bytes.find( "kemi_footprint" ) == std::string::npos );

        std::unique_ptr<BOARD> loaded = io.LoadBoard( path );
        BOOST_REQUIRE( loaded );
        BOOST_REQUIRE_EQUAL( loaded->Footprints().size(), 1U );
        BOOST_CHECK_EQUAL( loaded->Footprints().front()->GetFPID().GetUniStringLibItemName(), wxS( "A" ) );
    }
}


BOOST_AUTO_TEST_CASE( FootprintFileRoundTrip )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_mod_roundtrip" ) );

    for( bool kemi : { false, true } )
    {
        const wxString library = dir.CreateChildDirStr( kemi ? wxS( "new.pretty" ) : wxS( "old.pretty" ) );
        const wxString path = wxFileName( library, kemi ? wxS( "A.kemi_mod" ) : wxS( "A.kicad_mod" ) )
                                      .GetFullPath();
        PCB_IO_KICAD_SEXPR io;
        FOOTPRINT footprint( nullptr );
        footprint.SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
        footprint.SetLibDescription( wxS( "roundtrip" ) );
        if( kemi )
        {
            io.FootprintSave( path, &footprint );
        }
        else
        {
            std::string fixture = footprintText( false );
            fixture.insert( fixture.rfind( ')' ), " (descr \"roundtrip\")" );
            writeBytes( path, fixture );
        }

        BOOST_CHECK( readBytes( path ).starts_with( kemi ? "(kemi_footprint" : "(footprint" ) );
        wxString name;
        std::unique_ptr<FOOTPRINT> imported = io.ImportFootprint( path, name );
        BOOST_REQUIRE( imported );
        BOOST_CHECK_EQUAL( name, wxS( "A" ) );
        BOOST_CHECK_EQUAL( imported->GetLibDescription(), wxS( "roundtrip" ) );

        std::unique_ptr<FOOTPRINT> loaded = io.FootprintLoad( library, wxS( "A" ) );
        BOOST_REQUIRE( loaded );
        BOOST_CHECK_EQUAL( loaded->GetLibDescription(), wxS( "roundtrip" ) );
    }
}


BOOST_AUTO_TEST_CASE( RejectedFootprintCannotBeOverwrittenOrDeleted )
{
    const std::vector<std::string> rejected = {
        footprintText( false ),
        footprintText( true, "" ),
        footprintText( true, "(format_version 2)" ),
        footprintText( true, "(format_version 1.5)" ),
        footprintText( true, "(format_version 4294967297)" ),
        footprintText( true, "(format_version 1) (format_version 1)" )
    };

    for( const std::string& bytes : rejected )
    {
        KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_mod_rejected" ) );
        wxString path = dir.ChildPathStr( wxS( "A.kemi_mod" ) );
        writeBytes( path, bytes );
        PCB_IO_KICAD_SEXPR io;
        FOOTPRINT replacement( nullptr );
        replacement.SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
        const std::map<std::string, UTF8> skipValidation = { { "skip_cache_validation", "" } };

        for( int retry = 0; retry < 2; ++retry )
        {
            BOOST_CHECK_THROW( io.FootprintLoad( dir.PathStr(), wxS( "A" ) ), IO_ERROR );
            BOOST_CHECK_THROW( io.GetEnumeratedFootprint( dir.PathStr(), wxS( "A" ) ), IO_ERROR );
            BOOST_CHECK_THROW( io.FootprintSave( path, &replacement ), IO_ERROR );
            BOOST_CHECK_THROW( io.FootprintSave( path, &replacement, &skipValidation ), IO_ERROR );
            BOOST_CHECK_THROW( io.FootprintDelete( dir.PathStr(), wxS( "A" ) ), IO_ERROR );
            BOOST_CHECK_THROW( io.DeleteLibrary( dir.PathStr() ), IO_ERROR );
            BOOST_CHECK_EQUAL( readBytes( path ), bytes );
        }
    }
}


BOOST_AUTO_TEST_CASE( BoardContainerVersionMustBeExactInteger )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_pcb_invalid_version" ) );
    wxString path = dir.ChildPathStr( wxS( "board.kemi_pcb" ) );
    PCB_IO_KICAD_SEXPR io;

    for( const std::string& version : { "1.5", "4294967297", "2" } )
    {
        std::string bytes = "(kemi_pcb (format_version " + version + ") (version 20261018))";
        writeBytes( path, bytes );
        BOOST_CHECK_THROW( io.LoadBoard( path ), IO_ERROR );
        BOOST_CHECK_EQUAL( readBytes( path ), bytes );
    }
}




BOOST_AUTO_TEST_CASE( BoardContainerVersionPolicyRejectsAndAccepts )
{
    using namespace KEMI_FORMAT;
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_pcb_version_policy" ) );
    PCB_IO_KICAD_SEXPR io;

    auto writeBoard = [&]( const wxString& name, const std::string& bytes )
    {
        const wxString path = dir.ChildPathStr( name );
        writeBytes( path, bytes );
        return path;
    };

    // Supported current version loads.
    const wxString ok = writeBoard(
            wxS( "ok.kemi_pcb" ),
            "(kemi_pcb (format_version " + std::to_string( FORMAT_VERSION )
                    + ") (version 20261018))\n" );
    BOOST_CHECK_NO_THROW( io.LoadBoard( ok ) );

    // Supported MIN loads.
    const wxString oldOk = writeBoard(
            wxS( "old.kemi_pcb" ),
            "(kemi_pcb (format_version " + std::to_string( MIN_FORMAT_VERSION )
                    + ") (version 20261018))\n" );
    BOOST_CHECK_NO_THROW( io.LoadBoard( oldOk ) );

    // Missing format_version: reject.
    const wxString missing = writeBoard( wxS( "missing.kemi_pcb" ),
                                         "(kemi_pcb (version 20261018))\n" );
    BOOST_CHECK_THROW( io.LoadBoard( missing ), IO_ERROR );

    // Below MIN: reject.
    const wxString below = writeBoard(
            wxS( "below.kemi_pcb" ),
            "(kemi_pcb (format_version " + std::to_string( MIN_FORMAT_VERSION - 1 )
                    + ") (version 20261018))\n" );
    BOOST_CHECK_THROW( io.LoadBoard( below ), IO_ERROR );

    // Future: reject + upgrade wording.
    const wxString future = writeBoard(
            wxS( "future.kemi_pcb" ),
            "(kemi_pcb (format_version " + std::to_string( FORMAT_VERSION + 1 )
                    + ") (version 20261018))\n" );
    try
    {
        io.LoadBoard( future );
        BOOST_FAIL( "future format_version should throw" );
    }
    catch( const IO_ERROR& err )
    {
        BOOST_CHECK( err.What().Lower().Contains( wxS( "upgrade" ) )
                     || err.What().Lower().Contains( wxS( "format_version" ) ) );
    }

    // Unrecognizable format_version (non-integer): reject.
    const wxString badVer = writeBoard( wxS( "badver.kemi_pcb" ),
                                        "(kemi_pcb (format_version \"x\") (version 20261018))\n" );
    BOOST_CHECK_THROW( io.LoadBoard( badVer ), IO_ERROR );

    // Official .kicad_pcb without kemi marker remains loadable.
    const wxString legacy = writeBoard( wxS( "legacy.kicad_pcb" ),
                                        "(kicad_pcb (version 20261018))\n" );
    BOOST_CHECK_NO_THROW( io.LoadBoard( legacy ) );
}

BOOST_AUTO_TEST_CASE( OfficialFootprintFirstSaveCanBeUpdatedAndReopened )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_mod_first_save" ) );
    const wxString source = dir.ChildPathStr( wxS( "A.kicad_mod" ) );
    const wxString target = dir.ChildPathStr( wxS( "A.kemi_mod" ) );
    writeBytes( source, footprintText( false ) );
    PCB_IO_KICAD_SEXPR io;
    std::unique_ptr<FOOTPRINT> edited( io.FootprintLoad( dir.PathStr(), wxS( "A" ) ) );
    BOOST_REQUIRE( edited );
    edited->SetLibDescription( wxS( "first save" ) );
    io.FootprintSave( dir.PathStr(), edited.get() );
    BOOST_CHECK_EQUAL( readBytes( source ), footprintText( false ) );
    BOOST_CHECK( wxFileExists( target ) );
    edited->SetLibDescription( wxS( "second save" ) );
    io.FootprintSave( dir.PathStr(), edited.get() );

    PCB_IO_KICAD_SEXPR reopened;
    wxArrayString names;
    reopened.FootprintEnumerate( names, dir.PathStr(), false );
    BOOST_REQUIRE_EQUAL( names.size(), 1U );
    std::unique_ptr<FOOTPRINT> loaded( reopened.FootprintLoad( dir.PathStr(), wxS( "A" ) ) );
    BOOST_REQUIRE( loaded );
    BOOST_CHECK( loaded->IsKemiOnly() );
    BOOST_CHECK_EQUAL( loaded->GetLibDescription(), wxS( "second save" ) );
    BOOST_CHECK_EQUAL( readBytes( source ), footprintText( false ) );
}


BOOST_AUTO_TEST_CASE( OfficialFootprintCannotOverwriteExistingNativeMember )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_mod_existing_target" ) );
    const wxString source = dir.ChildPathStr( wxS( "A.kicad_mod" ) );
    const wxString target = dir.ChildPathStr( wxS( "A.kemi_mod" ) );
    writeBytes( source, footprintText( false ) );
    writeBytes( target, footprintText( true ) );
    PCB_IO_KICAD_SEXPR io;
    wxString name;
    std::unique_ptr<FOOTPRINT> imported( io.ImportFootprint( source, name ) );
    BOOST_REQUIRE( imported );
    BOOST_CHECK_THROW( io.FootprintSave( dir.PathStr(), imported.get() ), IO_ERROR );
    BOOST_CHECK_THROW( io.FootprintSave( source, imported.get() ), IO_ERROR );
    BOOST_CHECK_EQUAL( readBytes( source ), footprintText( false ) );
    BOOST_CHECK_EQUAL( readBytes( target ), footprintText( true ) );

    // Reopening the library selects the native member rather than the import source.
    std::unique_ptr<FOOTPRINT> native( io.FootprintLoad( dir.PathStr(), wxS( "A" ) ) );
    BOOST_REQUIRE( native );
    BOOST_CHECK( native->IsKemiOnly() );
    BOOST_CHECK_NO_THROW( io.FootprintSave( dir.PathStr(), native.get() ) );
}


BOOST_AUTO_TEST_CASE( ImportUsesRealSourcePath )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_mod_import" ) );
    wxString path = dir.ChildPathStr( wxS( "A.kemi_mod" ) );
    writeBytes( path, footprintText( false ) );
    PCB_IO_KICAD_SEXPR io;
    wxString name;
    BOOST_CHECK_THROW( io.ImportFootprint( path, name ), IO_ERROR );
    BOOST_CHECK_EQUAL( readBytes( path ), footprintText( false ) );
    std::unique_ptr<BOARD_ITEM> clipboard( io.Parse( wxString::FromUTF8( footprintText( false ) ) ) );
    BOOST_REQUIRE( clipboard );
    BOOST_CHECK( dynamic_cast<FOOTPRINT*>( clipboard.get() ) != nullptr );
}


BOOST_AUTO_TEST_CASE( RenameInvalidatesCachedFormat )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_mod_rename" ) );
    wxString oldPath = dir.ChildPathStr( wxS( "A.kicad_mod" ) );
    wxString newPath = dir.ChildPathStr( wxS( "A.kemi_mod" ) );
    writeBytes( oldPath, footprintText( false ) );
    PCB_IO_KICAD_SEXPR io;
    BOOST_REQUIRE( io.FootprintLoad( dir.PathStr(), wxS( "A" ) ) );
    BOOST_REQUIRE( wxRenameFile( oldPath, newPath ) );
    BOOST_CHECK_THROW( io.FootprintLoad( dir.PathStr(), wxS( "A" ) ), IO_ERROR );
    BOOST_CHECK_EQUAL( readBytes( newPath ), footprintText( false ) );
}


#ifdef __WINDOWS__
BOOST_AUTO_TEST_CASE( WindowsNativePreferenceIgnoresCase )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_mod_case" ) );
    writeBytes( dir.ChildPathStr( wxS( "Foo.kicad_mod" ) ), footprintText( false ) );
    writeBytes( dir.ChildPathStr( wxS( "foo.kemi_mod" ) ), footprintText( true ) );
    PCB_IO_KICAD_SEXPR io;
    std::unique_ptr<FOOTPRINT> native( io.FootprintLoad( dir.PathStr(), wxS( "foo" ) ) );
    BOOST_REQUIRE( native );
    BOOST_CHECK( native->IsKemiOnly() );
}

#endif


BOOST_AUTO_TEST_CASE( HistorySnapshotRestoresKemiBoard )
{
    LIBGIT2_SCOPE libgit;
    SCOPED_SET_RESET<COMMON_SETTINGS::AUTO_BACKUP> restoreBackup( Pgm().GetCommonSettings()->m_Backup );
    auto& backup = Pgm().GetCommonSettings()->m_Backup;
    backup.enabled = true;
    backup.format = BACKUP_FORMAT::INCREMENTAL;
    backup.location = BACKUP_LOCATION::PROJECT_DIR;
    backup.limit_total_size = COMMON_SETTINGS::AUTO_BACKUP::UNLIMITED_SIZE;

    SETTINGS_MANAGER manager;
    KI_TEST::SCOPED_TEMP_PROJECT project( manager, wxS( "kemi_pcb_history" ), wxS( "history" ) );
    writeBytes( project.Project().GetProjectFullName(), "{\"meta\":{\"version\":3}}\n" );
    BOARD board;
    board.SetProject( &project.Project() );
    board.SetFileName( wxFileName( project.DirStr(), wxS( "history.kemi_pcb" ) ).GetFullPath() );
    FOOTPRINT* footprint = new FOOTPRINT( &board );
    footprint->SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
    board.Add( footprint );

    LOCAL_HISTORY history;
    history.RegisterSaver( &board,
            [&board]( const wxString& aPath, std::vector<HISTORY_FILE_DATA>& aFiles )
            {
                board.SaveToHistory( aPath, aFiles );
            } );

    const wxString projectPath = project.Project().GetProjectPath();
    BOOST_REQUIRE( history.RunRegisteredSaversAndCommit( projectPath, wxS( "Kemi format QA" ) ) );
    history.WaitForPendingSave();
    wxString head = history.GetHeadHash( projectPath );
    BOOST_REQUIRE( !head.IsEmpty() );

    KI_TEST::SCOPED_TEMP_DIR restored( wxS( "kemi_pcb_restored" ) );
    BOOST_REQUIRE( history.ExtractAllFilesAtCommit( projectPath, head, restored.PathStr() ) );
    wxString path = restored.ChildPathStr( wxS( "history.kemi_pcb" ) );
    BOOST_CHECK( readBytes( path ).starts_with( "(kemi_pcb" ) );
    PCB_IO_KICAD_SEXPR io;
    std::unique_ptr<BOARD> loaded = io.LoadBoard( path );
    BOOST_REQUIRE( loaded );
    BOOST_CHECK_EQUAL( loaded->Footprints().size(), 1U );
    history.UnregisterSaver( &board );
    board.ClearProject();
}


BOOST_AUTO_TEST_CASE( GitMergeWriterUsesTargetFormat )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_pcb_git_format" ) );
    BOARD board;
    PCB_IO_KICAD_SEXPR io;

    for( bool kemi : { false, true } )
    {
        wxString path = dir.ChildPathStr( kemi ? wxS( "board.kemi_pcb" ) : wxS( "board.kicad_pcb" ) );
        std::string bytes = KIGIT_PCB_MERGE::FormatBoard( &board, path );
        BOOST_CHECK( bytes.starts_with( kemi ? "(kemi_pcb" : "(kicad_pcb" ) );
        writeBytes( path, bytes );
        BOOST_REQUIRE( io.LoadBoard( path ) );
    }
}



BOOST_AUTO_TEST_CASE( CliRejectsSiblingProjectMissingMarkerOrFutureVersion )
{
    using namespace KEMI_FORMAT;
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi_cli_pcb_project_gate" ) );

    const wxString boardPath = dir.ChildPathStr( wxS( "gate.kemi_pcb" ) );
    writeBytes( boardPath,
                "(kemi_pcb (format_version " + std::to_string( FORMAT_VERSION )
                        + ") (version 20261018))\n" );

    auto writePro = [&]( const std::string& json )
    {
        writeBytes( dir.ChildPathStr( wxS( "gate.kemi_pro" ) ), json );
    };

    auto expectReject = [&]( const char* label, const std::string& json, const wxString& needleMsg )
    {
        writePro( json );
        WX_STRING_REPORTER reporter;
        PCBNEW_JOBS_HANDLER handler( nullptr );
        handler.SetReporter( &reporter );

        JOB_EXPORT_PCB_GENCAD job;
        job.m_filename = boardPath;
        job.SetConfiguredOutputPath( dir.ChildPathStr( wxS( "out.cad" ) ) );

        const int rc = handler.JobExportGencad( &job );
        BOOST_CHECK_MESSAGE( rc != CLI::EXIT_CODES::OK
                             && rc != CLI::EXIT_CODES::SUCCESS, label );
        const wxString msgs = reporter.GetMessages().Lower();
        BOOST_CHECK_MESSAGE( msgs.Contains( needleMsg.Lower() )
                             || msgs.Contains( wxS( "kemicad" ) )
                             || msgs.Contains( wxS( "failed to load board" ) ),
                             wxString::Format( wxS( "%s: reporter=%s" ), label, reporter.GetMessages() ) );
    };

    expectReject( "missing marker", "{\"meta\":{\"version\":3}}\n", wxS( "marker" ) );
    expectReject( "future version",
                  std::string( "{\"meta\":{\"version\":3},\"kemicad\":{\"document_type\":\"project\","
                               "\"format_version\":" )
                          + std::to_string( FORMAT_VERSION + 1 ) + "}}\n",
                  wxS( "upgrade" ) );

    // No sidecar: board load may proceed with a transient project.
    wxRemoveFile( dir.ChildPathStr( wxS( "gate.kemi_pro" ) ) );
    WX_STRING_REPORTER okReporter;
    PCBNEW_JOBS_HANDLER okHandler( nullptr );
    okHandler.SetReporter( &okReporter );
    JOB_EXPORT_PCB_GENCAD okJob;
    okJob.m_filename = boardPath;
    okJob.SetConfiguredOutputPath( dir.ChildPathStr( wxS( "ok.cad" ) ) );
    const int okRc = okHandler.JobExportGencad( &okJob );
    BOOST_CHECK_MESSAGE( okRc == CLI::EXIT_CODES::OK || okRc == CLI::EXIT_CODES::SUCCESS,
                         okReporter.GetMessages() );
}

BOOST_AUTO_TEST_SUITE_END()
