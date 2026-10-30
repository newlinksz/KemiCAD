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

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <gestfich.h>
#include <kemi_format_profile.h>
#include <libraries/library_manager.h>
#include <libraries/symbol_library_adapter.h>
#include <lib_symbol.h>
#include <local_history.h>
#include <pgm_base.h>
#include <project.h>
#include <project/project_file.h>
#include <project_sch.h>
#include <richio.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_lib_cache.h>
#include <sch_io/sch_io_lib_cache.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_text.h>
#include <schematic.h>
#include <scoped_set_reset.h>
#include <settings/common_settings.h>
#include <settings/settings_manager.h>
#include <symbol_editor/symbol_editor_tab_context.h>
#include <symbol_library_manager.h>
#include <wx/log.h>

#include <git2.h>
#include <fstream>
#include <iterator>
#include <chrono>
#include <filesystem>
#include <map>
#include <utility>

namespace
{
std::string readBytes( const wxString& aPath )
{
    std::ifstream file( ToFsPath( aPath ), std::ios::binary );
    BOOST_REQUIRE( file.good() );
    return std::string( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
}


void writeBytes( const wxString& aPath, const std::string& aBytes )
{
    std::ofstream file( ToFsPath( aPath ), std::ios::binary );
    file.write( aBytes.data(), aBytes.size() );
    file.close();
    BOOST_REQUIRE( file.good() );
}


void createLibrary( const wxString& aPath, const wxString& aSymbol )
{
    if( KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::SYMBOL_LIB, wxFileName( aPath ).GetExt() ) )
    {
        const wxString fixture = wxString::Format(
                wxS( "(kicad_symbol_lib (version 20260830) (generator eeschema)"
                     "(symbol \"%s\" (property \"Reference\" \"U\" (at 0 0 0)"
                     "(effects (font (size 1 1))))"
                     "(property \"Value\" \"%s\" (at 0 0 0) (effects (font (size 1 1))))))" ),
                aSymbol, aSymbol );
        writeBytes( aPath, fixture.ToStdString( wxConvUTF8 ) );
        return;
    }

    SCH_IO_KICAD_SEXPR io;
    io.CreateLibrary( aPath );
    io.SaveSymbol( aPath, std::make_unique<LIB_SYMBOL>( aSymbol ) );
}


struct PROJECT_SYMBOL_TABLE_RESTORE
{
    LIBRARY_MANAGER& manager;
    wxString         directory;

    ~PROJECT_SYMBOL_TABLE_RESTORE()
    {
        manager.LoadProjectTables( directory, { LIBRARY_TABLE_TYPE::SYMBOL } );
    }
};


void loadManagerSymbolTable( SYMBOL_LIBRARY_ADAPTER& aAdapter, const wxString& aDirectory,
                             const wxString& aLibraryA, const wxString& aLibraryB )
{
    writeBytes( wxFileName( aDirectory, "sym-lib-table" ).GetFullPath(),
                "(sym_lib_table (version 7))\n" );
    Pgm().GetLibraryManager().LoadProjectTables( aDirectory, { LIBRARY_TABLE_TYPE::SYMBOL } );
    LIBRARY_TABLE* table = aAdapter.ProjectTable().value_or( nullptr );
    BOOST_REQUIRE( table );

    for( const auto& [nickname, path] :
         { std::pair<wxString, wxString>( "A", aLibraryA ),
           std::pair<wxString, wxString>( "B", aLibraryB ) } )
    {
        LIBRARY_TABLE_ROW& row = table->InsertRow();
        row.SetNickname( nickname );
        row.SetURI( path );
        row.SetType( "KiCad" );
        row.SetScope( LIBRARY_TABLE_SCOPE::PROJECT );
        aAdapter.LoadOne( nickname );
    }
}


std::map<wxString, wxString> enumeratedDescriptions( SYMBOL_LIBRARY_MANAGER& aManager,
                                                   const wxString& aLibrary,
                                                   bool aReloadCleanBuffer = true )
{
    std::map<wxString, wxString> descriptions;

    for( LIB_SYMBOL* symbol : aManager.EnumerateSymbols( aLibrary, aReloadCleanBuffer ) )
        descriptions.emplace( symbol->GetName(), symbol->GetDescription() );

    return descriptions;
}


void replaceManagerLibraryContents( const wxString& aLibrary )
{
    SCH_IO_KICAD_SEXPR writer;
    writer.DeleteSymbol( aLibrary, "Removed" );
    auto kept = std::make_unique<LIB_SYMBOL>( "Kept" );
    kept->SetDescription( "external-B" );
    writer.SaveSymbol( aLibrary, std::move( kept ) );
    writer.SaveSymbol( aLibrary, std::make_unique<LIB_SYMBOL>( "Added" ) );
}


class EXTERNAL_EDIT_DURING_SAVE : public SCH_IO_KICAD_SEXPR
{
public:
    EXTERNAL_EDIT_DURING_SAVE( const wxString& aPath, const std::string& aContents ) :
            m_path( aPath ),
            m_contents( aContents )
    {}

    void SaveSymbol( const wxString& aLibraryPath, std::unique_ptr<LIB_SYMBOL> aSymbol,
                     const std::map<std::string, UTF8>* aProperties = nullptr ) override
    {
        SCH_IO_KICAD_SEXPR::SaveSymbol( aLibraryPath, std::move( aSymbol ), aProperties );

        if( !m_path.IsEmpty() )
        {
            // Simulate another editor writing after preparation but before the final flush.
            const auto time = std::filesystem::last_write_time( ToFsPath( m_path ) );
            writeBytes( m_path, m_contents );
            std::filesystem::last_write_time( ToFsPath( m_path ), time );
            m_path.Clear();
        }
    }

private:
    wxString    m_path;
    std::string m_contents;
};


struct KEMI_SCHEMATIC_FIXTURE
{
    KEMI_SCHEMATIC_FIXTURE() :
            directory( "kemi-schematic-formats" ),
            schematic( std::make_unique<SCHEMATIC>( nullptr ) )
    {
        wxString projectPath = directory.ChildPathStr( wxS( "project.kemi_pro" ) );
        PROJECT seedProject;
        PROJECT_FILE seedFile( projectPath );
        seedFile.SetProject( &seedProject );

        // Use the native serializer; official documents are read-only fixtures.
        BOOST_REQUIRE( seedFile.SaveToFile( wxEmptyString, true ) );
        BOOST_REQUIRE( settings.LoadProject( projectPath ) );
        schematic->SetProject( &settings.Prj() );
    }

    ~KEMI_SCHEMATIC_FIXTURE()
    {
        schematic.reset();
        settings.UnloadProject( &settings.Prj(), false );
    }

    SCH_SHEET* loadSchematic( SCH_IO_KICAD_SEXPR& aIo, const wxString& aPath )
    {
        schematic->Reset();
        SCH_SHEET* oldDefault = schematic->GetTopLevelSheet( 0 );
        SCH_SHEET* loaded = aIo.LoadSchematicFile( aPath, schematic.get() );
        BOOST_REQUIRE( loaded );
        schematic->AddTopLevelSheet( loaded );
        schematic->RemoveTopLevelSheet( oldDefault );
        delete oldDefault;
        schematic->RefreshHierarchy();
        return loaded;
    }

    KI_TEST::SCOPED_TEMP_DIR    directory;
    SETTINGS_MANAGER           settings;
    std::unique_ptr<SCHEMATIC> schematic;
};


struct LIBGIT2_SCOPE
{
    LIBGIT2_SCOPE() { git_libgit2_init(); }
    ~LIBGIT2_SCOPE() { git_libgit2_shutdown(); }
};
}


BOOST_FIXTURE_TEST_SUITE( KemiSchematicFormats, KEMI_SCHEMATIC_FIXTURE )

BOOST_AUTO_TEST_CASE( SchematicFileRoundTrip )
{
    for( const wxString& ext : { wxString( "kemi_sch" ) } )
    {
        BOOST_TEST_CONTEXT( ext )
        {
            schematic->Reset();
            SCH_SHEET* sheet = schematic->GetTopLevelSheet( 0 );
            const wxString path = directory.ChildPathStr( "roundtrip." + ext );
            sheet->SetFileName( path );
            sheet->GetScreen()->SetFileName( path );
            sheet->GetScreen()->Append( new SCH_TEXT( VECTOR2I( 1000, 2000 ), "roundtrip payload" ) );

            SCH_IO_KICAD_SEXPR writer;
            writer.SaveSchematicFile( path, sheet, schematic.get() );
            BOOST_CHECK( readBytes( path ).find( ext == "kemi_sch" ? "(kemi_sch" : "(kicad_sch" ) == 0 );

            SCH_IO_KICAD_SEXPR reader;
            SCH_SHEET* loaded = loadSchematic( reader, path );
            BOOST_CHECK( reader.GetError().IsEmpty() );
            BOOST_CHECK( !loaded->GetScreen()->HasLoadError() );
            size_t textCount = 0;

            for( SCH_ITEM* item : loaded->GetScreen()->Items().OfType( SCH_TEXT_T ) )
            {
                BOOST_CHECK_EQUAL( static_cast<SCH_TEXT*>( item )->GetText(), "roundtrip payload" );
                ++textCount;
            }

            BOOST_CHECK_EQUAL( textCount, 1 );
        }
    }
}


