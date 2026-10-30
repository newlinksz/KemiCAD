/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <drawing_sheet/ds_data_model.h>
#include <design_block.h>
#include <design_block_io.h>
#include <embedded_files.h>
#include <json_common.h>
#include <kemi_format_profile.h>
#include <kemi_project_save_as.h>
#include <gestfich.h>
#include <ki_exception.h>
#include <project/project_file_downgrade.h>
#include <project_tree_traverser.h>

#include <memory>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>

namespace
{
using KEMI_FORMAT::DOC;
using KEMI_FORMAT::FLAVOR;

struct PERSISTENT_OUTPUT_FIXTURE
{
    KI_TEST::SCOPED_TEMP_DIR directory{ wxS( "kemi-persistent-output" ) };
};
}

BOOST_FIXTURE_TEST_SUITE( KemiPersistentOutput, PERSISTENT_OUTPUT_FIXTURE )

BOOST_AUTO_TEST_CASE( SubsequentSaveResolvesAllProfilesAndRejectsOfficialTargets )
{
    DS_DATA_MODEL model;
    model.SetDefaultLayout();

    for( const KEMI_FORMAT::DOC_PROFILE& profile : KEMI_FORMAT::AllProfiles() )
    {
        const wxString source = directory.ChildPathStr( wxS( "source." ) + wxString::FromUTF8( profile.kemiExt ) );
        BOOST_CHECK( KEMI_FORMAT::IsRestrictedOutput( source ) );
        BOOST_CHECK( KEMI_FORMAT::IsRestrictedOutput( wxS( "untitled" ), wxS( "project.kemi_pro" ) ) );
        const wxString target = directory.ChildPathStr( wxS( "saved." ) + wxString::FromUTF8( profile.kemiExt ) );
        BOOST_CHECK_EQUAL( KEMI_FORMAT::PersistentOutputPath( profile.doc,
                directory.ChildPathStr( wxS( "saved" ) ), true ), target );

        for( const wxString& suffix : { wxString( profile.kicadExt ),
                                       wxString( profile.kicadExt ).Upper() + wxS( ".tmp" ) } )
        {
            const wxString forbidden = directory.ChildPathStr( wxS( "blocked." ) + suffix );
            BOOST_CHECK_THROW( KEMI_FORMAT::PersistentOutputPath( profile.doc, forbidden, true ), IO_ERROR );
            BOOST_CHECK_THROW( KEMI_FORMAT::CheckPersistentOutput( profile.doc, forbidden,
                                                                  FLAVOR::KEMI, true ), IO_ERROR );
            BOOST_CHECK( !wxFileExists( forbidden ) );
            BOOST_CHECK_THROW( KEMI_FORMAT::PersistentOutputPath( profile.doc, forbidden, false ), IO_ERROR );
            BOOST_CHECK_THROW( KEMI_FORMAT::CheckPersistentOutput( profile.doc, forbidden,
                                                                  FLAVOR::LEGACY, false ), IO_ERROR );
        }

        BOOST_CHECK_THROW( KEMI_FORMAT::CheckPersistentOutput( profile.doc, target,
                                                              FLAVOR::LEGACY, true ), IO_ERROR );
        BOOST_CHECK_THROW( KEMI_FORMAT::CheckPersistentOutput( profile.doc, wxEmptyString,
                                                              FLAVOR::LEGACY, true ), IO_ERROR );
        BOOST_CHECK( !wxFileExists( target ) );
    }

    const wxString final = KEMI_FORMAT::PersistentOutputPath( DOC::DRAWING_SHEET,
            directory.ChildPathStr( wxS( "copy" ) ), true );
    const wxString staging = directory.ChildPathStr( wxS( "copy.tmp" ) );
    KEMI_FORMAT::CheckPersistentOutput( DOC::DRAWING_SHEET, staging, FLAVOR::KEMI, true );
    model.Save( staging, FLAVOR::KEMI );
    BOOST_CHECK( KI_TEST::LoadStringData( staging ).find( "(kemi_wks" ) == 0 );
    BOOST_REQUIRE( wxRenameFile( staging, final ) );
    BOOST_CHECK( !wxFileExists( directory.ChildPathStr( wxS( "copy.kicad_wks" ) ) ) );
}


