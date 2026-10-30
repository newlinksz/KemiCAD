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
 */

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <gestfich.h>
#include <kemi_format_profile.h>
#include <local_history.h>
#include <pgm_base.h>
#include <project/project_archiver.h>
#include <reporter.h>
#include <settings/common_settings.h>
#include <wx_filename.h>

#include <git2.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace
{

void writeDocument( const wxString& aPath, const std::string& aContent )
{
    BOOST_REQUIRE( wxFileName::Mkdir( wxFileName( aPath ).GetPath(), wxS_DIR_DEFAULT,
                                    wxPATH_MKDIR_FULL ) );
    wxFFile file( aPath, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE_EQUAL( file.Write( aContent.data(), aContent.size() ), aContent.size() );
    BOOST_REQUIRE( file.Close() );
}


wxString childPath( const wxString& aDirectory, const wxString& aRelativePath )
{
    return wxFileName( aDirectory + wxS( "/" ) + aRelativePath ).GetFullPath();
}


std::set<wxString> zipEntryNames( const wxString& aPath )
{
    wxFFileInputStream stream( aPath );
    BOOST_REQUIRE( stream.IsOk() );
    wxZipInputStream zip( stream );
    std::set<wxString> names;

    while( std::unique_ptr<wxZipEntry> entry{ zip.GetNextEntry() } )
    {
        wxString name = entry->GetName();
        name.Replace( wxS( "\\" ), wxS( "/" ) );
        names.insert( name );
    }

    return names;
}


void writeArchive( const wxString& aPath,
                   const std::vector<std::pair<wxString, std::string>>& aMembers )
{
    wxFFileOutputStream stream( aPath );
    BOOST_REQUIRE( stream.IsOk() );
    wxZipOutputStream zip( stream );

    for( const auto& [name, content] : aMembers )
    {
        BOOST_REQUIRE( zip.PutNextEntry( name ) );
        zip.Write( content.data(), content.size() );
        BOOST_REQUIRE( zip.IsOk() );
        BOOST_REQUIRE( zip.CloseEntry() );
    }

    BOOST_REQUIRE( zip.Close() );
}


struct SCOPED_HISTORY_SETTINGS
{
    SCOPED_HISTORY_SETTINGS() : saved( Pgm().GetCommonSettings()->m_Backup )
    {
        BOOST_REQUIRE( git_libgit2_init() > 0 );
        auto& backup = Pgm().GetCommonSettings()->m_Backup;
        backup.enabled = true;
        backup.format = BACKUP_FORMAT::INCREMENTAL;
        backup.location = BACKUP_LOCATION::PROJECT_DIR;
    }

    ~SCOPED_HISTORY_SETTINGS()
    {
        Pgm().GetCommonSettings()->m_Backup = saved;
        git_libgit2_shutdown();
    }

    COMMON_SETTINGS::AUTO_BACKUP saved;
};


struct KEMI_ARCHIVE_HISTORY_FIXTURE
{
    KEMI_ARCHIVE_HISTORY_FIXTURE() : directory( wxS( "kemi-archive-history" ) )
    {
        projectDirectory = directory.CreateChildDirStr( wxS( "project" ) );
    }

    std::map<wxString, std::string> createNativeProject()
    {
        std::map<wxString, std::string> documents;

        for( const KEMI_FORMAT::DOC_PROFILE& profile : KEMI_FORMAT::AllProfiles() )
        {
            wxString name = wxS( "design." ) + wxString::FromUTF8( profile.kemiExt );

            if( profile.doc == KEMI_FORMAT::DOC::FOOTPRINT )
                name = wxS( "cache.pretty/" ) + name;

            documents.emplace( name, "native document " + std::string( profile.kemiExt ) + '\n' );
        }

        documents.emplace( wxS( "sheets/child.kemi_sch" ), "native child sheet\n" );
        documents.emplace( wxS( "sym-lib-table" ), "(sym_lib_table)\n" );
        documents.emplace( wxS( "fp-lib-table" ), "(fp_lib_table)\n" );
        documents.emplace( wxS( "design-block-lib-table" ), "(design_block_lib_table)\n" );

        for( const auto& [name, content] : documents )
            writeDocument( childPath( projectDirectory, name ), content );

        return documents;
    }

    KI_TEST::SCOPED_TEMP_DIR directory;
    wxString                projectDirectory;
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( KemiArchiveHistory, KEMI_ARCHIVE_HISTORY_FIXTURE )


// AH01 / K03,K04: Native backups preserve all profiles and .pretty members without
// copying retained official source files into the archive or its restored project.
BOOST_AUTO_TEST_CASE( ArchiveRoundTripsNativeArtifacts )
{
    const auto documents = createNativeProject();
    const wxString oldBoard = childPath( projectDirectory, wxS( "retained.kicad_pcb" ) );
    writeDocument( oldBoard, "retained official source\n" );
    const wxString archive = directory.ChildPathStr( wxS( "backup.zip" ) );
    WX_STRING_REPORTER reporter;

    BOOST_REQUIRE( PROJECT_ARCHIVER::Archive( projectDirectory, archive, reporter, false ) );
    const auto entries = zipEntryNames( archive );
    BOOST_CHECK_EQUAL( entries.size(), documents.size() );

    for( const auto& [name, content] : documents )
        BOOST_CHECK_EQUAL( entries.count( name ), 1u );

    BOOST_CHECK_EQUAL( entries.count( wxS( "retained.kicad_pcb" ) ), 0u );
    const wxString restored = directory.CreateChildDirStr( wxS( "restored" ) );
    BOOST_REQUIRE( PROJECT_ARCHIVER::Unarchive( archive, restored, reporter ) );

    for( const auto& [name, content] : documents )
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( restored, name ) ), content );

    BOOST_CHECK( !wxFileExists( childPath( restored, wxS( "retained.kicad_pcb" ) ) ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( oldBoard ), "retained official source\n" );
}


// AH02 / K03: The production scan, full git snapshot and compare extraction all
// retain native artifacts while excluding nested projects and backup metadata.
BOOST_AUTO_TEST_CASE( HistorySnapshotsNativeArtifacts )
{
    SCOPED_HISTORY_SETTINGS settings;
    const auto documents = createNativeProject();
    writeDocument( childPath( projectDirectory, wxS( "retained.kicad_pcb" ) ), "keep source\n" );
    writeDocument( childPath( projectDirectory, wxS( "nested/other.kemi_pro" ) ), "nested\n" );
    writeDocument( childPath( projectDirectory, wxS( "nested/other.kemi_sch" ) ), "nested\n" );
    writeDocument( childPath( projectDirectory, wxS( "design-backups/old.kemi_pcb" ) ), "backup\n" );
    writeDocument( childPath( projectDirectory, wxS( "notes.txt" ) ), "unrelated\n" );
    const auto files = LOCAL_HISTORY::CollectProjectFiles( projectDirectory );
    BOOST_CHECK_EQUAL( files.size(), documents.size() );

    for( const auto& [name, content] : documents )
    {
        const wxString fullPath = childPath( projectDirectory, name );
        BOOST_CHECK( std::find( files.begin(), files.end(), fullPath ) != files.end() );
    }

    LOCAL_HISTORY history;
    BOOST_REQUIRE( history.CommitFullProjectSnapshot( projectDirectory, wxS( "Native snapshot" ) ) );
    const wxString hash = history.GetHeadHash( projectDirectory );
    BOOST_REQUIRE( !hash.IsEmpty() );
    const wxString extracted = directory.ChildPathStr( wxS( "history-extract" ) );
    BOOST_REQUIRE( history.ExtractAllFilesAtCommit( projectDirectory, hash, extracted ) );

    for( const auto& [name, content] : documents )
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( extracted, name ) ), content );

    BOOST_CHECK( !wxFileExists( childPath( extracted, wxS( "retained.kicad_pcb" ) ) ) );
    BOOST_CHECK( !wxDirExists( childPath( extracted, wxS( "nested" ) ) ) );
}