BOOST_AUTO_TEST_CASE( RejectedChildCannotBeSavedOrAutosaved )
{
    SCH_SHEET* root = schematic->GetTopLevelSheet( 0 );
    const wxString rootPath = directory.ChildPathStr( wxS( "root.kemi_sch" ) );
    const wxString childPath = directory.ChildPathStr( "bad.kicad_sch" );
    root->SetFileName( rootPath );
    root->GetScreen()->SetFileName( rootPath );
    auto child = new SCH_SHEET( schematic.get() );
    child->SetName( "Rejected child" );
    child->SetFileName( "bad.kicad_sch" );
    child->SetScreen( new SCH_SCREEN( schematic.get() ) );
    child->GetScreen()->SetFileName( childPath );
    root->GetScreen()->Append( child );

    SCH_IO_KICAD_SEXPR writer;
    writer.SaveSchematicFile( rootPath, root, schematic.get() );
    const std::string rejected = "(kemi_sch (format_version 2) (version 20250114))\n";
    writeBytes( childPath, rejected );

    SCH_IO_KICAD_SEXPR reader;
    root = loadSchematic( reader, rootPath );
    BOOST_CHECK( !reader.GetError().IsEmpty() );
    child = nullptr;

    for( SCH_ITEM* item : root->GetScreen()->Items().OfType( SCH_SHEET_T ) )
        child = static_cast<SCH_SHEET*>( item );

    BOOST_REQUIRE( child && child->GetScreen() );
    BOOST_REQUIRE( child->GetScreen()->HasLoadError() );
    child->GetScreen()->SetFileReadOnly( false );
    BOOST_CHECK( child->GetScreen()->IsReadOnly() );
    BOOST_CHECK_THROW( writer.SaveSchematicFile( childPath, child, schematic.get() ), IO_ERROR );
    const wxString nativeChildPath = directory.ChildPathStr( "bad.kemi_sch" );
    BOOST_CHECK_THROW( writer.SaveSchematicFile( nativeChildPath, child, schematic.get() ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( nativeChildPath ) );
    STRING_FORMATTER formatter;
    BOOST_CHECK_THROW( writer.FormatSchematicToFormatter( &formatter, child, schematic.get(),
                                                         nullptr, KEMI_FORMAT::FLAVOR::KEMI ), IO_ERROR );

    root->GetScreen()->SetContentModified( true );
    child->GetScreen()->SetContentModified( true );
    std::vector<HISTORY_FILE_DATA> data;
    schematic->SaveToHistory( settings.Prj().GetProjectPath(), data );
    BOOST_REQUIRE_EQUAL( data.size(), 1 );
    BOOST_CHECK_EQUAL( wxFileName( data.front().relativePath ).GetFullName(), "root.kemi_sch" );
    BOOST_CHECK( data.front().content.find( "(kemi_sch" ) == 0 );
    BOOST_CHECK_EQUAL( readBytes( childPath ), rejected );
}


BOOST_AUTO_TEST_CASE( SymbolFileRoundTrip )
{
    for( const wxString& ext : { wxString( "kicad_sym" ), wxString( "kemi_sym" ) } )
    {
        const wxString path = directory.ChildPathStr( "symbols." + ext );
        createLibrary( path, "Roundtrip" );
        SCH_IO_KICAD_SEXPR reader;
        LIB_SYMBOL* loaded = reader.LoadSymbol( path, "Roundtrip" );
        BOOST_REQUIRE( loaded );
        BOOST_CHECK_EQUAL( loaded->GetName(), "Roundtrip" );
        BOOST_CHECK( readBytes( path ).find( ext == "kemi_sym" ? "(kemi_symbol_lib" : "(kicad_symbol_lib" ) == 0 );
    }
}


BOOST_AUTO_TEST_CASE( MissingChildCanBeCreated )
{
    SCH_SHEET* root = schematic->GetTopLevelSheet( 0 );
    const wxString rootPath = directory.ChildPathStr( wxS( "missing-root.kemi_sch" ) );
    const wxString childPath = directory.ChildPathStr( wxS( "missing.kemi_sch" ) );
    root->SetFileName( rootPath );
    root->GetScreen()->SetFileName( rootPath );
    auto child = new SCH_SHEET( schematic.get() );
    child->SetName( "New child" );
    child->SetFileName( wxS( "missing.kemi_sch" ) );
    child->SetScreen( new SCH_SCREEN( schematic.get() ) );
    root->GetScreen()->Append( child );
    SCH_IO_KICAD_SEXPR writer;
    writer.SaveSchematicFile( rootPath, root, schematic.get() );
    SCH_IO_KICAD_SEXPR reader;
    root = loadSchematic( reader, rootPath );
    child = nullptr;

    for( SCH_ITEM* item : root->GetScreen()->Items().OfType( SCH_SHEET_T ) )
        child = static_cast<SCH_SHEET*>( item );

    BOOST_REQUIRE( child && child->GetScreen() );
    BOOST_CHECK( !child->GetScreen()->HasLoadError() );
    BOOST_CHECK_NO_THROW( writer.SaveSchematicFile( childPath, child, schematic.get() ) );
    BOOST_CHECK( wxFileExists( childPath ) );
}


BOOST_AUTO_TEST_CASE( RejectedSymbolFileCannotBeOverwritten )
{
    const std::vector<std::string> inputs = {
        "(kemi_symbol_lib (format_version 2) (version 20231120) (generator test))\n",
        "(kemi_symbol_lib (format_version 1.5) (version 20231120) (generator test))\n",
        "(kicad_symbol_lib (version 20231120) (generator test))\n"
    };

    for( size_t i = 0; i < inputs.size(); ++i )
    {
        BOOST_TEST_CONTEXT( "rejected input " << i )
        {
            const wxString path = directory.ChildPathStr(
                    wxString::Format( "rejected%zu.%s", i, i == 0 ? "kicad_sym" : "kemi_sym" ) );
            writeBytes( path, inputs[i] );
            SCH_IO_KICAD_SEXPR io;
            BOOST_CHECK_THROW( io.LoadSymbol( path, "Absent" ), IO_ERROR );
            BOOST_CHECK_THROW( io.LoadSymbol( path, "Absent" ), IO_ERROR );
            BOOST_CHECK_THROW( io.SaveLibrary( path ), IO_ERROR );
            BOOST_CHECK_THROW( io.SaveSymbol( path, std::make_unique<LIB_SYMBOL>( "Replacement" ) ), IO_ERROR );
            BOOST_CHECK_THROW( io.DeleteLibrary( path ), IO_ERROR );
            BOOST_CHECK_EQUAL( readBytes( path ), inputs[i] );
        }
    }
}


BOOST_AUTO_TEST_CASE( SchematicRejectsFractionalContainerVersion )
{
    const wxString path = directory.ChildPathStr( "fractional.kemi_sch" );
    const std::string contents = "(kemi_sch (format_version 1.5) (version 20250114))\n";
    writeBytes( path, contents );
    SCH_IO_KICAD_SEXPR io;
    BOOST_CHECK_THROW( io.LoadSchematicFile( path, schematic.get() ), IO_ERROR );
    BOOST_CHECK_EQUAL( readBytes( path ), contents );
}


BOOST_AUTO_TEST_CASE( RejectedSymbolSaveAsTargetRemainsUnchanged )
{
    const wxString source = directory.ChildPathStr( "source.kemi_sym" );
    const wxString target = directory.ChildPathStr( "future.kicad_sym" );
    createLibrary( source, "Source" );
    const std::string invalid = "(kemi_symbol_lib (format_version 2) (version 20231120))\n";
    writeBytes( target, invalid );
    SCH_IO_KICAD_SEXPR io;
    BOOST_REQUIRE( io.LoadSymbol( source, "Source" ) );
    BOOST_CHECK_THROW( io.SaveLibrary( target ), IO_ERROR );
    BOOST_CHECK_EQUAL( readBytes( target ), invalid );
    BOOST_REQUIRE( io.LoadSymbol( source, "Source" ) );
}


BOOST_AUTO_TEST_CASE( DirectoryNativeMembersShadowPreservedOfficialSources )
{
    const wxString dir = directory.CreateChildDirStr( "shadow.kicad_symdir" );
    const wxString nativePath = wxFileName( dir, "Part.kemi_sym" ).GetFullPath();
    const wxString legacyPath = wxFileName( dir, "Part.kicad_sym" ).GetFullPath();
    createLibrary( nativePath, "NativeOnly" );
    createLibrary( legacyPath, "LegacyOnly" );
    const std::string legacyBytes = readBytes( legacyPath );
    SCH_IO_KICAD_SEXPR io;
    BOOST_REQUIRE( io.LoadSymbol( dir, "NativeOnly" ) );
    BOOST_CHECK( !io.LoadSymbol( dir, "LegacyOnly" ) );
    wxArrayString names;
    io.EnumerateSymbolLib( names, dir );
    BOOST_REQUIRE_EQUAL( names.size(), 1U );
    BOOST_CHECK_NO_THROW( io.SaveLibrary( dir ) );
    BOOST_CHECK_EQUAL( readBytes( legacyPath ), legacyBytes );
}


BOOST_AUTO_TEST_CASE( DirectoryDetectsNativeChangesAndNewConflicts )
{
    const wxString dir = directory.CreateChildDirStr( "changing.kicad_symdir" );
    const wxString nativePath = wxFileName( dir, "Part.kemi_sym" ).GetFullPath();
    createLibrary( nativePath, "Original" );
    SCH_IO_KICAD_SEXPR reader;
    BOOST_REQUIRE( reader.LoadSymbol( dir, "Original" ) );

    SCH_IO_KICAD_SEXPR externalWriter;
    externalWriter.SaveSymbol( nativePath, std::make_unique<LIB_SYMBOL>( "Added" ) );
    BOOST_REQUIRE( reader.LoadSymbol( dir, "Added" ) );

    SCH_IO_KICAD_SEXPR_LIB_CACHE cache( dir );
    cache.Load();
    const wxString renamed = wxFileName( dir, "Renamed.kemi_sym" ).GetFullPath();
    BOOST_REQUIRE( wxRenameFile( nativePath, renamed ) );
    BOOST_CHECK( cache.IsFileChanged() );
    cache.Load();
    BOOST_REQUIRE( wxRemoveFile( renamed ) );
    BOOST_CHECK( cache.IsFileChanged() );
    cache.Load();
    BOOST_CHECK( cache.GetSymbolMap().empty() );

    createLibrary( nativePath, "Original" );
    BOOST_CHECK( cache.IsFileChanged() );
    cache.Load();
    const wxString legacyPath = wxFileName( dir, "Part.kicad_sym" ).GetFullPath();
    BOOST_REQUIRE( wxCopyFile( nativePath, legacyPath ) );
    BOOST_CHECK( cache.IsFileChanged() );
    BOOST_CHECK_NO_THROW( cache.Load() );
    BOOST_REQUIRE( reader.LoadSymbol( dir, "Original" ) );
}


BOOST_AUTO_TEST_CASE( DirectorySaveCannotCreateOppositeFormatSibling )
{
    const wxString dir = directory.CreateChildDirStr( "output.kicad_symdir" );
    const wxString nativePath = wxFileName( dir, "Foo.kemi_sym" ).GetFullPath();
    const wxString legacyPath = wxFileName( dir, "Foo.kicad_sym" ).GetFullPath();
    createLibrary( nativePath, "Bar" );
    SCH_IO_KICAD_SEXPR io;
    BOOST_REQUIRE( io.LoadSymbol( dir, "Bar" ) );

    // New symbols in a native library retain its format and share the existing file.
    BOOST_REQUIRE_NO_THROW( io.SaveSymbol( dir, std::make_unique<LIB_SYMBOL>( "Foo" ) ) );
    BOOST_REQUIRE_NO_THROW( io.SaveLibrary( dir ) );
    BOOST_CHECK( !wxFileExists( legacyPath ) );
    BOOST_CHECK( readBytes( nativePath ).find( "(kemi_symbol_lib" ) == 0 );
    SCH_IO_KICAD_SEXPR reader;
    BOOST_REQUIRE( reader.LoadSymbol( nativePath, "Bar" ) );
    BOOST_REQUIRE( reader.LoadSymbol( nativePath, "Foo" ) );

    // An official group is converted as a whole while retaining its original bytes.
    const wxString legacyDir = directory.CreateChildDirStr( "legacy-output.kicad_symdir" );
    const wxString legacySource = wxFileName( legacyDir, "Foo.kicad_sym" ).GetFullPath();
    const wxString nativeSibling = wxFileName( legacyDir, "Foo.kemi_sym" ).GetFullPath();
    createLibrary( legacySource, "Bar" );
    const std::string original = readBytes( legacySource );
    SCH_IO_KICAD_SEXPR legacyWriter;
    BOOST_REQUIRE( legacyWriter.LoadSymbol( legacyDir, "Bar" ) );
    auto restricted = std::make_unique<LIB_SYMBOL>( "Foo" );
    restricted->SetKemiOnly();
    BOOST_CHECK_NO_THROW( legacyWriter.SaveSymbol( legacyDir, std::move( restricted ) ) );
    BOOST_CHECK_NO_THROW( legacyWriter.SaveLibrary( legacyDir ) );
    BOOST_CHECK_EQUAL( readBytes( legacySource ), original );
    BOOST_CHECK( wxFileExists( nativeSibling ) );
    SCH_IO_KICAD_SEXPR reopened;
    BOOST_REQUIRE( reopened.LoadSymbol( legacyDir, "Bar" ) );
    BOOST_REQUIRE( reopened.LoadSymbol( legacyDir, "Foo" ) );
}


BOOST_AUTO_TEST_CASE( OfficialDirectoryMembersSaveRepeatedlyAndLoadedCacheCopiesNatively )
{
    const wxString source = directory.CreateChildDirStr( "official-source.kicad_symdir" );
    const wxString official = wxFileName( source, "Group.kicad_sym" ).GetFullPath();
    createLibrary( official, "Part" );
    const std::string before = readBytes( official );
    const wxString copy = directory.CreateChildDirStr( "official-copy.kicad_symdir" );
    SCH_IO_KICAD_SEXPR io;
    BOOST_REQUIRE( io.LoadSymbol( source, "Part" ) );
    io.SaveLibrary( copy );
    BOOST_CHECK( !wxFileExists( wxFileName( copy, "Group.kicad_sym" ).GetFullPath() ) );
    SCH_IO_KICAD_SEXPR reader;
    BOOST_REQUIRE( reader.LoadSymbol( copy, "Part" ) );
    BOOST_CHECK_EQUAL( readBytes( official ), before );

    auto edited = std::make_unique<LIB_SYMBOL>( *io.LoadSymbol( source, "Part" ) );
    edited->SetDescription( wxS( "first save" ) );
    io.SaveSymbol( source, std::move( edited ) );
    edited = std::make_unique<LIB_SYMBOL>( *io.LoadSymbol( source, "Part" ) );
    edited->SetDescription( wxS( "second save" ) );
    io.SaveSymbol( source, std::move( edited ) );
    SCH_IO_KICAD_SEXPR reopened;
    BOOST_REQUIRE( reopened.LoadSymbol( source, "Part" ) );
    BOOST_CHECK_EQUAL( reopened.LoadSymbol( source, "Part" )->GetDescription(), wxS( "second save" ) );
    BOOST_CHECK_EQUAL( readBytes( official ), before );
    io.DeleteSymbol( source, "Part" );
    SCH_IO_KICAD_SEXPR afterDelete;
    BOOST_CHECK( !afterDelete.LoadSymbol( source, "Part" ) );
    BOOST_CHECK_EQUAL( readBytes( official ), before );
}


BOOST_AUTO_TEST_CASE( LoadedOfficialDirectoryCopyRejectsExistingNativeGroup )
{
    const wxString source = directory.CreateChildDirStr( "copy-source.kicad_symdir" );
    const wxString target = directory.CreateChildDirStr( "copy-target.kicad_symdir" );
    const wxString official = wxFileName( source, "Group.kicad_sym" ).GetFullPath();
    const wxString native = wxFileName( target, "Group.kemi_sym" ).GetFullPath();
    createLibrary( official, "Source" );
    createLibrary( native, "Sentinel" );
    const std::string sourceBefore = readBytes( official );
    const std::string targetBefore = readBytes( native );
    SCH_IO_KICAD_SEXPR io;
    BOOST_REQUIRE( io.LoadSymbol( source, "Source" ) );
    BOOST_CHECK_THROW( io.SaveLibrary( target ), IO_ERROR );
    BOOST_CHECK_EQUAL( readBytes( official ), sourceBefore );
    BOOST_CHECK_EQUAL( readBytes( native ), targetBefore );
    BOOST_REQUIRE( io.LoadSymbol( source, "Source" ) );
}


BOOST_AUTO_TEST_CASE( DirectorySaveRefusesExternalEdits )
{
    const wxString dir = directory.CreateChildDirStr( "external.kicad_symdir" );
    const wxString nativePath = wxFileName( dir, "Part.kemi_sym" ).GetFullPath();
    createLibrary( nativePath, "Original" );
    SCH_IO_KICAD_SEXPR io;
    BOOST_REQUIRE( io.LoadSymbol( dir, "Original" ) );
    std::map<std::string, UTF8> properties = { { SCH_IO_KICAD_SEXPR::PropBuffering, "" } };
    io.SaveSymbol( dir, std::make_unique<LIB_SYMBOL>( "Unsaved" ), &properties );
    const std::string externalBytes = readBytes( nativePath ) + "\n";
    writeBytes( nativePath, externalBytes );
    BOOST_CHECK_THROW( io.SaveLibrary( dir ), IO_ERROR );
    BOOST_CHECK_EQUAL( readBytes( nativePath ), externalBytes );
    SCH_IO_KICAD_SEXPR reloaded;
    BOOST_REQUIRE( reloaded.LoadSymbol( dir, "Original" ) );
}


BOOST_AUTO_TEST_CASE( SymbolLibraryStampTracksBothFormats )
{
    const wxString dir = directory.CreateChildDirStr( "stamps.kicad_symdir" );
    long long stamp = SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( dir );

    for( const std::string& ext : KEMI_FORMAT::ReadableExts( KEMI_FORMAT::DOC::SYMBOL_LIB ) )
    {
        const wxString path = wxFileName( dir, "Part." + wxString( ext ) ).GetFullPath();
        createLibrary( path, wxString( ext ) );
        long long current = SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( dir );
        BOOST_CHECK_NE( stamp, current );
        stamp = current;

        writeBytes( path, readBytes( path ) + "\n" );
        current = SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( dir );
        BOOST_CHECK_NE( stamp, current );
        stamp = current;

        const wxString renamed = wxFileName( dir, "Renamed." + wxString( ext ) ).GetFullPath();
        BOOST_REQUIRE( wxRenameFile( path, renamed ) );
        current = SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( dir );
        BOOST_CHECK_NE( stamp, current );
        stamp = current;

        BOOST_REQUIRE( wxRemoveFile( renamed ) );
        current = SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( dir );
        BOOST_CHECK_NE( stamp, current );
        stamp = current;
    }

    const wxString ignored = wxFileName( dir, "ignored.txt" ).GetFullPath();
    writeBytes( ignored, "unrelated" );
    BOOST_CHECK_EQUAL( stamp, SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( dir ) );
    BOOST_REQUIRE( wxRemoveFile( ignored ) );
    BOOST_REQUIRE( wxFileName::Rmdir( dir ) );
    BOOST_CHECK_NE( stamp, SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( dir ) );
}


BOOST_AUTO_TEST_CASE( OfficialLibraryRefreshAndExternalChangeDetectionRemainReadable )
{
    // Inactive clean / inactive modified / current clean / Reference-only external change.
    for( int scenario : { 0, 1, 2, 3 } )
    {
        const wxString dir = directory.CreateChildDirStr( wxString::Format( wxS( "official-refresh-%d" ), scenario ) );
        const wxString libraryA = wxFileName( dir, wxS( "A.kicad_sym" ) ).GetFullPath();
        const wxString libraryB = wxFileName( dir, wxS( "B.kicad_sym" ) ).GetFullPath();
        createLibrary( libraryA, wxS( "Current" ) );
        auto externalRevision = [&]( bool aChanged )
        {
            // These bytes model another official editor. Production writers only write native targets.
            const wxString text = wxString::Format(
                    wxS( "(kicad_symbol_lib (version 20260830) (generator eeschema)"
                         "(symbol \"Kept\" (property \"Reference\" \"%s\" (at 0 0 0) (effects (font (size 1 1))))"
                         "(property \"Value\" \"Kept\" (at 0 0 0) (effects (font (size 1 1))))"
                         "(property \"Description\" \"%s\" (at 0 0 0) (effects (font (size 1 1)))))"
                         "(symbol \"%s\" (property \"Reference\" \"U\" (at 0 0 0) (effects (font (size 1 1))))"
                         "(property \"Value\" \"Other\" (at 0 0 0) (effects (font (size 1 1))))))" ),
                    aChanged && scenario == 3 ? wxS( "J" ) : wxS( "U" ),
                    aChanged && scenario != 3 ? wxS( "external-B" ) : wxS( "original-B" ),
                    aChanged && scenario != 3 ? wxS( "Added" ) : wxS( "Removed" ) );
            writeBytes( libraryB, text.ToStdString( wxConvUTF8 ) + ( aChanged ? "\n\n" : "\n" ) );
        };
        externalRevision( false );
        SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &settings.Prj() );
        BOOST_REQUIRE( adapter );
        const auto previousTable = adapter->ProjectTable();
        PROJECT_SYMBOL_TABLE_RESTORE restore{
            Pgm().GetLibraryManager(),
            previousTable && *previousTable ? wxFileName( ( *previousTable )->Path() ).GetPath() : wxString()
        };
        loadManagerSymbolTable( *adapter, dir, libraryA, libraryB );
        SYMBOL_LIBRARY_MANAGER manager( settings.Prj() );
        BOOST_REQUIRE( manager.GetBufferedSymbol( wxS( "Kept" ), wxS( "B" ) ) );
        SYMBOL_BUFFER* buffer = manager.GetBuffer( wxS( "Kept" ), wxS( "B" ) );
        BOOST_REQUIRE( buffer );
        SYMBOL_EDITOR_TAB_CONTEXT tab( wxS( "B" ), wxS( "Kept" ), buffer );
        const int originalHash = manager.GetLibraryHash( wxS( "B" ) );

        if( scenario == 1 )
        {
            LIB_SYMBOL edited( buffer->GetSymbol() );
            edited.SetDescription( wxS( "local-B" ) );
            BOOST_REQUIRE( manager.UpdateSymbol( &edited, wxS( "B" ) ) );
        }

        if( scenario != 2 )
            BOOST_REQUIRE( manager.GetBufferedSymbol( wxS( "Current" ), wxS( "A" ) ) );

        externalRevision( true );
        const std::string external = readBytes( libraryB );
        BOOST_CHECK_NE( manager.GetLibraryHash( wxS( "B" ) ), originalHash );
        const auto descriptions = enumeratedDescriptions( manager, wxS( "B" ), scenario != 2 );
        BOOST_REQUIRE( descriptions.contains( wxS( "Kept" ) ) );
        BOOST_CHECK_EQUAL( descriptions.at( wxS( "Kept" ) ), scenario == 1 ? wxS( "local-B" )
                : scenario == 2 || scenario == 3 ? wxS( "original-B" ) : wxS( "external-B" ) );
        BOOST_CHECK_EQUAL( descriptions.count( wxS( "Removed" ) ), scenario == 0 ? 0u : 1u );
        BOOST_CHECK_EQUAL( descriptions.count( wxS( "Added" ) ), scenario == 0 ? 1u : 0u );
        buffer = manager.GetBuffer( wxS( "Kept" ), wxS( "B" ), scenario != 2 );
        BOOST_REQUIRE( buffer );

        if( scenario == 3 )
        {
            BOOST_CHECK_EQUAL( buffer->GetOriginal().GetReferenceField().GetText(), wxS( "J" ) );
            BOOST_CHECK_EQUAL( tab.GetSymbol()->Compare( buffer->GetSymbol() ), 0 );
            BOOST_CHECK( !tab.MatchesLibrarySymbol( buffer->GetSymbol() ) );
        }

        if( scenario == 2 )
        {
            LIB_SYMBOL edited( buffer->GetSymbol() );
            edited.SetDescription( wxS( "local-current-B" ) );
            BOOST_REQUIRE( manager.UpdateSymbol( &edited, wxS( "B" ) ) );
        }

        {
            wxLogNull silence;
            BOOST_CHECK( !manager.SaveLibrary( wxS( "B" ), libraryB, SCH_IO_MGR::SCH_KICAD ) );
        }
        BOOST_CHECK_EQUAL( readBytes( libraryB ), external );

        // Fresh read and Save As keep the official revision and put all writer assertions on native output.
        const wxString native = wxFileName( dir, wxS( "copy.kemi_sym" ) ).GetFullPath();
        SCH_IO_KICAD_SEXPR reader;
        BOOST_REQUIRE( reader.LoadSymbol( libraryB, wxS( "Kept" ) ) );
        BOOST_CHECK_NO_THROW( reader.SaveLibrary( native ) );
        BOOST_CHECK( readBytes( native ).starts_with( "(kemi_symbol_lib" ) );
        SCH_IO_KICAD_SEXPR reopened;
        BOOST_REQUIRE( reopened.LoadSymbol( native, wxS( "Kept" ) ) );
        BOOST_CHECK_EQUAL( readBytes( libraryB ), external );
    }
}