BOOST_AUTO_TEST_CASE( FirstSaveResolvesOfficialSourcesWithoutMutatingOrOverwritingThem )
{
    for( const KEMI_FORMAT::DOC_PROFILE& profile : KEMI_FORMAT::AllProfiles() )
    {
        const wxString source = directory.ChildPathStr( wxS( "first." ) + wxString( profile.kicadExt ) );
        const wxString native = directory.ChildPathStr( wxS( "first." ) + wxString( profile.kemiExt ) );
        const std::string sourceBytes = "read-only official source\r\n";
        {
            wxFFile file( source, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE_EQUAL( file.Write( sourceBytes.data(), sourceBytes.size() ), sourceBytes.size() );
        }

        BOOST_CHECK( KEMI_FORMAT::IsRestrictedOutput( source ) );
        BOOST_CHECK_EQUAL( KEMI_FORMAT::ResolveNativeSaveTarget( profile.doc, source ), native );
        BOOST_CHECK( !wxFileExists( native ) );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), sourceBytes );

        {
            wxFFile file( native, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write( wxS( "existing native sentinel" ) ) );
        }

        BOOST_CHECK_THROW( KEMI_FORMAT::ResolveNativeSaveTarget( profile.doc, source ), IO_ERROR );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( native ), "existing native sentinel" );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), sourceBytes );
        BOOST_CHECK_EQUAL( KEMI_FORMAT::ResolveNativeSaveTarget( profile.doc, native ), native );
        BOOST_CHECK_THROW( KEMI_FORMAT::CheckPersistentOutput( profile.doc, native,
                                                              FLAVOR::LEGACY, false ), IO_ERROR );
        BOOST_CHECK_NO_THROW( KEMI_FORMAT::CheckPersistentOutput( profile.doc, wxEmptyString,
                                                                 FLAVOR::LEGACY, false ) );
    }

    BOOST_CHECK( !KEMI_FORMAT::IsRestrictedOutput( wxS( "clipboard" ) ) );
}