// AH03 / K03,K04: Exercise the real atomic autosave writer and recovery scanner
// in both storage layouts; no extension is inferred from a temporary filename.
BOOST_AUTO_TEST_CASE( AutosaveAndRecoveryKeepNativeNames )
{
    createNativeProject();
    HISTORY_FILE_DATA sheet;
    sheet.relativePath = wxS( "sheets/child.kemi_sch" );
    sheet.content = "(kemi_sch (format_version 1))\n";

    for( BACKUP_LOCATION location : { BACKUP_LOCATION::PROJECT_DIR, BACKUP_LOCATION::USER_DIR } )
    {
        const wxString root = directory.ChildPathStr(
                location == BACKUP_LOCATION::PROJECT_DIR ? wxS( "autosave-project" )
                                                         : wxS( "autosave-user" ) );
        BOOST_REQUIRE( LOCAL_HISTORY::WriteAutosaveFiles( root, projectDirectory, location, { sheet } ) );
        const wxString relative = location == BACKUP_LOCATION::PROJECT_DIR
                ? wxString( wxS( "sheets/_autosave-child.kemi_sch" ) ) : sheet.relativePath;
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( root, relative ) ), sheet.content );
        const auto pairs = LOCAL_HISTORY::CollectAutosaveFilePairs( root, projectDirectory, location );
        BOOST_REQUIRE_EQUAL( pairs.size(), 1u );
        BOOST_CHECK_EQUAL( pairs.front().first, childPath( root, relative ) );
        BOOST_CHECK_EQUAL( wxFileName( pairs.front().second ).GetExt(), wxS( "kemi_sch" ) );

        for( const auto& entry : std::filesystem::recursive_directory_iterator( ToFsPath( root ) ) )
            BOOST_CHECK( !FromFsPath( entry.path() ).Lower().Contains( wxS( ".kicad_" ) ) );
    }
}