BOOST_AUTO_TEST_CASE( ManagerRefreshReloadsCleanNonCurrentLibrary )
{
    for( const wxString& ext : { wxString( "kemi_sym" ) } )
    {
        BOOST_TEST_CONTEXT( ext )
        {
            const wxString dir = directory.CreateChildDirStr( "manager-clean-" + ext );
            const wxString libraryA = wxFileName( dir, "A." + ext ).GetFullPath();
            const wxString libraryB = wxFileName( dir, "B." + ext ).GetFullPath();
            createLibrary( libraryA, "Current" );
            createLibrary( libraryB, "Removed" );
            SCH_IO_KICAD_SEXPR seed;
            auto kept = std::make_unique<LIB_SYMBOL>( "Kept" );
            kept->SetDescription( "original-B" );
            seed.SaveSymbol( libraryB, std::move( kept ) );

            SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &settings.Prj() );
            BOOST_REQUIRE( adapter );
            const auto previousTable = adapter->ProjectTable();
            PROJECT_SYMBOL_TABLE_RESTORE restore{
                Pgm().GetLibraryManager(),
                previousTable && *previousTable ? wxFileName( ( *previousTable )->Path() ).GetPath()
                                                : wxString()
            };
            loadManagerSymbolTable( *adapter, dir, libraryA, libraryB );
            SYMBOL_LIBRARY_MANAGER manager( settings.Prj() );

            // Opening B populates its edit buffers; opening A represents switching away.
            BOOST_REQUIRE( manager.GetBufferedSymbol( "Kept", "B" ) );
            BOOST_CHECK( !manager.IsLibraryModified( "B" ) );
            const int originalHash = manager.GetLibraryHash( "B" );
            BOOST_REQUIRE( manager.GetBufferedSymbol( "Current", "A" ) );
            replaceManagerLibraryContents( libraryB );

            // The tree reads the hash before enumeration, then records the hash after refresh.
            BOOST_CHECK_NE( manager.GetLibraryHash( "B" ), originalHash );
            const auto refreshed = enumeratedDescriptions( manager, "B" );
            const int refreshedHash = manager.GetLibraryHash( "B" );
            BOOST_CHECK_EQUAL( refreshed.size(), 2 );
            BOOST_CHECK( refreshed.contains( "Added" ) );
            BOOST_CHECK( !refreshed.contains( "Removed" ) );
            BOOST_REQUIRE( refreshed.contains( "Kept" ) );
            BOOST_CHECK_EQUAL( refreshed.at( "Kept" ), "external-B" );
            BOOST_CHECK( !manager.IsLibraryModified( "B" ) );
            BOOST_CHECK_NE( refreshedHash, originalHash );

            const auto repeated = enumeratedDescriptions( manager, "B" );
            BOOST_CHECK( repeated == refreshed );
            BOOST_CHECK_EQUAL( manager.GetLibraryHash( "B" ), refreshedHash );

            // Returning to B must use the same new disk revision as the refreshed tree.
            BOOST_CHECK( !manager.GetBufferedSymbol( "Removed", "B" ) );
            BOOST_CHECK( !manager.GetSymbol( "Removed", "B" ) );
            LIB_SYMBOL* returned = manager.GetBufferedSymbol( "Kept", "B" );
            BOOST_REQUIRE( returned );
            BOOST_CHECK_EQUAL( returned->GetDescription(), "external-B" );
            SYMBOL_BUFFER* buffer = manager.GetBuffer( "Kept", "B" );
            BOOST_REQUIRE( buffer );
            BOOST_CHECK_EQUAL( buffer->GetOriginal().GetDescription(), "external-B" );

            // Tab activation explicitly reloads clean buffers even without a tree refresh.
            BOOST_REQUIRE( manager.GetBufferedSymbol( "Current", "A" ) );
            SCH_IO_KICAD_SEXPR laterWriter;
            auto later = std::make_unique<LIB_SYMBOL>( "Kept" );
            later->SetDescription( "external-on-return" );
            laterWriter.SaveSymbol( libraryB, std::move( later ) );
            buffer = manager.GetBuffer( "Kept", "B", true );
            BOOST_REQUIRE( buffer );
            BOOST_CHECK_EQUAL( buffer->GetOriginal().GetDescription(), "external-on-return" );
            returned = manager.GetBufferedSymbol( "Kept", "B" );
            BOOST_REQUIRE( returned );
            BOOST_CHECK_EQUAL( returned->GetDescription(), "external-on-return" );

            LIB_SYMBOL edited( *returned );
            edited.SetDescription( "local-after-refresh" );
            BOOST_REQUIRE( manager.UpdateSymbol( &edited, "B" ) );
            BOOST_REQUIRE( manager.SaveLibrary( "B", libraryB, SCH_IO_MGR::SCH_KICAD ) );
            SCH_IO_KICAD_SEXPR reader;
            BOOST_REQUIRE( reader.LoadSymbol( libraryB, "Kept" ) );
            BOOST_CHECK_EQUAL( reader.LoadSymbol( libraryB, "Kept" )->GetDescription(),
                               "local-after-refresh" );
            BOOST_REQUIRE( reader.LoadSymbol( libraryB, "Added" ) );
            BOOST_CHECK( !reader.LoadSymbol( libraryB, "Removed" ) );
        }
    }
}


