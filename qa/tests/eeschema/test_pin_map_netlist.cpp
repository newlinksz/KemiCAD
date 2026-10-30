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
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <memory>

#include <pin_map.h>
#include <lib_symbol.h>
#include <sch_pin.h>
#include <sch_symbol.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <schematic.h>
#include <connection_graph.h>
#include <settings/settings_manager.h>
#include <netlist_exporters/netlist_exporter_base.h>
#include <netlist_exporters/netlist_exporter_xml.h>
#include <netlist_exporters/netlist_exporter_kicad.h>
#include <advanced_config.h>
#include <scoped_set_reset.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <wx/xml/xml.h>
#include <netlist_reader/netlist.h>
#include <netlist_reader/netlist_reader.h>
#include <richio.h>
#include "pin_pad_query_test_utils.h"


namespace
{
LIB_ID makeFp( const wxString& aLibNick, const wxString& aName )
{
    LIB_ID id;
    id.SetLibNickname( aLibNick );
    id.SetLibItemName( aName );
    return id;
}


/// Minimal concrete exporter that exposes the protected CreatePinList() for testing.
class TEST_NETLIST_EXPORTER : public NETLIST_EXPORTER_BASE
{
public:
    explicit TEST_NETLIST_EXPORTER( SCHEMATIC* aSchematic ) :
            NETLIST_EXPORTER_BASE( aSchematic, nullptr )
    {}

    std::vector<PIN_INFO> Pins( SCH_SYMBOL* aSymbol, const SCH_SHEET_PATH& aPath )
    {
        CONNECTIVITY_SCOPE connectivity( *this );
        return CreatePinList( aSymbol, aPath );
    }
};
} // namespace


struct PIN_MAP_NETLIST_FIXTURE
{
    PIN_MAP_NETLIST_FIXTURE()
    {
        m_settingsManager.LoadProject( "" );
        m_schematic = std::make_unique<SCHEMATIC>( &m_settingsManager.Prj() );
        m_schematic->Reset();

        SCH_SHEET*  defaultSheet = m_schematic->GetTopLevelSheet( 0 );
        SCH_SHEET*  root = new SCH_SHEET( m_schematic.get() );
        SCH_SCREEN* screen = new SCH_SCREEN( m_schematic.get() );
        root->SetScreen( screen );

        m_schematic->AddTopLevelSheet( root );
        m_schematic->RemoveTopLevelSheet( defaultSheet );
        delete defaultSheet;
    }

    SETTINGS_MANAGER            m_settingsManager;
    std::unique_ptr<SCHEMATIC>  m_schematic;
    std::unique_ptr<LIB_SYMBOL> m_libSym;
};


BOOST_FIXTURE_TEST_SUITE( PinMapNetlist, PIN_MAP_NETLIST_FIXTURE )


// The headline verification (plan step 3): an op-amp pin mapped to "[4,9]" for a DFN-8-EP package
// must produce netlist entries for BOTH pad 4 and pad 9 on the same net.
BOOST_AUTO_TEST_CASE( MappedPinEmitsBothStackedPads )
{
    SCH_SCREEN*    screen = m_schematic->RootScreen();
    SCH_SHEET_PATH path;
    path.push_back( &m_schematic->Root() );

    const LIB_ID dfn = makeFp( wxS( "Package_DFN_QFN" ), wxS( "DFN-8-1EP" ) );

    m_libSym = std::make_unique<LIB_SYMBOL>( "LM358", nullptr );

    for( const wxString& number : { wxString( "1" ), wxString( "4" ) } )
    {
        SCH_PIN* pin = new SCH_PIN( m_libSym.get() );
        pin->SetNumber( number );
        pin->SetType( ELECTRICAL_PINTYPE::PT_PASSIVE );
        pin->SetPosition( VECTOR2I( number == "1" ? -508000 : 508000, 0 ) );
        m_libSym->AddDrawItem( pin );
    }

    PIN_MAP map( wxS( "DFN-8-EP" ) );
    map.SetEntry( wxS( "4" ), wxS( "[4,9]" ) );   // V- maps to pad 4 AND the exposed pad 9
    m_libSym->PinMaps().AddOrReplace( map );
    m_libSym->SetAssociatedFootprints( { { dfn, wxS( "DFN-8-EP" ) } } );

    SCH_SYMBOL* sym = new SCH_SYMBOL( *m_libSym, m_libSym->GetLibId(), &path, 0, 0,
                                      VECTOR2I( 15621000, 6223000 ) );
    sym->GetField( FIELD_T::REFERENCE )->SetText( wxS( "U1" ) );
    sym->SetFootprintFieldText( dfn.GetUniStringLibId() );
    sym->UpdatePins();
    screen->Append( sym );

    SCH_SHEET_LIST sheets = m_schematic->BuildSheetListSortedByPageNumbers();
    m_schematic->ConnectionGraph()->Recalculate( sheets, true );

    TEST_NETLIST_EXPORTER  exporter( m_schematic.get() );
    std::vector<PIN_INFO>  pins = exporter.Pins( sym, sheets[0] );

    wxString netForFour;
    wxString netForNine;
    bool     hasPadOne = false;

    for( const PIN_INFO& info : pins )
    {
        if( info.num == wxS( "1" ) )
            hasPadOne = true;
        else if( info.num == wxS( "4" ) )
            netForFour = info.netName;
        else if( info.num == wxS( "9" ) )
            netForNine = info.netName;
    }

    BOOST_CHECK( hasPadOne );                          // pin 1 resolves by identity to pad 1
    BOOST_CHECK( !netForFour.IsEmpty() );              // V- reaches pad 4
    BOOST_CHECK( !netForNine.IsEmpty() );              // V- also reaches the exposed pad 9
    BOOST_CHECK_EQUAL( netForFour, netForNine );       // both pads are on the same net

    m_libSym.reset();
}