// AH04 / K04,K06: Validate the whole batch before the first atomic write. Also
// protect standalone native documents when the project itself is not native.
BOOST_AUTO_TEST_CASE( OfficialAutosaveTargetsFailBeforeAnyWrite )
{
    createNativeProject();
    HISTORY_FILE_DATA valid;
    valid.relativePath = wxS( "first.kemi_sch" );
    valid.content = "native sheet\n";

    for( const char* suffix : { "kicad_sch", "KICAD_PCB", "kicad_sym.tmp", "kicad_mod" } )
    {
        HISTORY_FILE_DATA invalid;
        invalid.relativePath = wxS( "blocked." ) + wxString::FromUTF8( suffix );
        invalid.content = "must never be written\n";
        const wxString root = directory.ChildPathStr( wxString::FromUTF8( suffix ) );
        BOOST_CHECK( !LOCAL_HISTORY::WriteAutosaveFiles( root, projectDirectory,
                                                        BACKUP_LOCATION::PROJECT_DIR, { valid, invalid } ) );
        BOOST_CHECK( !wxDirExists( root ) );
    }

    const wxString standalone = directory.CreateChildDirStr( wxS( "standalone" ) );
    HISTORY_FILE_DATA copied;
    copied.relativePath = wxS( "copy.kicad_sch" );
    copied.sourcePath = childPath( projectDirectory, wxS( "design.kemi_sch" ) );
    const wxString root = directory.ChildPathStr( wxS( "standalone-autosave" ) );
    BOOST_CHECK( !LOCAL_HISTORY::WriteAutosaveFiles( root, standalone, BACKUP_LOCATION::USER_DIR,
                                                    { copied } ) );
    BOOST_CHECK( !wxDirExists( root ) );
}