// A clean inactive tab must notice a Reference-only change before accepting the new watcher
// baseline. LIB_SYMBOL::Compare intentionally ignores Reference text for schematic matching.
BOOST_AUTO_TEST_CASE( CleanSymbolTabDetectsExternalReferenceChange )
{
    for( const wxString& ext : { wxString( "kemi_sym" ) } )
    {
        BOOST_TEST_CONTEXT( ext )
        {
            const wxString dir = directory.CreateChildDirStr( "tab-reference-" + ext );
            const wxString libraryA = wxFileName( dir, "A." + ext ).GetFullPath();
            const wxString libraryB = wxFileName( dir, "B." + ext ).GetFullPath();
            createLibrary( libraryA, "Current" );
            createLibrary( libraryB, "Part" );
            SCH_IO_KICAD_SEXPR seed;
            auto original = std::make_unique<LIB_SYMBOL>( "Part" );
            original->SetRefProp( "R" );
            seed.SaveSymbol( libraryB, std::move( original ) );

            SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &settings.Prj() );
            BOOST_REQUIRE( adapter );
            const auto previousTable = adapter->ProjectTable();
            PROJECT_SYMBOL_TABLE_RESTORE restore{
                Pgm().GetLibraryManager(),
                previousTable && *previousTable ? wxFileName( ( *previousTable )->Path() ).GetPath()
                                                : wxString()
            };
            loadManagerSymbolTable( *adapter, dir, libraryA, libraryB );
            SYMBOL_LIBRARY_MANAGER manager( settings.Prj() );
            SYMBOL_BUFFER* buffer = manager.GetBuffer( "Part", "B" );
            BOOST_REQUIRE( buffer );
            SYMBOL_EDITOR_TAB_CONTEXT tab( "B", "Part", buffer );
            BOOST_CHECK( !tab.IsModified() );
            BOOST_CHECK( tab.MatchesLibrarySymbol( buffer->GetSymbol() ) );
            BOOST_CHECK_EQUAL( tab.GetSymbol()->GetReferenceField().GetText(), "R" );
            BOOST_REQUIRE( manager.GetBufferedSymbol( "Current", "A" ) );

            SCH_IO_KICAD_SEXPR externalWriter;
            auto changed = std::make_unique<LIB_SYMBOL>( *tab.GetSymbol() );
            changed->SetRefProp( "U" );
            externalWriter.SaveSymbol( libraryB, std::move( changed ) );
            buffer = manager.GetBuffer( "Part", "B", true );
            BOOST_REQUIRE( buffer );
            BOOST_CHECK_EQUAL( buffer->GetSymbol().GetReferenceField().GetText(), "U" );
            BOOST_CHECK_EQUAL( buffer->GetOriginal().GetReferenceField().GetText(), "U" );
            BOOST_CHECK( !tab.IsModified() );

            // Exercise the activation predicate itself, preserving the global Compare contract.
            BOOST_CHECK_EQUAL( tab.GetSymbol()->Compare( buffer->GetSymbol() ), 0 );
            BOOST_CHECK( !tab.MatchesLibrarySymbol( buffer->GetSymbol() ) );
        }
    }
}