BOOST_AUTO_TEST_CASE( NativeDesignBlockMembersCopyWithoutChangingBytes )
{
    const wxString library = directory.CreateChildDirStr( wxS( "blocks.kicad_blocks" ) );
    DESIGN_BLOCK block;
    block.SetLibId( LIB_ID( wxS( "Blocks" ), wxS( "Native" ) ) );
    DESIGN_BLOCK_IO io;

    for( DOC doc : { DOC::SCHEMATIC, DOC::PCB } )
    {
        const wxString source = directory.ChildPathStr( wxS( "source." ) + KEMI_FORMAT::Ext( doc, FLAVOR::KEMI ) );
        const std::string bytes = "(" + KEMI_FORMAT::RootToken( doc, FLAVOR::KEMI ) + " (format_version 1))\n";
        wxFFile file( source, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE_EQUAL( file.Write( bytes.data(), bytes.size() ), bytes.size() );
        BOOST_REQUIRE( file.Close() );

        if( doc == DOC::SCHEMATIC )
            block.SetSchematicFile( source );
        else
            block.SetBoardFile( source );
    }

    io.DesignBlockSave( library, &block );
    std::unique_ptr<DESIGN_BLOCK> reopened( io.DesignBlockLoad( library, wxS( "Native" ) ) );
    BOOST_REQUIRE( reopened );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( block.GetSchematicFile() ),
                       KI_TEST::LoadStringData( reopened->GetSchematicFile() ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( block.GetBoardFile() ),
                       KI_TEST::LoadStringData( reopened->GetBoardFile() ) );
}


BOOST_AUTO_TEST_CASE( EmbeddedCachePreservesNativeNamesAndRejectsOfficialExtraction )
{
    EMBEDDED_FILES files;
    files.SetKemiOnlyEmbeddedFiles();
    auto native = new EMBEDDED_FILES::EMBEDDED_FILE();
    native->name = wxS( "drawing.kemi_wks" );
    const std::string contents = "(kemi_wks (format_version 1))";
    native->decompressedData.assign( contents.begin(), contents.end() );
    BOOST_REQUIRE( EMBEDDED_FILES::CompressAndEncode( *native ) == EMBEDDED_FILES::RETURN_CODE::OK );
    files.AddFile( native );
    auto official = new EMBEDDED_FILES::EMBEDDED_FILE( *native );
    official->name = wxS( "drawing.kicad_wks" );
    files.AddFile( official );

    const wxFileName cached = files.WriteCacheFile( native->name, directory.PathStr() );
    BOOST_REQUIRE( cached.FileExists() );
    BOOST_CHECK_EQUAL( cached.GetExt(), wxS( "kemi_wks" ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( cached.GetFullPath() ), contents );

    // An interrupted cache from an older version must be regenerated on retry.
    {
        wxFFile truncated( cached.GetFullPath(), wxS( "wb" ) );
        BOOST_REQUIRE( truncated.IsOpened() );
        BOOST_REQUIRE_EQUAL( truncated.Write( "bad", 3 ), 3 );
    }

    BOOST_CHECK_EQUAL( files.WriteCacheFile( native->name, directory.PathStr() ).GetFullPath(),
                       cached.GetFullPath() );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( cached.GetFullPath() ), contents );
    BOOST_CHECK( !files.WriteCacheFile( official->name, directory.PathStr() ).IsOk() );

    // Copying an embedded collection for undo, extraction or an editor keeps its restriction.
    EMBEDDED_FILES copied( files );
    BOOST_CHECK( copied.IsKemiOnlyEmbeddedFiles() );
    BOOST_CHECK( !copied.WriteCacheFile( official->name, directory.PathStr() ).IsOk() );

    wxArrayString outputs;
    wxDir::GetAllFiles( directory.PathStr(), &outputs );
    BOOST_REQUIRE_EQUAL( outputs.size(), 1 );
    BOOST_CHECK_EQUAL( outputs.front(), cached.GetFullPath() );
}


BOOST_AUTO_TEST_CASE( ProjectDowngradeRejectsNativeMarkerBeforeMutation )
{
    nlohmann::json document = { { "meta", { { "version", 4 } } }, { "untouched", true } };
    KEMI_FORMAT::StampJsonMarker( document, DOC::PROJECT );
    const auto original = document;

    for( const DOWNGRADE_TARGET& target : GetDowngradeTargets() )
    {
        COMPATIBILITY_REPORT report;
        DowngradeProjectFileJson( document, target, report );
        BOOST_CHECK( report.IsBlocked() );
        BOOST_CHECK( document == original );
    }
}


BOOST_AUTO_TEST_CASE( ExportPreflightRejectsEveryRenamedNativeProfile )
{
    for( const KEMI_FORMAT::DOC_PROFILE& profile : KEMI_FORMAT::AllProfiles() )
    {
        const wxString source = directory.ChildPathStr(
                wxS( "disguised." ) + wxString::FromUTF8( profile.kicadExt ) );
        std::string contents;

        if( profile.jsonType )
        {
            nlohmann::json document = nlohmann::json::object();
            KEMI_FORMAT::StampJsonMarker( document, profile.doc );
            contents = document.dump();
        }
        else
        {
            contents = "# leading comment\n(" + std::string( profile.kemiRoot )
                       + " (format_version 1))\n";
        }

        {
            wxFFile file( source, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE_EQUAL( file.Write( contents.data(), contents.size() ), contents.size() );
        }

        BOOST_CHECK_THROW( KEMI_FORMAT::CheckLegacyExportSource( source ), IO_ERROR );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), contents );
    }
}