// AH05 / K06: A pre-conversion history cannot bypass the native output policy
// through restore or compare extraction, even when its official source files remain.
BOOST_AUTO_TEST_CASE( NativeProjectRejectsOfficialHistoryRestore )
{
    SCOPED_HISTORY_SETTINGS settings;
    const wxString oldProject = childPath( projectDirectory, wxS( "old.kicad_pro" ) );
    writeDocument( oldProject, "old project\n" );
    writeDocument( childPath( projectDirectory, wxS( "old.kicad_pcb" ) ), "old board\n" );
    LOCAL_HISTORY history;
    BOOST_REQUIRE( history.CommitFullProjectSnapshot( projectDirectory, wxS( "Before conversion" ) ) );
    const wxString hash = history.GetHeadHash( projectDirectory );
    BOOST_REQUIRE( !hash.IsEmpty() );
    createNativeProject();
    writeDocument( oldProject, "retained source is unchanged by failed restore\n" );

    BOOST_CHECK( !history.RestoreCommit( projectDirectory, hash, nullptr, false ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( oldProject ),
                       "retained source is unchanged by failed restore\n" );
    BOOST_CHECK( !wxDirExists( childPath( projectDirectory, wxS( "_restore_temp" ) ) ) );
    const wxString extracted = directory.ChildPathStr( wxS( "blocked-extract" ) );
    BOOST_CHECK( !history.ExtractAllFilesAtCommit( projectDirectory, hash, extracted ) );
    BOOST_CHECK( !wxDirExists( extracted ) );

    // The next native snapshot must also discard old official index entries, without
    // deleting the retained source files or rewriting the old commit.
    BOOST_REQUIRE( history.CommitFullProjectSnapshot( projectDirectory, wxS( "After conversion" ) ) );
    const wxString nativeHash = history.GetHeadHash( projectDirectory );
    BOOST_REQUIRE( nativeHash != hash );
    BOOST_REQUIRE( history.ExtractAllFilesAtCommit( projectDirectory, nativeHash, extracted ) );
    BOOST_CHECK( wxFileExists( childPath( extracted, wxS( "design.kemi_pro" ) ) ) );
    BOOST_CHECK( !wxFileExists( childPath( extracted, wxS( "old.kicad_pro" ) ) ) );
    BOOST_CHECK( !wxFileExists( childPath( extracted, wxS( "old.kicad_pcb" ) ) ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( oldProject ),
                       "retained source is unchanged by failed restore\n" );
}


// AH06 / K06: A ZIP from before conversion is rejected before even its first
// harmless member can overwrite the current native project.
BOOST_AUTO_TEST_CASE( NativeProjectRejectsOfficialArchiveRestore )
{
    createNativeProject();
    const wxString original = childPath( projectDirectory, wxS( "sym-lib-table" ) );
    const std::string before = KI_TEST::LoadStringData( original );
    const wxString oldDirectory = directory.CreateChildDirStr( wxS( "old-project" ) );
    writeDocument( childPath( oldDirectory, wxS( "sym-lib-table" ) ), "must not overwrite\n" );
    writeDocument( childPath( oldDirectory, wxS( "old.kicad_pro" ) ), "old project\n" );
    writeDocument( childPath( oldDirectory, wxS( "old.kicad_pcb" ) ), "old board\n" );
    const wxString archive = directory.ChildPathStr( wxS( "old-backup.zip" ) );
    WX_STRING_REPORTER reporter;
    BOOST_REQUIRE( PROJECT_ARCHIVER::Archive( oldDirectory, archive, reporter, false ) );
    BOOST_CHECK( !PROJECT_ARCHIVER::Unarchive( archive, projectDirectory, reporter ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( original ), before );
    BOOST_CHECK( !wxFileExists( childPath( projectDirectory, wxS( "old.kicad_pro" ) ) ) );
    BOOST_CHECK( !wxFileExists( childPath( projectDirectory, wxS( "old.kicad_pcb" ) ) ) );
}


// AH07 / K03,K04: A standalone native document also restricts its backup outputs.
// Official sources are always excluded; independent nested projects remain out of scope.
BOOST_AUTO_TEST_CASE( StandaloneNativeDocumentRestrictsBackupDirectory )
{
    const wxString board = childPath( projectDirectory, wxS( "standalone.kemi_pcb" ) );
    const wxString retained = childPath( projectDirectory, wxS( "retained.kicad_pcb" ) );
    writeDocument( board, "native board\n" );
    writeDocument( retained, "keep original board\n" );
    WX_STRING_REPORTER reporter;
    const wxString archive = directory.ChildPathStr( wxS( "standalone.zip" ) );
    BOOST_REQUIRE( PROJECT_ARCHIVER::Archive( projectDirectory, archive, reporter, false ) );
    const auto entries = zipEntryNames( archive );
    BOOST_REQUIRE_EQUAL( entries.size(), 1u );
    BOOST_CHECK_EQUAL( entries.count( wxS( "standalone.kemi_pcb" ) ), 1u );
    const auto files = LOCAL_HISTORY::CollectProjectFiles( projectDirectory );
    BOOST_REQUIRE_EQUAL( files.size(), 1u );
    BOOST_CHECK_EQUAL( files.front(), board );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( retained ), "keep original board\n" );

    const wxString official = directory.CreateChildDirStr( wxS( "official-project" ) );
    const wxString oldProject = childPath( official, wxS( "main.kicad_pro" ) );
    const wxString oldBoard = childPath( official, wxS( "main.kicad_pcb" ) );
    writeDocument( oldProject, "official project\n" );
    writeDocument( oldBoard, "official board\n" );
    writeDocument( childPath( official, wxS( "nested/other.kemi_pro" ) ), "other project\n" );
    writeDocument( childPath( official, wxS( "nested/other.kemi_pcb" ) ), "other board\n" );
    const auto officialFiles = LOCAL_HISTORY::CollectProjectFiles( official );
    BOOST_CHECK( officialFiles.empty() );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( oldProject ), "official project\n" );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( oldBoard ), "official board\n" );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( official, wxS( "nested/other.kemi_pro" ) ) ),
                       "other project\n" );
}