BOOST_AUTO_TEST_CASE( ManagerRefreshPreservesModifiedNonCurrentLibrary )
{
    for( const wxString& ext : { wxString( "kemi_sym" ) } )
    {
        BOOST_TEST_CONTEXT( ext )
        {
            const wxString dir = directory.CreateChildDirStr( "manager-modified-" + ext );
            const wxString libraryA = wxFileName( dir, "A." + ext ).GetFullPath();
            const wxString libraryB = wxFileName( dir, "B." + ext ).GetFullPath();
            createLibrary( libraryA, "Current" );
            createLibrary( libraryB, "Removed" );
            SCH_IO_KICAD_SEXPR seed;
            auto kept = std::make_unique<LIB_SYMBOL>( "Kept" );
            kept->SetDescription( "original-B" );
            seed.SaveSymbol( libraryB, std::move( kept ) );

            SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &settings.Prj() );
            BOOST_REQUIRE( adapter );
            const auto previousTable = adapter->ProjectTable();
            PROJECT_SYMBOL_TABLE_RESTORE restore{
                Pgm().GetLibraryManager(),
                previousTable && *previousTable ? wxFileName( ( *previousTable )->Path() ).GetPath()
                                                : wxString()
            };
            loadManagerSymbolTable( *adapter, dir, libraryA, libraryB );
            SYMBOL_LIBRARY_MANAGER manager( settings.Prj() );
            LIB_SYMBOL* opened = manager.GetBufferedSymbol( "Kept", "B" );
            BOOST_REQUIRE( opened );
            manager.SetSymbolModified( "Kept", "B" );
            LIB_SYMBOL edited( *opened );
            edited.SetDescription( "local-B" );
            BOOST_REQUIRE( manager.UpdateSymbol( &edited, "B" ) );
            BOOST_REQUIRE( manager.IsLibraryModified( "B" ) );
            const int originalHash = manager.GetLibraryHash( "B" );
            BOOST_REQUIRE( manager.GetBufferedSymbol( "Current", "A" ) );
            replaceManagerLibraryContents( libraryB );
            const std::string external = readBytes( libraryB );
            const int externalHash = manager.GetLibraryHash( "B" );
            BOOST_CHECK_NE( externalHash, originalHash );

            for( int retry = 0; retry < 2; ++retry )
            {
                BOOST_CHECK_EQUAL( manager.GetLibraryHash( "B" ), externalHash );
                const auto refreshed = enumeratedDescriptions( manager, "B" );
                BOOST_CHECK_EQUAL( manager.GetLibraryHash( "B" ), externalHash );
                BOOST_CHECK_EQUAL( refreshed.size(), 2 );
                BOOST_CHECK( refreshed.contains( "Removed" ) );
                BOOST_CHECK( !refreshed.contains( "Added" ) );
                BOOST_REQUIRE( refreshed.contains( "Kept" ) );
                BOOST_CHECK_EQUAL( refreshed.at( "Kept" ), "local-B" );
                BOOST_CHECK( manager.GetBufferedSymbol( "Kept", "B" ) == opened );
                SYMBOL_BUFFER* buffer = manager.GetBuffer( "Kept", "B", true );
                BOOST_REQUIRE( buffer );
                BOOST_CHECK_EQUAL( buffer->GetOriginal().GetDescription(), "original-B" );
                BOOST_CHECK( manager.IsLibraryModified( "B" ) );

                {
                    wxLogNull silence;
                    BOOST_CHECK( !manager.SaveLibrary( "B", libraryB, SCH_IO_MGR::SCH_KICAD ) );
                }

                BOOST_CHECK_EQUAL( readBytes( libraryB ), external );
                BOOST_CHECK_EQUAL( opened->GetDescription(), "local-B" );
            }

            SCH_IO_KICAD_SEXPR reader;
            BOOST_REQUIRE( reader.LoadSymbol( libraryB, "Kept" ) );
            BOOST_CHECK_EQUAL( reader.LoadSymbol( libraryB, "Kept" )->GetDescription(), "external-B" );
            BOOST_REQUIRE( reader.LoadSymbol( libraryB, "Added" ) );
            BOOST_CHECK( !reader.LoadSymbol( libraryB, "Removed" ) );
        }
    }
}


