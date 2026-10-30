/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
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

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <schematic_utils/schematic_file_util.h>

#include <set>
#include <advanced_config.h>
#include <erc/erc.h>
#include <kiface_ids.h>
#include <kiway.h>
#include <lib_symbol.h>
#include <locale_io.h>
#include <sch_marker.h>
#include <sch_pin.h>
#include <sch_sheet.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <settings/settings_manager.h>
#include <scoped_set_reset.h>
#include <sch_label.h>
#include "../pin_pad_query_test_utils.h"

BOOST_AUTO_TEST_CASE( ERCUnmappedPinsBindSharedInstancePaths )
{
    struct FOOTPRINT_FACE : KIFACE
    {
        bool OnKifaceStart( PGM_BASE*, int, KIWAY* ) override { return true; }
        void OnKifaceEnd() override {}
        void Reset() override {}
        wxWindow* CreateKiWindow( wxWindow*, int, KIWAY*, int ) override { return nullptr; }
        void GetActions( std::vector<TOOL_ACTION*>& ) const override {}

        static void Pads( const wxString& aFootprint, PROJECT*, std::set<wxString>& aPads )
        {
            if( aFootprint == "Acceptance:3" )
                aPads = { "1", "2" };
            else if( aFootprint == "Acceptance:2" )
                aPads = { "NO_NATIVE_PIN" };
        }

        void* IfaceOrAddress( int aId ) override
        {
            // ERC first asks the legacy pad-number entry point and asks KIFACE_FOOTPRINT_PAD_QUERY
            // only when that legacy answer is empty.  This face models a kiface built before the
            // query API existed: it must not be asked anything else, and it does not provide the
            // query, so an empty legacy answer stays "pad information unavailable".
            BOOST_REQUIRE( aId == KIFACE_FOOTPRINT_PAD_NUMBERS || aId == KIFACE_FOOTPRINT_PAD_QUERY );

            if( aId == KIFACE_FOOTPRINT_PAD_QUERY )
                return nullptr;

            return reinterpret_cast<void*>( &Pads );
        }
    } face;

    LOCALE_IO locale;
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, enabled );
    SETTINGS_MANAGER settings;
    std::unique_ptr<SCHEMATIC> schematic;
    KI_TEST::LoadSchematic( settings, "legacy_hierarchy/legacy_hierarchy", schematic );
    std::vector<SCH_SHEET_PATH> paths;

    for( const SCH_SHEET_PATH& path : schematic->Hierarchy() )
    {
        if( path.LastScreen()->GetFileName().EndsWith( "ampli_ht.kicad_sch" ) )
            paths.push_back( path );
    }

    BOOST_REQUIRE_EQUAL( paths.size(), 2u );
    SCH_SCREEN& screen = *paths[0].LastScreen();
    BOOST_REQUIRE( &screen == paths[1].LastScreen() );
    paths[0].SetPageNumber( "2" );
    paths[1].SetPageNumber( "3" );
    SCH_SYMBOL* symbol = nullptr;

    for( SCH_ITEM* item : screen.Items().OfType( SCH_SYMBOL_T ) )
    {
        auto* candidate = static_cast<SCH_SYMBOL*>( item );

        if( candidate->GetLibId().GetLibItemName() == "R" )
        {
            symbol = candidate;
            break;
        }
    }

    BOOST_REQUIRE( symbol );
    LIB_ID footprint;
    BOOST_REQUIRE_LT( footprint.Parse( "Acceptance:2", true ), 0 );
    symbol->GetLibSymbolRef()->SetAssociatedFootprints( { { footprint, wxString() } } );
    symbol->SetFootprintFieldText( "Acceptance:${#}" );
    schematic->ErcSettings().SetSeverity( ERCE_PIN_MAP_UNMAPPED_PIN, RPT_SEVERITY_WARNING );
    const auto pins = symbol->GetPins( &paths[0] );
    BOOST_REQUIRE_EQUAL( pins.size(), 2u );
    const std::set<KIID> expected{ pins[0]->m_Uuid, pins[1]->m_Uuid };

    for( bool backend : { true, false } )
    {
        enabled = backend;
        schematic->RebuildConnectivity();

        for( const SCH_SHEET_PATH& displayed : paths )
        {
            BOOST_TEST_CONTEXT( "backend=" << backend << "; displayed=" << displayed.GetPageNumber() )
            {
                schematic->SetCurrentSheet( displayed );
                ERC_TESTER tester( schematic.get() );
                tester.TestPinMap( &face, &schematic->Project() );
                std::set<KIID> found;
                size_t count = 0;
                std::vector<SCH_MARKER*> markers;

                for( SCH_ITEM* item : screen.Items().OfType( SCH_MARKER_T ) )
                {
                    auto* marker = static_cast<SCH_MARKER*>( item );
                    markers.push_back( marker );
                    const auto error = std::static_pointer_cast<ERC_ITEM>( marker->GetRCItem() );

                    if( error->GetErrorCode() != ERCE_PIN_MAP_UNMAPPED_PIN
                        || !expected.contains( error->GetMainItemID() ) )
                    {
                        continue;
                    }

                    ++count;
                    found.insert( error->GetMainItemID() );
                    BOOST_CHECK( error->IsSheetSpecific() );
                    BOOST_CHECK( error->MainItemHasSheetPath() );

                    if( error->IsSheetSpecific() && error->MainItemHasSheetPath() )
                    {
                        BOOST_CHECK( error->GetSpecificSheetPath().PathRef() == paths[0].PathRef() );
                        BOOST_CHECK( error->GetMainItemSheetPath().PathRef() == paths[0].PathRef() );
                    }
                }

                BOOST_CHECK_EQUAL( count, expected.size() );
                BOOST_CHECK( found == expected );

                for( SCH_MARKER* marker : markers )
                    screen.DeleteItem( marker );
            }
        }
    }

    for( bool backend : { false, true } )
    {
        BOOST_TEST_CONTEXT( "removed sheet; backend=" << backend )
        {
            enabled = backend;
            schematic->RebuildConnectivity();
            SCH_SHEET* removed = paths[0].Last();
            BOOST_REQUIRE( schematic->RootScreen()->Remove( removed ) );
            schematic->RefreshHierarchy();
            ERC_TESTER retained( schematic.get() );
            BOOST_CHECK_NO_THROW( retained.TestPinMap( &face, &schematic->Project() ) );
            schematic->RootScreen()->Append( removed );
            schematic->RefreshHierarchy();
            std::vector<SCH_MARKER*> markers;

            for( SCH_ITEM* item : screen.Items().OfType( SCH_MARKER_T ) )
                markers.push_back( static_cast<SCH_MARKER*>( item ) );

            for( SCH_MARKER* marker : markers )
                screen.DeleteItem( marker );
        }
    }
}