// AH08 / P6-P1-1: A library does not convert the official project beside it.
// ZIP import archives retain their existing contract; native history never re-emits official sources.
BOOST_AUTO_TEST_CASE( HistoryExcludesOfficialSourcesBesideNativeLibrary )
{
    SCOPED_HISTORY_SETTINGS settings;
    const std::map<wxString, std::string> documents = {
        { wxS( "main.kicad_pro" ), "official project\n" },
        { wxS( "main.kicad_sch" ), "official sheet\n" },
        { wxS( "main.kicad_pcb" ), "official board\n" },
        { wxS( "scratch.kemi_sym" ), "unrelated native library\n" }
    };

    for( const auto& [name, content] : documents )
        writeDocument( childPath( projectDirectory, name ), content );

    WX_STRING_REPORTER reporter;
    const wxString archive = directory.ChildPathStr( wxS( "official.zip" ) );
    BOOST_REQUIRE( PROJECT_ARCHIVER::Archive( projectDirectory, archive, reporter, false ) );
    BOOST_CHECK_EQUAL( zipEntryNames( archive ).size(), documents.size() );
    const wxString restored = directory.ChildPathStr( wxS( "official-restored" ) );
    BOOST_REQUIRE( PROJECT_ARCHIVER::Unarchive( archive, restored, reporter ) );

    const auto historyFiles = LOCAL_HISTORY::CollectProjectFiles( projectDirectory );
    BOOST_REQUIRE_EQUAL( historyFiles.size(), 1u );
    BOOST_CHECK_EQUAL( historyFiles.front(), childPath( projectDirectory, wxS( "scratch.kemi_sym" ) ) );
    LOCAL_HISTORY history;
    BOOST_REQUIRE( history.CommitFullProjectSnapshot( projectDirectory, wxS( "Official project" ) ) );
    const wxString extracted = directory.ChildPathStr( wxS( "official-history" ) );
    BOOST_REQUIRE( history.ExtractAllFilesAtCommit( projectDirectory,
                                                   history.GetHeadHash( projectDirectory ), extracted ) );

    for( const auto& [name, content] : documents )
    {
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( restored, name ) ), content );
        if( name == wxS( "scratch.kemi_sym" ) )
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( extracted, name ) ), content );
        else
            BOOST_CHECK( !wxFileExists( childPath( extracted, name ) ) );

        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( projectDirectory, name ) ), content );
    }

    // A standalone official board is also excluded without changing its source bytes.
    const wxString standalone = directory.CreateChildDirStr( wxS( "official-standalone" ) );
    writeDocument( childPath( standalone, wxS( "main.kicad_pcb" ) ), "official board\n" );
    writeDocument( childPath( standalone, wxS( "scratch.kemi_sym" ) ), "native library\n" );
    const wxString standaloneZip = directory.ChildPathStr( wxS( "official-standalone.zip" ) );
    BOOST_REQUIRE( PROJECT_ARCHIVER::Archive( standalone, standaloneZip, reporter, false ) );
    BOOST_CHECK_EQUAL( zipEntryNames( standaloneZip ).count( wxS( "main.kicad_pcb" ) ), 1u );
    const auto standaloneFiles = LOCAL_HISTORY::CollectProjectFiles( standalone );
    BOOST_REQUIRE_EQUAL( standaloneFiles.size(), 1u );
    BOOST_CHECK_EQUAL( standaloneFiles.front(), childPath( standalone, wxS( "scratch.kemi_sym" ) ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( standalone, wxS( "main.kicad_pcb" ) ) ),
                       "official board\n" );
}