BOOST_AUTO_TEST_CASE( ManagerDefaultReadsPreserveCurrentLibraryRevision )
{
    for( const wxString& ext : { wxString( "kemi_sym" ) } )
    {
        BOOST_TEST_CONTEXT( ext )
        {
            const wxString dir = directory.CreateChildDirStr( "manager-current-" + ext );
            const wxString libraryA = wxFileName( dir, "A." + ext ).GetFullPath();
            const wxString libraryB = wxFileName( dir, "B." + ext ).GetFullPath();
            createLibrary( libraryA, "Current" );
            createLibrary( libraryB, "Removed" );
            SCH_IO_KICAD_SEXPR seed;
            auto kept = std::make_unique<LIB_SYMBOL>( "Kept" );
            kept->SetDescription( "original-B" );
            seed.SaveSymbol( libraryB, std::move( kept ) );

            SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &settings.Prj() );
            BOOST_REQUIRE( adapter );
            const auto previousTable = adapter->ProjectTable();
            PROJECT_SYMBOL_TABLE_RESTORE restore{
                Pgm().GetLibraryManager(),
                previousTable && *previousTable ? wxFileName( ( *previousTable )->Path() ).GetPath()
                                                : wxString()
            };
            loadManagerSymbolTable( *adapter, dir, libraryA, libraryB );
            SYMBOL_LIBRARY_MANAGER manager( settings.Prj() );
            LIB_SYMBOL* opened = manager.GetBufferedSymbol( "Kept", "B" );
            BOOST_REQUIRE( opened );
            BOOST_CHECK( !manager.IsLibraryModified( "B" ) );
            replaceManagerLibraryContents( libraryB );
            const std::string external = readBytes( libraryB );

            // Ordinary reads may serve the current editor clone.  They must not pair its old
            // contents with a new disk revision before the user makes the next local edit.
            const auto currentTree = enumeratedDescriptions( manager, "B", false );
            BOOST_CHECK_EQUAL( currentTree.size(), 2 );
            BOOST_CHECK( currentTree.contains( "Removed" ) );
            BOOST_CHECK( !currentTree.contains( "Added" ) );
            BOOST_REQUIRE( currentTree.contains( "Kept" ) );
            BOOST_CHECK_EQUAL( currentTree.at( "Kept" ), "original-B" );
            LIB_SYMBOL* current = manager.GetBufferedSymbol( "Kept", "B" );
            BOOST_REQUIRE( current );
            BOOST_CHECK( current == opened );
            BOOST_CHECK_EQUAL( current->GetDescription(), "original-B" );
            SYMBOL_BUFFER* buffer = manager.GetBuffer( "Kept", "B" );
            BOOST_REQUIRE( buffer );
            BOOST_CHECK_EQUAL( buffer->GetSymbol().GetDescription(), "original-B" );
            BOOST_CHECK_EQUAL( buffer->GetOriginal().GetDescription(), "original-B" );
            BOOST_CHECK( !manager.IsLibraryModified( "B" ) );
            LIB_SYMBOL edited( *current );
            edited.SetDescription( "local-current-B" );
            BOOST_REQUIRE( manager.UpdateSymbol( &edited, "B" ) );

            {
                wxLogNull silence;
                BOOST_CHECK( !manager.SaveLibrary( "B", libraryB, SCH_IO_MGR::SCH_KICAD ) );
            }

            BOOST_CHECK( manager.IsLibraryModified( "B" ) );
            BOOST_CHECK_EQUAL( current->GetDescription(), "local-current-B" );
            BOOST_CHECK_EQUAL( buffer->GetOriginal().GetDescription(), "original-B" );
            BOOST_CHECK_EQUAL( readBytes( libraryB ), external );
        }
    }
}


BOOST_AUTO_TEST_CASE( BufferedSaveRejectsExternalEditsWithFreshPlugin )
{
    for( const wxString& ext : { wxString( "kemi_sym" ), wxString( "kicad_symdir" ) } )
    {
        BOOST_TEST_CONTEXT( ext )
        {
            const bool isDirectory = ext.EndsWith( "kicad_symdir" );
            const bool isMixed = ext == "mixed.kicad_symdir";
            const wxString library = directory.ChildPathStr( "buffered." + ext );

            if( isDirectory )
                BOOST_REQUIRE( wxFileName::Mkdir( library ) );

            const wxString first = isDirectory ? wxFileName( library, "A.kemi_sym" ).GetFullPath()
                                               : library;
            const wxString secondName = isMixed ? wxString( "B.kicad_sym" ) : wxString( "B.kemi_sym" );
            const wxString second = isDirectory ? wxFileName( library, secondName ).GetFullPath()
                                                : library;
            createLibrary( first, "A" );
            SCH_IO_KICAD_SEXPR seed;

            if( isDirectory )
                seed.CreateLibrary( second );

            auto original = std::make_unique<LIB_SYMBOL>( "B" );
            original->SetDescription( "original-B" );
            seed.SaveSymbol( second, std::move( original ) );

            LIB_BUFFER buffer( "Buffered" );
            buffer.Load( library );
            BOOST_REQUIRE( buffer.GetSymbol( "A" ) );
            BOOST_REQUIRE( buffer.GetSymbol( "B" ) );
            buffer.GetSymbol( "A" )->SetDescription( "local-A" );
            buffer.GetBuffer( "A" )->GetScreen()->SetContentModified();

            const auto time = std::filesystem::last_write_time( ToFsPath( second ) );
            std::string external = readBytes( second );
            const size_t offset = external.find( "original-B" );
            BOOST_REQUIRE( offset != std::string::npos );
            external.replace( offset, std::string( "original-B" ).size(), "external-B" );
            writeBytes( second, external );
            std::filesystem::last_write_time( ToFsPath( second ), time );
            const std::string beforeFirst = readBytes( first );

            for( int retry = 0; retry < 2; ++retry )
            {
                // The GUI creates a fresh writer for each save. Its new cache must not
                // turn the old edit buffers into a valid revision of the external file.
                SCH_IO_KICAD_SEXPR writer;
                wxLogNull silence;
                const wxString target = isDirectory ? library + wxFileName::GetPathSeparator() : library;
                BOOST_CHECK( !buffer.SaveLibrary( target, &writer, false ) );
            }

            BOOST_CHECK( buffer.IsModified() );
            BOOST_CHECK_EQUAL( buffer.GetSymbol( "A" )->GetDescription(), "local-A" );
            BOOST_CHECK( buffer.GetBuffer( "A" )->GetOriginal().GetDescription().IsEmpty() );
            BOOST_CHECK_EQUAL( readBytes( first ), beforeFirst );
            BOOST_CHECK_EQUAL( readBytes( second ), external );
            SCH_IO_KICAD_SEXPR reader;
            BOOST_REQUIRE( reader.LoadSymbol( library, "B" ) );
            BOOST_CHECK_EQUAL( reader.LoadSymbol( library, "B" )->GetDescription(), "external-B" );

            LIB_BUFFER reloaded( "Buffered" );
            reloaded.Load( library );
            reloaded.GetSymbol( "A" )->SetDescription( "reloaded edit" );
            SCH_IO_KICAD_SEXPR writer;

            if( isMixed )
            {
                // Reloading resolves a stale revision, but cannot lift the native output restriction.
                wxLogNull silence;
                BOOST_CHECK( !reloaded.SaveLibrary( library, &writer ) );
                BOOST_CHECK_EQUAL( readBytes( first ), beforeFirst );
                BOOST_CHECK_EQUAL( readBytes( second ), external );
                BOOST_CHECK( !wxFileExists( wxFileName( library, "B.kemi_sym" ).GetFullPath() ) );
                continue;
            }

            BOOST_REQUIRE( reloaded.SaveLibrary( library, &writer ) );
            SCH_IO_KICAD_SEXPR verified;
            BOOST_REQUIRE( verified.LoadSymbol( library, "B" ) );
            BOOST_CHECK_EQUAL( verified.LoadSymbol( library, "B" )->GetDescription(), "external-B" );
            BOOST_REQUIRE( verified.LoadSymbol( library, "A" ) );
            BOOST_CHECK_EQUAL( verified.LoadSymbol( library, "A" )->GetDescription(), "reloaded edit" );
        }
    }
}