BOOST_AUTO_TEST_CASE( ProjectCopyPlansRestrictedJsonBeforeWritingAnySiblings )
{
    const wxString sourceDir = directory.CreateChildDirStr( wxS( "source" ) );
    const wxString outputDir = directory.CreateChildDirStr( wxS( "output" ) );
    const wxString source = wxFileName( sourceDir, wxS( "source.kicad_pro" ) ).GetFullPath();
    const wxString local = wxFileName( sourceDir, wxS( "source.kicad_prl" ) ).GetFullPath();
    const wxString child = wxFileName( sourceDir, wxS( "child.KICAD_SCH" ) ).GetFullPath();
    auto write = []( const wxString& aPath, const wxString& aText )
    {
        wxFFile file( aPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aText, wxConvUTF8 ) );
        BOOST_REQUIRE( file.Close() );
    };
    write( source, wxS( "{\"kemicad\":{\"document_type\":\"project\",\"format_version\":1}}" ) );
    write( local, wxS( "{}" ) );
    write( child, wxS( "(kicad_sch)" ) );

    // A loaded marker, without a sibling .kemi_pro, restricts the entire copy.
    // Queue a valid sibling and directory before discovering the incompatible child.
    PROJECT_TREE_TRAVERSER blocked( nullptr, sourceDir, wxS( "source" ), outputDir,
                                    wxS( "copy" ), true );
    blocked.OnFile( local );
    blocked.OnDir( wxFileName( sourceDir, wxS( "nested" ) ).GetFullPath() );
    blocked.OnFile( source );
    blocked.OnFile( child );
    BOOST_CHECK( !blocked.GetErrors().empty() );
    BOOST_CHECK( !blocked.CopyFiles() );
    wxArrayString files;
    wxDir::GetAllFiles( outputDir, &files );
    BOOST_CHECK( files.empty() );
    BOOST_CHECK( !wxDirExists( wxFileName( outputDir, wxS( "nested" ) ).GetFullPath() ) );

    PROJECT_TREE_TRAVERSER copy( nullptr, sourceDir, wxS( "source" ), outputDir,
                                 wxS( "copy" ), true );
    const wxString nativeChild = wxFileName( sourceDir, wxS( "child.kemi_sch" ) ).GetFullPath();
    write( nativeChild, wxS( "(kemi_sch (format_version 1))" ) );
    copy.OnFile( local );
    copy.OnFile( source );
    copy.OnFile( child );
    copy.OnFile( nativeChild );
    BOOST_CHECK( !copy.GetNewProjectFile().FileExists() );
    BOOST_REQUIRE( copy.CopyFiles() );
    BOOST_CHECK_EQUAL( copy.GetNewProjectFile().GetExt(), wxS( "kemi_pro" ) );
    const wxString nativeLocal = wxFileName( outputDir, wxS( "copy.kemi_prl" ) ).GetFullPath();
    for( const wxString& file : { copy.GetNewProjectFile().GetFullPath(), nativeLocal } )
    {
        const auto json = nlohmann::json::parse( KI_TEST::LoadStringData( file ) );
        BOOST_CHECK( json.contains( KEMI_FORMAT::JSON_MARKER_KEY ) );
    }
    BOOST_CHECK( !wxFileExists( wxFileName( outputDir, wxS( "copy.kicad_pro" ) ).GetFullPath() ) );
    BOOST_CHECK( !wxFileExists( wxFileName( outputDir, wxS( "copy.kicad_prl" ) ).GetFullPath() ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ),
            "{\"kemicad\":{\"document_type\":\"project\",\"format_version\":1}}" );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( local ), "{}" );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData(
            wxFileName( outputDir, wxS( "child.kemi_sch" ) ).GetFullPath() ),
            KI_TEST::LoadStringData( nativeChild ) );
    BOOST_CHECK( !wxFileExists( wxFileName( outputDir, wxS( "child.KICAD_SCH" ) ).GetFullPath() ) );
}


BOOST_AUTO_TEST_CASE( ProjectCopyRejectsTargetCollisionsBeforeCopying )
{
    const wxString sourceDir = directory.CreateChildDirStr( wxS( "source" ) );
    const wxString outputDir = directory.CreateChildDirStr( wxS( "output" ) );
    PROJECT_TREE_TRAVERSER copy( nullptr, sourceDir, wxS( "source" ), outputDir, wxS( "copy" ) );
    // Renaming source.txt would overwrite the unrelated copy.txt.
    copy.OnFile( wxFileName( sourceDir, wxS( "source.txt" ) ).GetFullPath() );
    copy.OnFile( wxFileName( sourceDir, wxS( "copy.txt" ) ).GetFullPath() );
    BOOST_CHECK( !copy.GetErrors().empty() );
    BOOST_CHECK( !copy.CopyFiles() );
    wxArrayString files;
    wxDir::GetAllFiles( outputDir, &files );
    BOOST_CHECK( files.empty() );
}