// AH09 / P6-P1-2,K04: Prefixes and member order must not hide native project
// ownership. Reject the entire archive before even a harmless member is written.
BOOST_AUTO_TEST_CASE( PrefixedMixedArchiveFailsBeforeAnyWrite )
{
    const wxString archive = directory.ChildPathStr( wxS( "mixed.zip" ) );
    writeArchive( archive, { { wxS( "demo/sym-lib-table" ), "must not overwrite\n" },
                            { wxS( "demo/sub/old.kicad_sch" ), "official sheet\n" },
                            { wxS( "demo/main.kemi_pro" ), "native project\n" } } );

    for( bool strip : { false, true } )
    {
        const wxString restored = directory.CreateChildDirStr(
                strip ? wxString( wxS( "stripped" ) ) : wxString( wxS( "prefixed" ) ) );
        const wxString sentinel = childPath( restored, strip ? wxString( wxS( "sym-lib-table" ) )
                                                             : wxString( wxS( "demo/sym-lib-table" ) ) );
        writeDocument( sentinel, "unchanged\n" );
        WX_STRING_REPORTER reporter;
        BOOST_CHECK( !PROJECT_ARCHIVER::Unarchive( archive, restored, reporter,
                                                   strip ? wxString( wxS( "demo" ) ) : wxString() ) );
        BOOST_CHECK( reporter.HasMessageOfSeverity( RPT_SEVERITY_ERROR ) );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( sentinel ), "unchanged\n" );
        const wxString root = strip ? restored : childPath( restored, wxS( "demo" ) );
        BOOST_CHECK( !wxFileExists( childPath( root, wxS( "main.kemi_pro" ) ) ) );
        BOOST_CHECK( !wxDirExists( childPath( root, wxS( "sub" ) ) ) );
    }

    // Destination ownership also applies below an archive prefix.
    writeArchive( archive, { { wxS( "demo/old.kicad_sch" ), "official sheet\n" } } );
    writeDocument( childPath( projectDirectory, wxS( "demo/main.kemi_pro" ) ), "native project\n" );
    WX_STRING_REPORTER reporter;
    BOOST_CHECK( !PROJECT_ARCHIVER::Unarchive( archive, projectDirectory, reporter ) );
    BOOST_CHECK( !wxFileExists( childPath( projectDirectory, wxS( "demo/old.kicad_sch" ) ) ) );
}


// AH10 / P6-P1-1,P6-P1-2: Independent nested projects retain their own format
// in both directions; a native neighbour does not restrict the official subtree.
BOOST_AUTO_TEST_CASE( ArchivePreservesIndependentProjectScopes )
{
    writeDocument( childPath( projectDirectory, wxS( "main.kemi_pro" ) ), "native project\n" );
    writeDocument( childPath( projectDirectory, wxS( "main.kemi_sch" ) ), "native sheet\n" );
    writeDocument( childPath( projectDirectory, wxS( "retained.kicad_sch" ) ), "retained source\n" );
    writeDocument( childPath( projectDirectory, wxS( "official/main.kicad_pro" ) ), "official project\n" );
    writeDocument( childPath( projectDirectory, wxS( "official/main.kicad_sch" ) ), "official sheet\n" );
    writeDocument( childPath( projectDirectory, wxS( "official/scratch.kemi_sym" ) ), "native library\n" );
    const wxString archive = directory.ChildPathStr( wxS( "scopes.zip" ) );
    WX_STRING_REPORTER reporter;
    BOOST_REQUIRE( PROJECT_ARCHIVER::Archive( projectDirectory, archive, reporter, false ) );
    const auto entries = zipEntryNames( archive );
    BOOST_CHECK_EQUAL( entries.size(), 5u );
    BOOST_CHECK_EQUAL( entries.count( wxS( "retained.kicad_sch" ) ), 0u );
    const wxString restored = directory.ChildPathStr( wxS( "scopes-restored" ) );
    BOOST_REQUIRE( PROJECT_ARCHIVER::Unarchive( archive, restored, reporter ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( restored, wxS( "official/main.kicad_pro" ) ) ),
                       "official project\n" );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( restored, wxS( "official/main.kicad_sch" ) ) ),
                       "official sheet\n" );
}