BOOST_AUTO_TEST_CASE( BufferedSourceSaveUpdatesRevisionAfterSuccess )
{
    const wxString source = directory.ChildPathStr( "consecutive.kemi_sym" );
    createLibrary( source, "Part" );
    LIB_BUFFER buffer( "Consecutive" );
    buffer.Load( source );

    for( const wxString& description : { wxString( "first edit" ), wxString( "second edit" ) } )
    {
        buffer.GetSymbol( "Part" )->SetDescription( description );
        SCH_IO_KICAD_SEXPR writer;
        BOOST_REQUIRE( buffer.SaveLibrary( source, &writer ) );
        BOOST_CHECK_EQUAL( buffer.GetBuffer( "Part" )->GetOriginal().GetDescription(), description );
        SCH_IO_KICAD_SEXPR reader;
        BOOST_REQUIRE( reader.LoadSymbol( source, "Part" ) );
        BOOST_CHECK_EQUAL( reader.LoadSymbol( source, "Part" )->GetDescription(), description );
    }
}


BOOST_AUTO_TEST_CASE( BufferedSaveAsPreservesSourceRevisionAndOriginalNames )
{
    const wxString source = directory.ChildPathStr( "buffer-source.kemi_sym" );
    const wxString target = directory.ChildPathStr( "buffer-copy.kemi_sym" );
    createLibrary( source, "Original" );
    const std::string original = readBytes( source );
    LIB_BUFFER buffer( "SaveAs" );
    buffer.Load( source );
    std::shared_ptr<SYMBOL_BUFFER> symbol = buffer.GetBuffer( "Original" );
    BOOST_REQUIRE( symbol );
    symbol->GetSymbol().SetName( "Renamed" );
    SCH_IO_KICAD_SEXPR copyWriter;
    BOOST_REQUIRE( buffer.SaveLibrary( target, &copyWriter, false ) );
    BOOST_CHECK_EQUAL( readBytes( source ), original );
    BOOST_CHECK_EQUAL( symbol->GetOriginal().GetName(), "Original" );

    const std::string external = original + "\n";
    writeBytes( source, external );

    {
        SCH_IO_KICAD_SEXPR sourceWriter;
        wxLogNull silence;
        BOOST_CHECK( !buffer.SaveLibrary( source, &sourceWriter ) );
    }

    BOOST_CHECK_EQUAL( readBytes( source ), external );
    BOOST_CHECK_EQUAL( symbol->GetOriginal().GetName(), "Original" );

    // Restoring the loaded revision permits a retry; SaveAs did not consume the rename.
    writeBytes( source, original );
    SCH_IO_KICAD_SEXPR sourceWriter;
    BOOST_REQUIRE( buffer.SaveLibrary( source, &sourceWriter ) );
    SCH_IO_KICAD_SEXPR reader;
    BOOST_REQUIRE( reader.LoadSymbol( source, "Renamed" ) );
    BOOST_CHECK( !reader.LoadSymbol( source, "Original" ) );
}


BOOST_AUTO_TEST_CASE( BufferedLateExternalEditPreservesRenameAndDeletionForRetry )
{
    const wxString source = directory.ChildPathStr( "late-change.kemi_sym" );
    createLibrary( source, "Original" );
    SCH_IO_KICAD_SEXPR seed;
    seed.SaveSymbol( source, std::make_unique<LIB_SYMBOL>( "Deleted" ) );
    const std::string original = readBytes( source );
    LIB_BUFFER buffer( "LateChange" );
    buffer.Load( source );
    std::shared_ptr<SYMBOL_BUFFER> renamed = buffer.GetBuffer( "Original" );
    BOOST_REQUIRE( renamed );
    renamed->GetSymbol().SetName( "Renamed" );
    std::shared_ptr<SYMBOL_BUFFER> deleted = buffer.GetBuffer( "Deleted" );
    BOOST_REQUIRE( deleted );
    BOOST_REQUIRE( buffer.DeleteBuffer( *deleted ) );
    std::string external = original;
    const size_t newline = external.find( '\n' );
    BOOST_REQUIRE( newline != std::string::npos );
    external[newline] = ' ';
    const long long stamp = SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( source );

    {
        EXTERNAL_EDIT_DURING_SAVE writer( source, external );
        wxLogNull silence;
        BOOST_CHECK( !buffer.SaveLibrary( source, &writer ) );
    }

    BOOST_CHECK_EQUAL( readBytes( source ), external );
    BOOST_CHECK_EQUAL( SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( source ), stamp );
    BOOST_CHECK_EQUAL( renamed->GetOriginal().GetName(), "Original" );
    BOOST_CHECK_EQUAL( buffer.GetDeletedBuffers().size(), 1 );

    writeBytes( source, original );
    SCH_IO_KICAD_SEXPR writer;
    BOOST_REQUIRE( buffer.SaveLibrary( source, &writer ) );
    BOOST_CHECK( buffer.GetDeletedBuffers().empty() );
    SCH_IO_KICAD_SEXPR reader;
    BOOST_REQUIRE( reader.LoadSymbol( source, "Renamed" ) );
    BOOST_CHECK( !reader.LoadSymbol( source, "Original" ) );
    BOOST_CHECK( !reader.LoadSymbol( source, "Deleted" ) );
}