BOOST_AUTO_TEST_CASE( CopySexprFileSupportsNamedSecondValueWithoutChangingExistingCallback )
{
    const wxString source = directory.ChildPathStr( wxS( "pair-source.kemi_sch" ) );
    const wxString oldCopy = directory.ChildPathStr( wxS( "first-value.kemi_sch" ) );
    const wxString newCopy = directory.ChildPathStr( wxS( "second-value.kemi_sch" ) );
    {
        wxFFile file( source, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(design(project source)"
                "(property \"Footprint\" \"source:Pad\")(property \"Value\" \"source:keep\"))" ),
                                  wxConvUTF8 ) );
    }
    auto firstValue = []( const std::string& aToken, wxString& aValue )
    {
        if( aToken == "project" && aValue == wxS( "source" ) )
        {
            aValue = wxS( "copy" );
            return true;
        }

        return false;
    };
    wxString errors;
    CopySexprFile( source, oldCopy, firstValue, errors );
    CopySexprFile( source, newCopy, firstValue, errors,
            []( const std::string& aToken, const wxString& aName, wxString& aValue )
            {
                if( aToken == "property" && aName == wxS( "Footprint" ) )
                {
                    aValue = wxS( "copy:Pad" );
                    return true;
                }

                return false;
            } );
    BOOST_CHECK_MESSAGE( errors.empty(), errors );
    const std::string first = KI_TEST::LoadStringData( oldCopy );
    const std::string second = KI_TEST::LoadStringData( newCopy );
    BOOST_CHECK( first.find( "source:Pad" ) != std::string::npos );
    BOOST_CHECK( second.find( "copy:Pad" ) != std::string::npos );
    BOOST_CHECK( second.find( "source:Pad" ) == std::string::npos );
    BOOST_CHECK( second.find( "source:keep" ) != std::string::npos );
    BOOST_CHECK( KI_TEST::LoadStringData( source ).find( "source:Pad" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( ManagedProjectCacheRenameRequiresNativeMarkedLocalLibrary )
{
    const wxString sourceDir = directory.CreateChildDirStr( wxS( "cache-identity" ) );
    const wxString project = wxFileName( sourceDir, wxS( "source.kemi_pro" ) ).GetFullPath();
    const wxString library = wxFileName( sourceDir, wxS( "source.kemi_sym" ) ).GetFullPath();
    const wxString tablePath = wxFileName( sourceDir, wxS( "sym-lib-table" ) ).GetFullPath();
    auto write = []( const wxString& aPath, const wxString& aText )
    {
        wxFFile file( aPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aText, wxConvUTF8 ) );
        BOOST_REQUIRE( file.Close() );
    };
    write( project, wxS( "{}" ) );
    write( library, wxS( "native cache bytes" ) );

    for( const wxString& options : { wxString(), wxString( "kicad_import_cache=1" ),
                                     wxString( "kicad_import_cache=10" ) } )
    {
        for( bool local : { true, false } )
        {
            write( tablePath, wxString( wxS( "(sym_lib_table(version 7)(lib (name source)(type KiCad)(uri \"" ) )
                    + ( local ? wxS( "${KIPRJMOD}/source.kemi_sym" )
                              : wxS( "${KIPRJMOD}/vendor/source.kemi_sym" ) )
                    + wxS( "\")(options \"" ) + options + wxS( "\")(descr \"\")))" ) );
            LIBRARY_TABLE parsed( wxFileName( tablePath ), LIBRARY_TABLE_SCOPE::PROJECT );
            BOOST_REQUIRE_MESSAGE( parsed.IsOk(), parsed.ErrorDescription() );
            BOOST_CHECK_EQUAL( KEMI_FORMAT::IsManagedProjectImportCache(
                    sourceDir, wxS( "source" ), DOC::SYMBOL_LIB ),
                    local && options == wxS( "kicad_import_cache=1" ) );
        }
    }

    // A managed official library must retain the official Save As behavior.
    write( tablePath, wxS( "(sym_lib_table(version 7)(lib (name source)(type KiCad)"
            "(uri ${KIPRJMOD}/source.kicad_sym)(options kicad_import_cache=1)(descr \"\")))" ) );
    BOOST_CHECK( !KEMI_FORMAT::IsManagedProjectImportCache(
            sourceDir, wxS( "source" ), DOC::SYMBOL_LIB ) );
}



BOOST_AUTO_TEST_CASE( ProjectCopyRenamesOnlyTheRootManagedFootprintDirectory )
{
    const wxString sourceDir = directory.CreateChildDirStr( wxS( "managed-directories" ) );
    const wxString outputDir = directory.CreateChildDirStr( wxS( "managed-directories-copy" ) );
    const wxString pretty = wxFileName( sourceDir, wxS( "source.pretty" ) ).GetFullPath();
    const wxString vendor = wxFileName( sourceDir, wxS( "vendor" ) ).GetFullPath();
    const wxString vendorPretty = wxFileName( vendor, wxS( "source.pretty" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( pretty, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    BOOST_REQUIRE( wxFileName::Mkdir( vendorPretty, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    auto write = []( const wxString& aPath, const wxString& aText )
    {
        wxFFile file( aPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aText, wxConvUTF8 ) );
        BOOST_REQUIRE( file.Close() );
    };
    write( wxFileName( sourceDir, wxS( "source.kemi_pro" ) ).GetFullPath(), wxS( "{}" ) );
    write( wxFileName( sourceDir, wxS( "fp-lib-table" ) ).GetFullPath(),
           wxS( "(fp_lib_table(version 7)(lib (name source)(type KiCad)"
                "(uri ${KIPRJMOD}/source.pretty)(options kicad_import_cache=1)(descr \"\")))" ) );
    const wxString localFootprint = wxFileName( pretty, wxS( "Pad.kemi_mod" ) ).GetFullPath();
    const wxString vendorFootprint = wxFileName( vendorPretty, wxS( "Pad.kemi_mod" ) ).GetFullPath();
    write( localFootprint, wxS( "local footprint" ) );
    write( vendorFootprint, wxS( "third-party footprint" ) );
    PROJECT_TREE_TRAVERSER copy( nullptr, sourceDir, wxS( "source" ), outputDir, wxS( "copy" ), true );
    copy.OnDir( pretty );
    copy.OnDir( vendor );
    copy.OnDir( vendorPretty );
    copy.OnFile( localFootprint );
    copy.OnFile( vendorFootprint );
    BOOST_REQUIRE_MESSAGE( copy.CopyFiles(), copy.GetErrors() );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( wxFileName(
            outputDir + wxS( "/copy.pretty/Pad.kemi_mod" ) ).GetFullPath() ), "local footprint" );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( wxFileName(
            outputDir + wxS( "/vendor/source.pretty/Pad.kemi_mod" ) ).GetFullPath() ),
            "third-party footprint" );
    BOOST_CHECK( !wxDirExists( wxFileName( outputDir + wxS( "/vendor/copy.pretty" ) ).GetFullPath() ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( vendorFootprint ), "third-party footprint" );
}



BOOST_AUTO_TEST_CASE( ProjectCopyRejectsManagedCacheNicknameConflictsBeforeWriting )
{
    for( bool symbol : { true, false } )
    {
        const wxString sourceDir = directory.CreateChildDirStr( symbol ? wxS( "sym-conflict" )
                                                                     : wxS( "fp-conflict" ) );
        const wxString outputDir = directory.CreateChildDirStr( symbol ? wxS( "sym-conflict-copy" )
                                                                     : wxS( "fp-conflict-copy" ) );
        auto write = []( const wxString& aPath, const wxString& aText )
        {
            wxFFile file( aPath, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write( aText, wxConvUTF8 ) );
            BOOST_REQUIRE( file.Close() );
        };
        const wxString project = wxFileName( sourceDir, wxS( "source.kemi_pro" ) ).GetFullPath();
        const wxString sch = wxFileName( sourceDir, wxS( "source.kemi_sch" ) ).GetFullPath();
        const wxString pcb = wxFileName( sourceDir, wxS( "source.kemi_pcb" ) ).GetFullPath();
        write( project, wxS( "{}" ) );
        write( sch, wxS( "(kemi_sch (format_version 1))" ) );
        write( pcb, wxS( "(kemi_pcb (format_version 1))" ) );
        const wxString extension = symbol ? wxS( "kemi_sym" ) : wxS( "pretty" );
        const wxString library = wxFileName( sourceDir, wxS( "source" ), extension ).GetFullPath();
        wxString member = library;
        if( !symbol )
        {
            BOOST_REQUIRE( wxFileName::Mkdir( library ) );
            member = wxFileName( library, wxS( "Pad.kemi_mod" ) ).GetFullPath();
        }
        write( member, wxS( "source cache bytes" ) );
        const wxString tableName = symbol ? wxS( "sym-lib-table" ) : wxS( "fp-lib-table" );
        const wxString tablePath = wxFileName( sourceDir, tableName ).GetFullPath();
        write( tablePath, wxString( wxS( "(" ) ) + ( symbol ? wxS( "sym_lib_table" ) : wxS( "fp_lib_table" ) )
                + wxS( "(version 7)(lib (name source)(type KiCad)(uri ${KIPRJMOD}/source." )
                + extension + wxS( ")(options kicad_import_cache=1)(descr \"\"))"
                "(lib (name copy)(type KiCad)(uri ${KIPRJMOD}/vendor." )
                + extension + wxS( ")(options \"\")(descr \"third party\")))" ) );
        LIBRARY_TABLE parsed( wxFileName( tablePath ), LIBRARY_TABLE_SCOPE::PROJECT );
        BOOST_REQUIRE_MESSAGE( parsed.IsOk(), parsed.ErrorDescription() );
        const wxString tableBefore = wxString::FromUTF8( KI_TEST::LoadStringData( tablePath ) );
        const wxString sentinel = wxFileName( outputDir, wxS( "sentinel.txt" ) ).GetFullPath();
        write( sentinel, wxS( "must remain" ) );

        PROJECT_TREE_TRAVERSER copy( nullptr, sourceDir, wxS( "source" ), outputDir,
                                     wxS( "copy" ), true );
        if( !symbol )
            copy.OnDir( library );
        for( const wxString& file : { sch, pcb, member, tablePath } )
            copy.OnFile( file );
        BOOST_CHECK( !copy.GetErrors().empty() );
        BOOST_CHECK( !copy.CopyFiles() );
        wxArrayString files;
        wxDir::GetAllFiles( outputDir, &files );
        BOOST_REQUIRE_EQUAL( files.size(), 1u );
        BOOST_CHECK_EQUAL( files.front(), sentinel );
        wxDir output( outputDir );
        BOOST_CHECK( !output.HasSubDirs() );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( sentinel ), "must remain" );
        BOOST_CHECK_EQUAL( wxString::FromUTF8( KI_TEST::LoadStringData( tablePath ) ), tableBefore );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( member ), "source cache bytes" );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( sch ), "(kemi_sch (format_version 1))" );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( pcb ), "(kemi_pcb (format_version 1))" );
    }
}