// AH11 / P6-P1-3,K04: With no native files on disk, either a serialized native
// target or a copy saver source restricts every member, in either batch order.
BOOST_AUTO_TEST_CASE( FirstNativeAutosaveBatchRejectsOfficialSiblings )
{
    HISTORY_FILE_DATA native;
    native.relativePath = wxS( "main.kemi_sch" );
    native.content = "native sheet\n";
    HISTORY_FILE_DATA official;
    official.relativePath = wxS( "old.kicad_sch" );
    official.content = "official sheet\n";

    for( BACKUP_LOCATION location : { BACKUP_LOCATION::PROJECT_DIR, BACKUP_LOCATION::USER_DIR } )
    {
        const wxString root = directory.ChildPathStr( wxS( "first-autosave" ) );

        for( bool nativeFirst : { false, true } )
        {
            const std::vector<HISTORY_FILE_DATA> batch = nativeFirst
                    ? std::vector<HISTORY_FILE_DATA>{ native, official }
                    : std::vector<HISTORY_FILE_DATA>{ official, native };
            BOOST_CHECK( !LOCAL_HISTORY::WriteAutosaveFiles( root, projectDirectory, location, batch ) );
            BOOST_CHECK( !wxDirExists( root ) );
        }

        HISTORY_FILE_DATA copied;
        copied.relativePath = wxS( "sym-lib-table" );
        copied.sourcePath = directory.ChildPathStr( wxS( "standalone.kemi_sym" ) );
        writeDocument( copied.sourcePath, "native library\n" );
        BOOST_CHECK( !LOCAL_HISTORY::WriteAutosaveFiles( root, projectDirectory, location,
                                                        { official, copied } ) );
        BOOST_CHECK( !wxDirExists( root ) );
    }

    SCOPED_HISTORY_SETTINGS settings;
    writeDocument( childPath( projectDirectory, wxS( "main.kicad_pro" ) ), "official project\n" );
    LOCAL_HISTORY history;
    history.RegisterSaver( &native,
            [&]( const wxString&, std::vector<HISTORY_FILE_DATA>& aData )
            {
                aData = { official, native };
            } );
    BOOST_CHECK( !history.RunRegisteredSaversAndCommit( projectDirectory, wxS( "First autosave" ) ) );
    BOOST_CHECK( !wxDirExists( childPath( projectDirectory, wxS( ".history" ) ) ) );
}