BOOST_AUTO_TEST_CASE( SymbolSaveAsKeepsSourceCacheIdentity )
{
    for( const wxString& ext : { wxString( "kemi_sym" ) } )
    {
        for( bool buffered : { false, true } )
        {
            const wxString source = directory.ChildPathStr(
                    wxString::Format( "source-%d.", buffered ) + ext );
            const wxString target = directory.ChildPathStr(
                    wxString::Format( "copy-%d.", buffered ) + ext );
            createLibrary( source, "Source" );
            const std::string original = readBytes( source );
            const auto time = std::filesystem::last_write_time( ToFsPath( source ) );
            std::filesystem::last_write_time( ToFsPath( source ), time - std::chrono::hours( 24 ) );
            SCH_IO_KICAD_SEXPR io;
            BOOST_REQUIRE( io.LoadSymbol( source, "Source" ) );

            if( buffered )
            {
                std::map<std::string, UTF8> properties = { { SCH_IO_KICAD_SEXPR::PropBuffering, "" } };
                io.SaveSymbol( source, std::make_unique<LIB_SYMBOL>( "Pending" ), &properties );
            }

            BOOST_CHECK_NO_THROW( io.SaveLibrary( target ) );
            BOOST_CHECK_EQUAL( readBytes( source ), original );
            BOOST_CHECK_NO_THROW( io.SaveLibrary( source ) );

            for( const wxString& path : { source, target } )
            {
                SCH_IO_KICAD_SEXPR reader;
                BOOST_REQUIRE( reader.LoadSymbol( path, "Source" ) );

                if( buffered )
                    BOOST_REQUIRE( reader.LoadSymbol( path, "Pending" ) );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( BufferedSavePreservesMultilevelInheritance )
{
    const wxString source = directory.ChildPathStr( "inheritance.kemi_sym" );
    const wxString target = directory.ChildPathStr( "inheritance-copy.kemi_sym" );
    createLibrary( source, "ZRoot" );
    SCH_IO_KICAD_SEXPR seed;
    LIB_SYMBOL* root = seed.LoadSymbol( source, "ZRoot" );
    BOOST_REQUIRE( root );
    auto child = std::make_unique<LIB_SYMBOL>( "YChild" );
    child->SetParent( root );
    seed.SaveSymbol( source, std::move( child ) );
    LIB_SYMBOL* parent = seed.LoadSymbol( source, "YChild" );
    BOOST_REQUIRE( parent );
    auto grandchild = std::make_unique<LIB_SYMBOL>( "XGrandchild" );
    grandchild->SetParent( parent );
    seed.SaveSymbol( source, std::move( grandchild ) );

    LIB_BUFFER buffer( "Inheritance" );
    buffer.Load( source );

    for( const wxString& path : { source, target } )
    {
        SCH_IO_KICAD_SEXPR writer;
        BOOST_REQUIRE( buffer.SaveLibrary( path, &writer ) );
        SCH_IO_KICAD_SEXPR reader;
        LIB_SYMBOL* loadedChild = reader.LoadSymbol( path, "YChild" );
        LIB_SYMBOL* loadedGrandchild = reader.LoadSymbol( path, "XGrandchild" );
        BOOST_REQUIRE( loadedChild );
        BOOST_REQUIRE( loadedGrandchild );
        BOOST_REQUIRE( loadedChild->IsDerived() );
        BOOST_REQUIRE( loadedGrandchild->IsDerived() );
        BOOST_CHECK_EQUAL( loadedChild->GetParent().lock()->GetName(), "ZRoot" );
        BOOST_CHECK_EQUAL( loadedGrandchild->GetParent().lock()->GetName(), "YChild" );
    }
}


BOOST_AUTO_TEST_CASE( SymbolSaveAsDoesNotConsumeSourcePendingDeletes )
{
    const wxString source = directory.CreateChildDirStr( "pending-source.kicad_symdir" );
    const wxString target = directory.CreateChildDirStr( "pending-copy.kicad_symdir" );
    const wxString first = wxFileName( source, "A.kemi_sym" ).GetFullPath();
    const wxString second = wxFileName( source, "B.kemi_sym" ).GetFullPath();
    createLibrary( first, "A" );
    createLibrary( second, "B" );
    const std::string firstBytes = readBytes( first );
    const std::string secondBytes = readBytes( second );
    SCH_IO_KICAD_SEXPR io;
    BOOST_REQUIRE( io.LoadSymbol( source, "A" ) );
    std::map<std::string, UTF8> properties = { { SCH_IO_KICAD_SEXPR::PropBuffering, "" } };
    io.DeleteSymbol( source, "B", &properties );
    BOOST_CHECK_NO_THROW( io.SaveLibrary( target ) );
    BOOST_CHECK_EQUAL( readBytes( first ), firstBytes );
    BOOST_CHECK_EQUAL( readBytes( second ), secondBytes );
    SCH_IO_KICAD_SEXPR copyReader;
    BOOST_REQUIRE( copyReader.LoadSymbol( target, "A" ) );
    BOOST_CHECK( !copyReader.LoadSymbol( target, "B" ) );

    BOOST_CHECK_NO_THROW( io.SaveLibrary( source ) );
    BOOST_CHECK( wxFileExists( first ) );
    BOOST_CHECK( !wxFileExists( second ) );
    SCH_IO_KICAD_SEXPR sourceReader;
    BOOST_REQUIRE( sourceReader.LoadSymbol( source, "A" ) );
    BOOST_CHECK( !sourceReader.LoadSymbol( source, "B" ) );
}


BOOST_AUTO_TEST_CASE( BufferedLoadFailureCannotBeSaved )
{
    const wxString source = directory.ChildPathStr( "buffer-rejected.kemi_sym" );
    const wxString target = directory.ChildPathStr( "buffer-rejected-copy.kicad_sym" );
    const std::string invalid = "(kemi_symbol_lib (format_version 2) (version 20231120))\n";
    writeBytes( source, invalid );
    LIB_BUFFER buffer( "Rejected" );
    BOOST_CHECK_THROW( buffer.Load( source ), IO_ERROR );

    for( const wxString& path : { source, target } )
    {
        SCH_IO_KICAD_SEXPR writer;
        wxLogNull silence;
        BOOST_CHECK( !buffer.SaveLibrary( path, &writer ) );
    }

    BOOST_CHECK_EQUAL( readBytes( source ), invalid );
    BOOST_CHECK( !wxFileExists( target ) );
}


BOOST_AUTO_TEST_CASE( BufferedSaveRejectsRemovedSourceDirectory )
{
    const wxString source = directory.CreateChildDirStr( "removed.kicad_symdir" );
    const wxString symbolFile = wxFileName( source, "A.kemi_sym" ).GetFullPath();
    createLibrary( symbolFile, "A" );
    LIB_BUFFER buffer( "Removed" );
    buffer.Load( source + wxFileName::GetPathSeparator() );
    BOOST_REQUIRE( wxRemoveFile( symbolFile ) );
    BOOST_REQUIRE( wxFileName::Rmdir( source ) );

    {
        SCH_IO_KICAD_SEXPR writer;
        wxLogNull silence;
        BOOST_CHECK( !buffer.SaveLibrary( source, &writer, false ) );
    }

    BOOST_CHECK( !wxFileExists( source ) );
    BOOST_CHECK( !wxFileName::DirExists( source ) );
}


#ifdef __WXMSW__
BOOST_AUTO_TEST_CASE( WindowsDirectoryConflictIgnoresCase )
{
    const wxString dir = directory.CreateChildDirStr( "case.kicad_symdir" );
    const wxString nativePath = wxFileName( dir, "Foo.kemi_sym" ).GetFullPath();
    const wxString legacyPath = wxFileName( dir, "foo.kicad_sym" ).GetFullPath();
    createLibrary( nativePath, "Native" );
    createLibrary( legacyPath, "Legacy" );
    SCH_IO_KICAD_SEXPR io;
    BOOST_CHECK_THROW( io.LoadSymbol( dir, "Native" ), IO_ERROR );
    BOOST_CHECK_THROW( io.LoadSymbol( dir, "Legacy" ), IO_ERROR );
    BOOST_CHECK_THROW( io.SaveLibrary( dir ), IO_ERROR );
}
#endif


BOOST_AUTO_TEST_CASE( NativeHistorySnapshotRestoresReadableSchematic )
{
    LIBGIT2_SCOPE libgit;
    COMMON_SETTINGS* common = Pgm().GetCommonSettings();
    BOOST_REQUIRE( common );
    SCOPED_SET_RESET<COMMON_SETTINGS::AUTO_BACKUP> restoreBackup( common->m_Backup );
    common->m_Backup.enabled = true;
    common->m_Backup.format = BACKUP_FORMAT::INCREMENTAL;
    common->m_Backup.location = BACKUP_LOCATION::PROJECT_DIR;
    common->m_Backup.limit_total_size = COMMON_SETTINGS::AUTO_BACKUP::UNLIMITED_SIZE;

    SCH_SHEET* sheet = schematic->GetTopLevelSheet( 0 );
    const wxString path = directory.ChildPathStr( "history.kemi_sch" );
    sheet->SetFileName( path );
    sheet->GetScreen()->SetFileName( path );
    sheet->GetScreen()->Append( new SCH_TEXT( VECTOR2I( 1000, 2000 ), "history payload" ) );
    const wxString projectPath = settings.Prj().GetProjectPath();
    LOCAL_HISTORY history;
    history.RegisterSaver( schematic.get(),
            [&]( const wxString& aProjectPath, std::vector<HISTORY_FILE_DATA>& aData )
            {
                schematic->SaveToHistory( aProjectPath, aData );
            } );
    BOOST_REQUIRE( history.RunRegisteredSaversAndCommit( projectPath, "Format regression", "sch" ) );
    history.WaitForPendingSave();
    const wxString commit = history.GetHeadHash( projectPath );
    BOOST_REQUIRE( !commit.IsEmpty() );
    const wxString restoreDir = directory.CreateChildDirStr( "restored" );
    BOOST_REQUIRE( history.ExtractAllFilesAtCommit( projectPath, commit, restoreDir ) );
    const wxString restored = wxFileName( restoreDir, "history.kemi_sch" ).GetFullPath();
    BOOST_CHECK( readBytes( restored ).find( "(kemi_sch" ) == 0 );
    history.UnregisterSaver( schematic.get() );
    SCH_IO_KICAD_SEXPR reader;
    SCH_SHEET* loaded = loadSchematic( reader, restored );
    BOOST_CHECK( reader.GetError().IsEmpty() );
    size_t count = 0;

    for( SCH_ITEM* item : loaded->GetScreen()->Items().OfType( SCH_TEXT_T ) )
    {
        BOOST_CHECK_EQUAL( static_cast<SCH_TEXT*>( item )->GetText(), "history payload" );
        ++count;
    }

    BOOST_CHECK_EQUAL( count, 1 );
}

BOOST_AUTO_TEST_SUITE_END()
