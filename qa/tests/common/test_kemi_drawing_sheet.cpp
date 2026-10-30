/*
 * This file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <kemi_format_profile.h>
#include <ki_exception.h>
#include <boost/test/unit_test.hpp>
#include <drawing_sheet/ds_data_item.h>
#include <drawing_sheet/ds_data_model.h>
#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>


BOOST_AUTO_TEST_SUITE( KemiDrawingSheet )

BOOST_AUTO_TEST_CASE( TemporarySaveUsesFinalFormatAndReopens )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet" ) );
    DS_DATA_MODEL original;
    original.SetDefaultLayout();
    original.SetLeftMargin( 13.25 );

    for( const wxString& extension : { wxS( "kemi_wks" ) } )
    {
        const wxString target = dir.PathStr() + wxS( "/drawing." ) + extension;
        const wxString temporary = dir.PathStr() + wxS( "/temporary" );

        original.Save( temporary, KEMI_FORMAT::FlavorForPath(
                                          KEMI_FORMAT::DOC::DRAWING_SHEET, target ) );
        BOOST_REQUIRE( wxRenameFile( temporary, target ) );

        DS_DATA_MODEL loaded;
        wxString error;
        BOOST_REQUIRE_MESSAGE( loaded.LoadDrawingSheet( target, &error ), error );
        BOOST_CHECK_EQUAL( loaded.GetCount(), original.GetCount() );
        BOOST_CHECK_EQUAL( loaded.GetLeftMargin(), 13.25 );

        // Ordinary saving must preserve the format chosen by the final file path too.
        loaded.Save( target );
        DS_DATA_MODEL reopened;
        BOOST_REQUIRE_MESSAGE( reopened.LoadDrawingSheet( target, &error ), error );
        BOOST_CHECK_EQUAL( reopened.GetLeftMargin(), 13.25 );

        const std::string bytes = KI_TEST::LoadStringData( target );
        BOOST_CHECK( bytes.find( extension == wxS( "kemi_wks" ) ? "(kemi_wks" : "(kicad_wks" )
                     != std::string::npos );
    }
}

BOOST_AUTO_TEST_CASE( RejectsInvalidContainerVersions )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet-version" ) );
    const wxString target = dir.ChildPathStr( wxS( "drawing.kemi_wks" ) );

    for( const wxString& version : { wxS( "1.5" ), wxS( "2" ), wxS( "4294967297" ) } )
    {
        {
            wxFFile file( target, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write( wxS( "(kemi_wks (format_version " ) + version
                                       + wxS( ") (version 20231118))" ), wxConvUTF8 ) );
        }

        DS_DATA_MODEL loaded;
        wxString error;
        BOOST_CHECK( !loaded.LoadDrawingSheet( target, &error ) );
        BOOST_CHECK( !error.IsEmpty() );
    }
}

BOOST_AUTO_TEST_CASE( RejectsUnclosedContainerWithoutChangingFile )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet-unclosed" ) );
    const wxString target = dir.ChildPathStr( wxS( "drawing.kemi_wks" ) );

    {
        wxFFile file( target, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(kemi_wks (format_version 1) (version 20231118)" ),
                                   wxConvUTF8 ) );
    }

    const std::string original = KI_TEST::LoadStringData( target );
    DS_DATA_MODEL loaded;
    wxString error;
    BOOST_CHECK( !loaded.LoadDrawingSheet( target, &error ) );
    BOOST_CHECK( !error.IsEmpty() );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( target ), original );
}

BOOST_AUTO_TEST_CASE( RejectsContentAfterContainerWithoutChangingFile )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet-trailing" ) );
    const wxString target = dir.ChildPathStr( wxS( "drawing.kemi_wks" ) );

    for( const wxString& trailing : { wxS( "(version 20231118)" ), wxS( ")" ), wxS( "extra" ) } )
    {
        {
            wxFFile file( target, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write( wxS( "(kemi_wks (format_version 1) (version 20231118))\n" )
                                       + trailing, wxConvUTF8 ) );
        }

        const std::string original = KI_TEST::LoadStringData( target );
        DS_DATA_MODEL loaded;
        wxString error;
        BOOST_CHECK( !loaded.LoadDrawingSheet( target, &error ) );
        BOOST_CHECK( !error.IsEmpty() );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( target ), original );
    }
}

BOOST_AUTO_TEST_CASE( AcceptsClosedContainerWithTrailingWhitespace )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet-closed" ) );
    const wxString target = dir.ChildPathStr( wxS( "drawing.kemi_wks" ) );

    {
        wxFFile file( target, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(kemi_wks (format_version 1) (version 20231118))\n \t\r\n" ),
                                   wxConvUTF8 ) );
    }

    DS_DATA_MODEL loaded;
    wxString error;
    BOOST_REQUIRE_MESSAGE( loaded.LoadDrawingSheet( target, &error ), error );
    BOOST_CHECK_EQUAL( loaded.GetFileFormatVersionAtLoad(), 20231118 );
}

BOOST_AUTO_TEST_CASE( AppendFailureLeavesModelAndSavedFileUnchanged )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet-append-failure" ) );

    for( const wxString& extension : { wxS( "kicad_wks" ), wxS( "kemi_wks" ) } )
    {
        const wxString source = dir.ChildPathStr( wxS( "invalid." ) + extension );
        const wxString target = dir.ChildPathStr( wxS( "drawing-" ) + extension + wxS( ".kemi_wks" ) );
        const wxString root = extension == wxS( "kemi_wks" )
                                      ? wxS( "(kemi_wks (format_version 1)" )
                                      : wxS( "(kicad_wks" );

        {
            wxFFile file( source, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write(
                    root + wxS( " (version 20231118) (generator external)"
                                " (setup (left_margin 99) (right_margin 98)"
                                " (top_margin 97) (bottom_margin 96)"
                                " (textsize 8 9) (linewidth 7) (textlinewidth 6))"
                                " (line (name partial) (start 1 2) (end 3 4))"
                                " (unexpected))" ), wxConvUTF8 ) );
        }

        DS_DATA_MODEL model;
        model.SetDefaultLayout();
        model.SetGenerator( wxS( "original" ) );
        model.SetFileFormatVersionAtLoad( 20210606 );
        model.SetLeftMargin( 13 );
        model.SetRightMargin( 14 );
        model.SetTopMargin( 15 );
        model.SetBottomMargin( 16 );
        model.m_DefaultTextSize = VECTOR2D( 2, 3 );
        model.m_DefaultLineWidth = 0.4;
        model.m_DefaultTextThickness = 0.5;
        model.AllowVoidList( false );
        const std::vector<DS_DATA_ITEM*> originalItems = model.GetItems();
        model.Save( target );
        const std::string original = KI_TEST::LoadStringData( target );
        const std::string invalid = KI_TEST::LoadStringData( source );

        wxString error;
        BOOST_CHECK( !model.LoadDrawingSheet( source, &error, true ) );
        BOOST_CHECK( !error.IsEmpty() );
        BOOST_CHECK_EQUAL_COLLECTIONS( model.GetItems().begin(), model.GetItems().end(),
                                      originalItems.begin(), originalItems.end() );
        BOOST_CHECK_EQUAL( model.GetFileFormatVersionAtLoad(), 20210606 );
        BOOST_CHECK_EQUAL( model.GetGenerator(), wxS( "original" ) );
        BOOST_CHECK_EQUAL( model.GetLeftMargin(), 13 );
        BOOST_CHECK_EQUAL( model.GetRightMargin(), 14 );
        BOOST_CHECK_EQUAL( model.GetTopMargin(), 15 );
        BOOST_CHECK_EQUAL( model.GetBottomMargin(), 16 );
        BOOST_CHECK_EQUAL( model.m_DefaultTextSize.x, 2 );
        BOOST_CHECK_EQUAL( model.m_DefaultTextSize.y, 3 );
        BOOST_CHECK_EQUAL( model.m_DefaultLineWidth, 0.4 );
        BOOST_CHECK_EQUAL( model.m_DefaultTextThickness, 0.5 );
        BOOST_CHECK( !model.VoidListAllowed() );
        model.Save( target );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( target ), original );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), invalid );
    }
}

BOOST_AUTO_TEST_CASE( AppendSuccessMergesItemsAndSetup )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet-append-success" ) );

    for( const wxString& extension : { wxS( "kicad_wks" ), wxS( "kemi_wks" ) } )
    {
        const wxString source = dir.ChildPathStr( wxS( "append." ) + extension );
        const wxString target = dir.ChildPathStr( wxS( "drawing-" ) + extension + wxS( ".kemi_wks" ) );
        const wxString root = extension == wxS( "kemi_wks" )
                                      ? wxS( "(kemi_wks (format_version 1)" )
                                      : wxS( "(kicad_wks" );

        {
            wxFFile file( source, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write(
                    root + wxS( " (version 20231118) (generator external)"
                                " (setup (left_margin 99) (linewidth 0.4))"
                                " (line (name appended) (start 1 2) (end 3 4)))" ), wxConvUTF8 ) );
        }

        DS_DATA_MODEL model;
        model.SetDefaultLayout();
        model.SetRightMargin( 14 );
        model.m_DefaultTextSize = VECTOR2D( 2, 3 );
        model.AllowVoidList( false );
        const std::vector<DS_DATA_ITEM*> originalItems = model.GetItems();
        const std::string originalSource = KI_TEST::LoadStringData( source );
        wxString error;
        BOOST_REQUIRE_MESSAGE( model.LoadDrawingSheet( source, &error, true ), error );
        BOOST_REQUIRE_EQUAL( model.GetCount(), originalItems.size() + 1 );

        for( unsigned i = 0; i < originalItems.size(); ++i )
            BOOST_CHECK( model.GetItem( i ) == originalItems[i] );

        BOOST_CHECK_EQUAL( model.GetItem( model.GetCount() - 1 )->m_Name, wxS( "appended" ) );
        BOOST_CHECK_EQUAL( model.GetFileFormatVersionAtLoad(), 20231118 );
        BOOST_CHECK_EQUAL( model.GetGenerator(), wxS( "external" ) );
        BOOST_CHECK_EQUAL( model.GetLeftMargin(), 99 );
        BOOST_CHECK_EQUAL( model.GetRightMargin(), 14 );
        BOOST_CHECK_EQUAL( model.m_DefaultLineWidth, 0.4 );
        BOOST_CHECK_EQUAL( model.m_DefaultTextSize.x, 2 );
        BOOST_CHECK_EQUAL( model.m_DefaultTextSize.y, 3 );
        BOOST_CHECK( model.VoidListAllowed() );
        model.Save( target );
        DS_DATA_MODEL reopened;
        BOOST_REQUIRE_MESSAGE( reopened.LoadDrawingSheet( target, &error ), error );
        BOOST_CHECK_EQUAL( reopened.GetCount(), model.GetCount() );
        BOOST_CHECK_EQUAL( reopened.GetItem( reopened.GetCount() - 1 )->m_Name, wxS( "appended" ) );
        BOOST_CHECK_EQUAL( reopened.GetLeftMargin(), 99 );
        BOOST_CHECK_EQUAL( reopened.GetRightMargin(), 14 );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), originalSource );
    }
}

// DW08 / K06: Provenance follows both the native extension and an accepted native
// root renamed to an official suffix. Reject before opening any file formatter.
BOOST_AUTO_TEST_CASE( LoadedNativeDrawingSheetRejectsOfficialOutput )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet-persistent" ) );
    DS_DATA_MODEL source;
    source.SetDefaultLayout();
    const wxString native = dir.ChildPathStr( wxS( "source.kemi_wks" ) );
    source.Save( native );
    const wxString renamed = dir.ChildPathStr( wxS( "renamed.kicad_wks" ) );
    BOOST_REQUIRE( wxCopyFile( native, renamed ) );

    for( const wxString& input : { native, renamed } )
    {
        DS_DATA_MODEL loaded;
        wxString error;
        BOOST_REQUIRE_MESSAGE( loaded.LoadDrawingSheet( input, &error ), error );
        BOOST_REQUIRE( loaded.IsKemiOnlyOutput() );
        const wxString official = dir.ChildPathStr( wxS( "blocked.kicad_wks" ) );
        const wxString temporary = dir.ChildPathStr( wxS( "blocked.tmp" ) );
        BOOST_CHECK_THROW( loaded.Save( official ), IO_ERROR );
        BOOST_CHECK_THROW( loaded.Save( official, KEMI_FORMAT::FLAVOR::KEMI ), IO_ERROR );
        BOOST_CHECK_THROW( loaded.Save( temporary, KEMI_FORMAT::FLAVOR::LEGACY ), IO_ERROR );
        BOOST_CHECK( !wxFileExists( official ) );
        BOOST_CHECK( !wxFileExists( temporary ) );

        // The undo/clipboard serializer must preserve the marker too.
        wxString serialized;
        loaded.SaveInString( &serialized );
        BOOST_CHECK( serialized.StartsWith( wxS( "(kemi_wks" ) ) );
        DS_DATA_MODEL undo;
        undo.SetPageLayout( serialized.utf8_str() );
        BOOST_CHECK( undo.IsKemiOnlyOutput() );
        BOOST_CHECK_THROW( undo.Save( official ), IO_ERROR );
        loaded.SetDefaultLayout();
        BOOST_CHECK( loaded.IsKemiOnlyOutput() );

        const wxString name = dir.ChildPathStr( wxS( "saved" ) );
        loaded.Save( name );
        BOOST_CHECK( !wxFileExists( name ) );
        BOOST_CHECK( wxFileExists( name + wxS( ".kemi_wks" ) ) );
    }
}


// DW09 / K03,K06: Failed parsing must not change policy; a successful native
// append restricts the merged drawing sheet, even after later legacy appends.
BOOST_AUTO_TEST_CASE( NativeProvenanceFollowsSuccessfulTransactions )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-drawing-sheet-provenance" ) );
    const wxString native = dir.ChildPathStr( wxS( "native.kemi_wks" ) );
    const wxString legacy = dir.ChildPathStr( wxS( "legacy.kicad_wks" ) );
    const wxString invalid = dir.ChildPathStr( wxS( "invalid.kemi_wks" ) );
    DS_DATA_MODEL source;
    source.SetDefaultLayout();
    wxString legacyText;
    source.SaveInString( &legacyText );
    {
        wxFFile fixture( legacy, wxS( "wb" ) );
        BOOST_REQUIRE( fixture.IsOpened() );
        BOOST_REQUIRE( fixture.Write( legacyText, wxConvUTF8 ) );
    }
    source.Save( native );

    {
        wxFFile file( invalid, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "(kemi_wks (format_version 1) (version 20231118) (invalid))" ),
                                   wxConvUTF8 ) );
    }

    DS_DATA_MODEL model;
    model.SetDefaultLayout();
    const unsigned count = model.GetCount();
    wxString error;
    BOOST_CHECK( !model.LoadDrawingSheet( invalid, &error, true ) );
    BOOST_CHECK( !model.IsKemiOnlyOutput() );
    BOOST_CHECK_EQUAL( model.GetCount(), count );
    BOOST_REQUIRE_MESSAGE( model.LoadDrawingSheet( native, &error, true ), error );
    BOOST_CHECK( model.IsKemiOnlyOutput() );
    BOOST_REQUIRE_MESSAGE( model.LoadDrawingSheet( legacy, &error, true ), error );
    BOOST_CHECK( model.IsKemiOnlyOutput() );
    const unsigned mergedCount = model.GetCount();
    BOOST_CHECK( !model.LoadDrawingSheet( invalid, &error ) );
    BOOST_CHECK( model.IsKemiOnlyOutput() );
    BOOST_CHECK_EQUAL( model.GetCount(), mergedCount );

    // Explicitly loading a different, valid official document replaces the old data.
    BOOST_REQUIRE_MESSAGE( model.LoadDrawingSheet( legacy, &error ), error );
    BOOST_CHECK( !model.IsKemiOnlyOutput() );
    BOOST_CHECK_EQUAL( model.GetCount(), count );
}


BOOST_AUTO_TEST_SUITE_END()