// AH12 / P6-P1-3,K04: A native snapshot must enforce its own policy even if
// no native document remains on disk. Simulate a mixed snapshot made by an old saver.
BOOST_AUTO_TEST_CASE( MixedNativeHistoryRejectsExtractionWithoutNativeDiskFiles )
{
    SCOPED_HISTORY_SETTINGS settings;
    writeDocument( childPath( projectDirectory, wxS( "main.kicad_pro" ) ), "official project\n" );
    LOCAL_HISTORY history;
    BOOST_REQUIRE( history.CommitFullProjectSnapshot( projectDirectory, wxS( "Before conversion" ) ) );

    git_repository* rawRepo = nullptr;
    const wxString historyRoot = childPath( projectDirectory, wxS( ".history" ) );
    BOOST_REQUIRE_EQUAL( git_repository_open( &rawRepo, historyRoot.utf8_str().data() ), 0 );
    std::unique_ptr<git_repository, decltype( &git_repository_free )> repo( rawRepo, git_repository_free );
    git_index* rawIndex = nullptr;
    BOOST_REQUIRE_EQUAL( git_repository_index( &rawIndex, repo.get() ), 0 );
    std::unique_ptr<git_index, decltype( &git_index_free )> index( rawIndex, git_index_free );
    const std::string content = "native project\n";
    git_index_entry entry = {};
    entry.mode = GIT_FILEMODE_BLOB;
    entry.path = "main.kemi_pro";
    BOOST_REQUIRE_EQUAL( git_index_add_from_buffer( index.get(), &entry, content.data(), content.size() ), 0 );
    git_oid treeOid;
    BOOST_REQUIRE_EQUAL( git_index_write_tree( &treeOid, index.get() ), 0 );
    git_tree* rawTree = nullptr;
    BOOST_REQUIRE_EQUAL( git_tree_lookup( &rawTree, repo.get(), &treeOid ), 0 );
    std::unique_ptr<git_tree, decltype( &git_tree_free )> tree( rawTree, git_tree_free );
    git_signature* rawSignature = nullptr;
    BOOST_REQUIRE_EQUAL( git_signature_now( &rawSignature, "QA", "qa@example.invalid" ), 0 );
    std::unique_ptr<git_signature, decltype( &git_signature_free )> signature( rawSignature, git_signature_free );
    git_oid commitOid;
    BOOST_REQUIRE_EQUAL( git_commit_create( &commitOid, repo.get(), nullptr, signature.get(),
                                            signature.get(), nullptr, "Mixed old snapshot", tree.get(),
                                            0, nullptr ), 0 );
    const wxString hash = wxString::FromUTF8( git_oid_tostr_s( &commitOid ) );
    const wxString extracted = directory.ChildPathStr( wxS( "mixed-history-extract" ) );
    BOOST_CHECK( !history.ExtractAllFilesAtCommit( projectDirectory, hash, extracted ) );
    BOOST_CHECK( !wxDirExists( extracted ) );
    BOOST_CHECK( !history.RestoreCommit( projectDirectory, hash, nullptr, false ) );
    BOOST_CHECK( !wxDirExists( childPath( projectDirectory, wxS( "_restore_temp" ) ) ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( projectDirectory, wxS( "main.kicad_pro" ) ) ),
                       "official project\n" );
}


// AH13 / P6-P1-3: Native data known only to the pending saver batch must also
// remove inherited official index entries, without touching the on-disk originals.
BOOST_AUTO_TEST_CASE( FirstNativeHistoryBatchDropsInheritedOfficialEntries )
{
    SCOPED_HISTORY_SETTINGS settings;
    const wxString original = childPath( projectDirectory, wxS( "main.kicad_pro" ) );
    writeDocument( original, "retained project\n" );
    LOCAL_HISTORY history;
    BOOST_REQUIRE( history.CommitFullProjectSnapshot( projectDirectory, wxS( "Before conversion" ) ) );
    const wxString oldHash = history.GetHeadHash( projectDirectory );
    HISTORY_FILE_DATA native;
    native.relativePath = wxS( "main.kemi_sch" );
    native.content = "native sheet\n";
    history.RegisterSaver( &native,
            [&]( const wxString&, std::vector<HISTORY_FILE_DATA>& aData )
            {
                aData = { native };
            } );
    BOOST_REQUIRE( history.RunRegisteredSaversAndCommit( projectDirectory, wxS( "First native batch" ),
                                                        wxS( "sch" ) ) );
    const wxString hash = history.GetHeadHash( projectDirectory );
    BOOST_REQUIRE( hash != oldHash );
    const wxString extracted = directory.ChildPathStr( wxS( "first-native-history" ) );
    BOOST_REQUIRE( history.ExtractAllFilesAtCommit( projectDirectory, hash, extracted ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( childPath( extracted, native.relativePath ) ), native.content );
    BOOST_CHECK( !wxFileExists( childPath( extracted, wxS( "main.kicad_pro" ) ) ) );
    BOOST_CHECK( !wxFileExists( childPath( projectDirectory, native.relativePath ) ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( original ), "retained project\n" );
}


BOOST_AUTO_TEST_SUITE_END()