BOOST_AUTO_TEST_CASE( ProjectCopyRenamesPrimaryBoardReferenceAndPreservesExtraBoards )
{
    for( bool native : { true, false } )
    {
        const wxString sourceDir = directory.CreateChildDirStr(
                native ? wxS( "native-board-source" ) : wxS( "official-board-source" ) );
        const wxString outputDir = directory.CreateChildDirStr(
                native ? wxS( "native-board-copy" ) : wxS( "official-board-copy" ) );
        const wxString projectExt = native ? wxS( "kemi_pro" ) : wxS( "kicad_pro" );
        const wxString boardExt = native ? wxS( "kemi_pcb" ) : wxS( "kicad_pcb" );
        const wxString source = wxFileName( sourceDir, wxS( "source" ), projectExt ).GetFullPath();
        const wxString primaryName = wxS( "source." ) + boardExt;
        const wxString extraName = wxS( "auxiliary." ) + boardExt;
        const wxString dottedName = wxS( "source.aux." ) + boardExt;
        const wxString nestedDir = wxFileName( sourceDir, wxS( "nested" ) ).GetFullPath();
        const wxString outputNested = wxFileName( outputDir, wxS( "nested" ) ).GetFullPath();
        const wxString externalDir = directory.CreateChildDirStr(
                native ? wxS( "native-board-external" ) : wxS( "official-board-external" ) );
        const wxString siblingDir = sourceDir + wxS( "-sibling" );
        BOOST_REQUIRE( wxMkdir( nestedDir ) );
        BOOST_REQUIRE( wxMkdir( siblingDir ) );

        const wxString primary = wxFileName( sourceDir, primaryName ).GetFullPath();
        const wxString extra = wxFileName( sourceDir, extraName ).GetFullPath();
        const wxString dotted = wxFileName( sourceDir, dottedName ).GetFullPath();
        const wxString nested = wxFileName( nestedDir, primaryName ).GetFullPath();
        const wxString external = wxFileName( externalDir, primaryName ).GetFullPath();
        const wxString sibling = wxFileName( siblingDir, primaryName ).GetFullPath();
        wxFileName relativeExternal( external );
        BOOST_REQUIRE( relativeExternal.MakeRelativeTo( sourceDir ) );
        const wxString outputPrimary = wxFileName( outputDir, wxS( "copy" ), boardExt ).GetFullPath();
        const wxString outputExtra = wxFileName( outputDir, extraName ).GetFullPath();
        const wxString nestedName = wxFileName( wxS( "nested" ), primaryName ).GetFullPath();
        const wxString nestedCopyName = wxFileName( wxS( "nested" ), wxS( "copy" ), boardExt ).GetFullPath();
        const std::vector<wxString> references = {
            primaryName, extraName, dottedName, nestedName, wxS( "./" ) + primaryName,
            primary, extra, external, sibling, relativeExternal.GetFullPath()
        };
        const std::vector<wxString> copiedReferences = {
            wxS( "copy." ) + boardExt, extraName, dottedName, nestedCopyName,
            wxS( "copy." ) + boardExt, outputPrimary, outputExtra, external, sibling,
            relativeExternal.GetFullPath()
        };
        nlohmann::json expected = nlohmann::json::array();
        nlohmann::json document = {
            { "meta", { { "version", 4 } } },
            { "boards", nlohmann::json::array() }
        };

        for( size_t index = 0; index < references.size(); ++index )
        {
            const std::string uuid = "00000000-0000-4000-8000-" + std::to_string( 100000000000 + index );
            document["boards"].push_back( nlohmann::json::array( { uuid, references[index].ToUTF8() } ) );
            expected.push_back( nlohmann::json::array( { uuid, copiedReferences[index].ToUTF8() } ) );
        }

        if( native )
            KEMI_FORMAT::StampJsonMarker( document, DOC::PROJECT );

        auto write = []( const wxString& aPath, const std::string& aContents )
        {
            wxFFile file( aPath, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE_EQUAL( file.Write( aContents.data(), aContents.size() ), aContents.size() );
            BOOST_REQUIRE( file.Close() );
        };
        const std::string sourceBytes = document.dump( 2 );
        write( source, sourceBytes );
        for( const wxString& board : { primary, extra, dotted, nested, external, sibling } )
            write( board, "unchanged board bytes" );

        PROJECT_TREE_TRAVERSER copy( nullptr, sourceDir, wxS( "source" ), outputDir,
                                     wxS( "copy" ) );
        copy.OnDir( nestedDir );
        for( const wxString& file : { source, primary, extra, dotted, nested } )
            copy.OnFile( file );

        BOOST_REQUIRE_MESSAGE( copy.CopyFiles(), copy.GetErrors() );
        const nlohmann::json saved = nlohmann::json::parse(
                KI_TEST::LoadStringData( copy.GetNewProjectFile().GetFullPath() ) );
        BOOST_CHECK( saved.at( "boards" ) == expected );

        for( const auto& record : saved.at( "boards" ) )
        {
            wxFileName board( wxString::FromUTF8( record.at( 1 ).get<std::string>() ) );
            BOOST_REQUIRE( board.MakeAbsolute( outputDir ) );
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( board.GetFullPath() ), "unchanged board bytes" );
        }

        BOOST_CHECK( !wxFileExists( wxFileName( outputDir, primaryName ).GetFullPath() ) );
        BOOST_CHECK( !wxFileExists( wxFileName( outputNested, primaryName ).GetFullPath() ) );
        BOOST_CHECK( !wxFileExists( wxFileName( outputDir, wxS( "copy.aux." ) + boardExt ).GetFullPath() ) );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), sourceBytes );

        for( const wxString& board : { primary, extra, dotted, nested, external, sibling } )
            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( board ), "unchanged board bytes" );
    }
}


BOOST_AUTO_TEST_SUITE_END()