BOOST_AUTO_TEST_CASE( ERCUnmappedFootprintOnlyPinsPreserveDiagnostics )
{
    using namespace PIN_PAD_QUERY_TEST;
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, enabled );

    for( bool backend : { false, true } )
    {
        enabled = backend;

        for( const wxString& footprint : { wxString( "Acceptance:Empty" ), wxString( "Acceptance:Missing" ) } )
        {
            BOOST_TEST_CONTEXT( "backend=" << backend << "; footprint=" << footprint )
            {
                DESIGN design;
                FOOTPRINT_FACE face;
                SCH_SYMBOL* symbol = design.Add( wxS( "U1" ), footprint, VECTOR2I( 0, 0 ) );
                SCH_PIN* pin = symbol->GetPin( wxS( "1" ) );
                BOOST_REQUIRE( symbol->GetLibSymbolRef()->GetEffectiveAssociatedFootprints().empty() );
                design.schematic->RootScreen()->Append( new SCH_LABEL( pin->GetPosition(), wxS( "SOURCE_NET" ) ) );
                design.schematic->ErcSettings().SetSeverity( ERCE_PIN_MAP_UNMAPPED_PIN, RPT_SEVERITY_WARNING );
                design.schematic->RebuildConnectivity();
                ERC_TESTER tester( design.schematic.get() );
                tester.TestPinMap( &face, &design.schematic->Project() );
                int matching = 0;

                for( SCH_ITEM* item : design.schematic->RootScreen()->Items().OfType( SCH_MARKER_T ) )
                {
                    auto* marker = static_cast<SCH_MARKER*>( item );
                    const auto error = std::static_pointer_cast<ERC_ITEM>( marker->GetRCItem() );

                    if( error->GetErrorCode() == ERCE_PIN_MAP_UNMAPPED_PIN
                            && error->GetMainItemID() == pin->m_Uuid )
                    {
                        ++matching;
                        BOOST_CHECK( error->IsSheetSpecific() );
                        BOOST_CHECK( error->MainItemHasSheetPath() );
                    }
                }

                BOOST_CHECK_EQUAL( matching, 1 );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( ERCUnmappedAssociatedNoConnectPinsKeepExistingMarkers )
{
    using namespace PIN_PAD_QUERY_TEST;
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, enabled );

    for( bool backend : { false, true } )
    {
        enabled = backend;

        for( ELECTRICAL_PINTYPE type : { ELECTRICAL_PINTYPE::PT_NC, ELECTRICAL_PINTYPE::PT_NIC } )
        {
            BOOST_TEST_CONTEXT( "backend=" << backend << "; pin type=" << static_cast<int>( type ) )
            {
                DESIGN design;
                FOOTPRINT_FACE face;
                SCH_SYMBOL* symbol = design.Add( wxS( "U1" ), wxS( "Acceptance:Missing" ), VECTOR2I( 0, 0 ) );
                LIB_ID footprint;
                BOOST_REQUIRE_LT( footprint.Parse( wxS( "Acceptance:Missing" ), true ), 0 );
                symbol->GetLibSymbolRef()->SetAssociatedFootprints( { { footprint, wxString() } } );
                symbol->UpdatePins();
                SCH_PIN* pin = symbol->GetPin( wxS( "1" ) );
                pin->SetType( type );
                BOOST_REQUIRE( !pin->IsDangling() );
                design.schematic->ErcSettings().SetSeverity( ERCE_PIN_MAP_UNMAPPED_PIN, RPT_SEVERITY_WARNING );
                design.schematic->RebuildConnectivity();
                ERC_TESTER tester( design.schematic.get() );
                tester.TestPinMap( &face, &design.schematic->Project() );
                int matching = 0;

                for( SCH_ITEM* item : design.schematic->RootScreen()->Items().OfType( SCH_MARKER_T ) )
                {
                    const auto error = std::static_pointer_cast<ERC_ITEM>(
                            static_cast<SCH_MARKER*>( item )->GetRCItem() );

                    if( error->GetErrorCode() == ERCE_PIN_MAP_UNMAPPED_PIN
                        && error->GetMainItemID() == pin->m_Uuid )
                    {
                        ++matching;
                        BOOST_CHECK( error->IsSheetSpecific() );
                        const wxString message = error->GetErrorMessage( false ).Lower();
                        BOOST_CHECK( message.Contains( type == ELECTRICAL_PINTYPE::PT_NC
                                                              ? wxS( "no-connect" ) : wxS( "unconnected" ) ) );
                        BOOST_CHECK( !message.Contains( wxS( "is connected" ) ) );
                    }
                }

                BOOST_CHECK_EQUAL( matching, 1 );
                BOOST_CHECK( pin->GetType() == type );
            }
        }
    }
}
