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

#include <import_proj_properties.h>

BOOST_AUTO_TEST_SUITE( ImportProjProperties )

BOOST_AUTO_TEST_CASE( CacheNicknamesUseProjectStemWithoutImportOrDateSuffixes )
{
    const wxString dated = wxS( "demo-board-V7-20241210-1210" );
    const wxString project = wxS( "demo-board-V7" );
    BOOST_CHECK_EQUAL( IMPORT_PROJ_PROPS::StripImportDateSuffix( dated ), project );
    BOOST_CHECK_EQUAL( IMPORT_PROJ_PROPS::MakeCacheNickname( dated ), project );
    BOOST_CHECK_EQUAL( IMPORT_PROJ_PROPS::MakeSymbolCacheNickname( dated ), project );
    BOOST_CHECK_EQUAL( IMPORT_PROJ_PROPS::MakeCacheNickname( project ), project );
    BOOST_CHECK_EQUAL( IMPORT_PROJ_PROPS::MakeSymbolCacheNickname( project ), project );

    for( const wxString& stem : { wxString( "board-V7" ), wxString( "board-12345678-1234" ),
                                 wxString( "board-20241210-1210-rev2" ) } )
    {
        BOOST_CHECK_EQUAL( IMPORT_PROJ_PROPS::StripImportDateSuffix( stem ), stem );
        BOOST_CHECK_EQUAL( IMPORT_PROJ_PROPS::MakeCacheNickname( stem ), stem );
        BOOST_CHECK_EQUAL( IMPORT_PROJ_PROPS::MakeSymbolCacheNickname( stem ), stem );
    }
}


BOOST_AUTO_TEST_CASE( NetNameMapRoundTripsAndDistinguishesAbsence )
{
    const std::map<wxString, wxString> names = { { wxS( "N123" ), wxS( "Net-(R1-Pad1)" ) },
                                                 { wxS( "N456" ), wxS( "/sheet/SDA" ) } };

    auto decoded = IMPORT_PROJ_PROPS::SplitNetNameMap( IMPORT_PROJ_PROPS::JoinNetNameMap( names ) );
    BOOST_REQUIRE( decoded );
    BOOST_CHECK( *decoded == names );

    // A design that maps no nets still has to report that the import ran, because the manager reads
    // a present map as the schematic import having succeeded.
    auto empty = IMPORT_PROJ_PROPS::SplitNetNameMap( IMPORT_PROJ_PROPS::JoinNetNameMap( {} ) );
    BOOST_REQUIRE( empty );
    BOOST_CHECK( empty->empty() );

    // An import that never replied decodes to nothing at all.
    BOOST_CHECK( !IMPORT_PROJ_PROPS::SplitNetNameMap( wxString() ) );
}


BOOST_AUTO_TEST_CASE( ImportedFootprintLinksPreserveKnownEmptyPads )
{
    using namespace IMPORT_PROJ_PROPS;
    FOOTPRINT_LINK_DATA links;
    links.projectFile = wxS( "D:/runs/中文工程/中文工程.kemi_pro" );
    links.ids = { { wxS( "H1" ), wxS( "中文工程:机械孔" ) },
                  { wxS( "U1" ), wxS( "中文工程:器件" ) } };
    links.pads = { { wxS( "H1" ), {} }, { wxS( "U1" ), { wxS( "A1" ), wxS( "B2" ) } } };

    const wxString encoded = JoinFootprintLinkData( links );
    const std::optional<FOOTPRINT_LINK_DATA> decoded = SplitFootprintLinkData( encoded );
    BOOST_REQUIRE( decoded );
    BOOST_CHECK_EQUAL( decoded->projectFile, links.projectFile );
    BOOST_CHECK( decoded->ids == links.ids );
    BOOST_CHECK( decoded->pads == links.pads );
    BOOST_CHECK_EQUAL( decoded->pads.count( wxS( "H1" ) ), 1U );
    BOOST_CHECK( decoded->pads.at( wxS( "H1" ) ).empty() );

    // An absent pad-set entry is an incomplete reply, not a successfully loaded mechanical hole.
    links.pads.erase( wxS( "H1" ) );
    BOOST_CHECK( JoinFootprintLinkData( links ).IsEmpty() );
    BOOST_CHECK( !SplitFootprintLinkData( wxString() ) );
}


BOOST_AUTO_TEST_CASE( ImportedFootprintLinksRejectMalformedReplies )
{
    using namespace IMPORT_PROJ_PROPS;
    FOOTPRINT_LINK_DATA links;
    links.projectFile = wxS( "D:/runs/project/project.kemi_pro" );
    links.ids = { { wxS( "H1" ), wxS( "project:Hole" ) }, { wxS( "U1" ), wxS( "project:IC" ) } };
    links.pads = { { wxS( "H1" ), {} }, { wxS( "U1" ), { wxS( "A1" ), wxS( "B2" ) } } };
    const wxArrayString valid = wxSplit( JoinFootprintLinkData( links ), LIST_SEPARATOR, '\0' );
    BOOST_REQUIRE_EQUAL( valid.size(), 11U );

    wxArrayString truncated = valid;
    truncated.RemoveAt( truncated.size() - 1 );
    BOOST_CHECK( !SplitFootprintLinkData( JoinList( truncated ) ) );

    wxArrayString duplicateRef = valid;
    duplicateRef[6] = duplicateRef[3];
    BOOST_CHECK( !SplitFootprintLinkData( JoinList( duplicateRef ) ) );

    wxArrayString duplicatePad = valid;
    duplicatePad[10] = duplicatePad[9];
    BOOST_CHECK( !SplitFootprintLinkData( JoinList( duplicatePad ) ) );

    wxArrayString trailing = valid;
    trailing.Add( wxS( "unexpected" ) );
    BOOST_CHECK( !SplitFootprintLinkData( JoinList( trailing ) ) );

    wxArrayString invalidCount = valid;
    invalidCount[2] = wxS( "not-a-count" );
    BOOST_CHECK( !SplitFootprintLinkData( JoinList( invalidCount ) ) );
}

BOOST_AUTO_TEST_SUITE_END()