BOOST_AUTO_TEST_CASE( KnownEmptyAndUnavailableFootprintsRemainDistinct )
{
    using namespace PIN_PAD_QUERY_TEST;
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, enabled );

    for( bool backend : { false, true } )
    {
        enabled = backend;

        for( const wxString& footprint : { wxString( "Acceptance:Empty" ),
                                          wxString( "Acceptance:Unavailable" ), wxString() } )
        {
            BOOST_TEST_CONTEXT( "backend=" << backend << "; footprint=" << footprint )
            {
                DESIGN design;
                TEST_KIWAY kiway;
                SCH_SYMBOL* symbol = design.Add( wxS( "U1" ), footprint, VECTOR2I( 0, 0 ) );
                const KIID pinId = symbol->GetPin( wxS( "1" ) )->m_Uuid;
                const VECTOR2I pinPosition = symbol->GetPin( wxS( "1" ) )->GetPosition();
                OUTPUT_FILE output;
                REPORT reporter;
                NETLIST_EXPORTER_XML exporter( design.schematic.get(), &kiway );
                BOOST_REQUIRE( exporter.WriteNetlist( output.path, GNL_ALL, reporter ) );

                wxXmlDocument xml;
                BOOST_REQUIRE( xml.Load( output.path ) );
                int nodes = 0;

                for( wxXmlNode* section = xml.GetRoot()->GetChildren(); section; section = section->GetNext() )
                {
                    if( section->GetName() != wxS( "nets" ) )
                        continue;

                    for( wxXmlNode* net = section->GetChildren(); net; net = net->GetNext() )
                    {
                        for( wxXmlNode* node = net->GetChildren(); node; node = node->GetNext() )
                        {
                            if( node->GetName() == wxS( "node" )
                                    && node->GetAttribute( wxS( "ref" ) ) == wxS( "U1" ) )
                                ++nodes;
                        }
                    }
                }

                BOOST_CHECK_EQUAL( nodes, footprint == wxS( "Acceptance:Empty" ) ? 0 : 1 );
                BOOST_CHECK_EQUAL( reporter.errors, 0 );

                if( !footprint.IsEmpty() )
                {
                    BOOST_CHECK_GT( reporter.warnings, 0 );
                    BOOST_CHECK( reporter.messages.Contains( wxS( "U1" ) ) );
                    BOOST_CHECK( reporter.messages.Contains( footprint ) );
                }

                if( footprint == wxS( "Acceptance:Empty" ) )
                    BOOST_CHECK( reporter.messages.Lower().Contains( wxS( "unconnected" ) ) );

                int noConnects = 0;

                for( SCH_ITEM* item : design.schematic->RootScreen()->Items().OfType( SCH_NO_CONNECT_T ) )
                {
                    (void) item;
                    ++noConnects;
                }

                BOOST_CHECK_EQUAL( noConnects, 0 );

                BOOST_CHECK( symbol->GetPin( wxS( "1" ) )->m_Uuid == pinId );
                BOOST_CHECK( symbol->GetPin( wxS( "1" ) )->GetPosition() == pinPosition );
                BOOST_CHECK( symbol->GetPin( wxS( "1" ) )->GetType() == ELECTRICAL_PINTYPE::PT_PASSIVE );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( KnownEmptyNoConnectKeepsSourceMarker )
{
    using namespace PIN_PAD_QUERY_TEST;
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, enabled );

    for( bool backend : { false, true } )
    {
        enabled = backend;
        DESIGN design;
        TEST_KIWAY kiway;
        SCH_SYMBOL* symbol = design.Add( wxS( "U1" ), wxS( "Acceptance:Empty" ), VECTOR2I( 0, 0 ) );
        const VECTOR2I position = symbol->GetPin( wxS( "1" ) )->GetPosition();
        auto* marker = new SCH_NO_CONNECT( position );
        const KIID markerId = marker->m_Uuid;
        design.schematic->RootScreen()->Append( marker );
        OUTPUT_FILE output;
        REPORT reporter;
        NETLIST_EXPORTER_XML exporter( design.schematic.get(), &kiway );
        BOOST_REQUIRE( exporter.WriteNetlist( output.path, GNL_ALL, reporter ) );
        BOOST_CHECK_EQUAL( reporter.errors, 0 );
        BOOST_CHECK_GT( reporter.warnings, 0 );
        BOOST_CHECK( reporter.messages.Contains( wxS( "U1" ) ) );
        BOOST_CHECK( reporter.messages.Lower().Contains( wxS( "no-connect" ) ) );
        int preserved = 0;

        for( SCH_ITEM* item : design.schematic->RootScreen()->Items().OfType( SCH_NO_CONNECT_T ) )
        {
            if( item->m_Uuid == markerId && item->GetPosition() == position )
                ++preserved;
        }

        BOOST_CHECK_EQUAL( preserved, 1 );
        BOOST_CHECK( symbol->GetPin( wxS( "1" ) )->GetType() == ELECTRICAL_PINTYPE::PT_PASSIVE );
    }
}


BOOST_AUTO_TEST_CASE( ConnectedUnmappedPadsBlockBeforeOutput )
{
    using namespace PIN_PAD_QUERY_TEST;
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, enabled );

    for( bool backend : { false, true } )
    {
        enabled = backend;

        for( const wxString& footprint : { wxString( "Acceptance:Empty" ), wxString( "Acceptance:Missing" ) } )
        {
            for( int topology : { 0, 1, 2 } )
            {
                for( bool native : { false, true } )
                {
                    BOOST_TEST_CONTEXT( "backend=" << backend << "; footprint=" << footprint
                                        << "; topology=" << topology << "; native=" << native )
                    {
                        DESIGN design;
                        TEST_KIWAY kiway;
                        SCH_SYMBOL* source = design.Add( wxS( "U1" ), footprint, VECTOR2I( 0, 0 ) );
                        const VECTOR2I position = source->GetPin( wxS( "1" ) )->GetPosition();

                        if( topology == 1 )
                        {
                            // A named single-pin net is still a real connection.
                            design.schematic->RootScreen()->Append( new SCH_LABEL( position, wxS( "SOURCE_NET" ) ) );
                        }
                        else
                        {
                            SCH_SYMBOL* target = design.Add( wxS( "U2" ), wxS( "Acceptance:Good" ),
                                                             VECTOR2I( 2540000, 0 ) );
                            auto* wire = new SCH_LINE( position, LAYER_WIRE );
                            wire->SetEndPoint( target->GetPin( wxS( "1" ) )->GetPosition() );
                            design.schematic->RootScreen()->Append( wire );

                            if( topology == 2 )
                                design.schematic->RootScreen()->Append( new SCH_NO_CONNECT( position ) );
                        }

                        OUTPUT_FILE output;
                        const wxString sentinel = wxS( "existing output must survive rejected export\n" );
                        output.Write( sentinel );
                        REPORT reporter;
                        std::unique_ptr<NETLIST_EXPORTER_BASE> exporter;

                        if( native )
                            exporter = std::make_unique<NETLIST_EXPORTER_KICAD>( design.schematic.get(), &kiway );
                        else
                            exporter = std::make_unique<NETLIST_EXPORTER_XML>( design.schematic.get(), &kiway );

                        BOOST_CHECK( !exporter->WriteNetlist( output.path, GNL_ALL, reporter ) );
                        BOOST_CHECK_GT( reporter.errors, 0 );
                        BOOST_CHECK( reporter.messages.Contains( wxS( "U1" ) ) );
                        BOOST_CHECK( reporter.messages.Contains( footprint ) );
                        BOOST_CHECK_EQUAL( output.Read(), sentinel );
                    }
                }
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( PhysicalPadValidationPreservesAssignmentAndOfflineExport )
{
    using namespace PIN_PAD_QUERY_TEST;
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, enabled );

    for( bool backend : { false, true } )
    {
        enabled = backend;

        for( bool assignment : { false, true } )
        {
            BOOST_TEST_CONTEXT( "backend=" << backend << "; assignment=" << assignment )
            {
                DESIGN design;
                TEST_KIWAY kiway;
                const wxString footprint = assignment ? wxS( "Acceptance:Empty" )
                                                      : wxS( "Acceptance:Unavailable" );
                SCH_SYMBOL* symbol = design.Add( wxS( "U1" ), footprint, VECTOR2I( 0, 0 ) );
                SCH_PIN* pin = symbol->GetPin( wxS( "1" ) );
                design.schematic->RootScreen()->Append( new SCH_LABEL( pin->GetPosition(), wxS( "SOURCE_NET" ) ) );
                NETLIST_EXPORTER_KICAD exporter( design.schematic.get(), &kiway );
                STRING_FORMATTER formatter;
                REPORT reporter;
                const int options = GNL_ALL | GNL_OPT_KICAD
                                    | ( assignment ? GNL_OPT_FOOTPRINT_ASSIGNMENT : 0 );
                BOOST_REQUIRE( exporter.Format( &formatter, options, &reporter ) );
                BOOST_CHECK_EQUAL( reporter.errors, 0 );

                if( !assignment )
                    BOOST_CHECK_GT( reporter.warnings, 0 );

                // Use the same native reader as CvPcb, retaining its pin-count filter and
                // preview pin-function/net data even when the assignment needs repair.
                NETLIST netlist;
                KICAD_NETLIST_READER reader(
                        new STRING_LINE_READER( formatter.GetString(), "assignment" ), &netlist );
                BOOST_REQUIRE_NO_THROW( reader.LoadNetlist() );
                COMPONENT* component = netlist.GetComponentByReference( wxS( "U1" ) );
                BOOST_REQUIRE( component );
                BOOST_CHECK_EQUAL( component->GetPinCount(), 1u );
                BOOST_CHECK_EQUAL( component->GetFPID().GetUniStringLibId(), footprint );
                BOOST_CHECK_EQUAL( component->GetNetCount(), 1u );
                BOOST_CHECK( component->GetNet( wxS( "1" ) ).GetNetName().Contains( wxS( "SOURCE_NET" ) ) );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( InvalidStackedPadMappingCannotPassAsLiteralPad )
{
    using namespace PIN_PAD_QUERY_TEST;
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, enabled );

    for( bool backend : { false, true } )
    {
        enabled = backend;

        // The malformed descending range falls back to its entire input string.  A real pad
        // with that literal name cannot turn an invalid mapping into a valid physical export.
        // The escaped literal and an ordinary valid stacked map remain supported.
        for( const wxString& mapping : { wxString( "[2-1]" ), wxString( "[\\[2\\-1\\]]" ), wxString( "[2,3]" ) } )
        {
            for( bool native : { false, true } )
            {
                BOOST_TEST_CONTEXT( "backend=" << backend << "; map=" << mapping << "; native=" << native )
                {
                    DESIGN design;
                    TEST_KIWAY kiway;
                    SCH_SYMBOL* symbol = design.Add( wxS( "U1" ), wxS( "Acceptance:Stacked" ), VECTOR2I( 0, 0 ) );
                    PIN_MAP_INSTANCE_OVERRIDE override;
                    override.m_Edits.push_back( { wxS( "1" ), mapping } );
                    symbol->SetPinMapOverride( override );
                    design.schematic->RootScreen()->Append(
                            new SCH_LABEL( symbol->GetPin( wxS( "1" ) )->GetPosition(), wxS( "SOURCE_NET" ) ) );
                    OUTPUT_FILE output;
                    const wxString sentinel = wxS( "preserved before invalid stacked map\n" );
                    output.Write( sentinel );
                    REPORT reporter;
                    std::unique_ptr<NETLIST_EXPORTER_BASE> exporter;

                    if( native )
                        exporter = std::make_unique<NETLIST_EXPORTER_KICAD>( design.schematic.get(), &kiway );
                    else
                        exporter = std::make_unique<NETLIST_EXPORTER_XML>( design.schematic.get(), &kiway );

                    const bool valid = mapping != wxS( "[2-1]" );
                    BOOST_CHECK_EQUAL( exporter->WriteNetlist( output.path, GNL_ALL, reporter ), valid );

                    if( valid )
                    {
                        BOOST_CHECK_EQUAL( reporter.errors, 0 );
                        BOOST_CHECK( output.Read() != sentinel );
                    }
                    else
                    {
                        BOOST_CHECK_GT( reporter.errors, 0 );
                        BOOST_CHECK( reporter.messages.Contains( wxS( "U1" ) ) );
                        BOOST_CHECK_EQUAL( output.Read(), sentinel );
                    }
                }
            }
        }
    }
}


BOOST_AUTO_TEST_SUITE_END()
