/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 */

// KemiCAD 0.9.6: imported NPTH pads keep their pin numbers (e.g. CN602 7/8, CON1 6/7,
// CON2 1/6).  Only aperture pads (no copper layer) lose their number.  Numbered NPTH pads
// stay mechanical: no net from the netlist, no "no pin in symbol" warning, not a pin pad.

#include <board.h>
#include <footprint.h>
#include <pad.h>
#include <lib_id.h>
#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/pcb_netlist.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <project.h>
#include <settings/settings_manager.h>
#include <tool/tool_manager.h>
#include <pcbnew_utils/board_test_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>


namespace
{

PAD* addPad( FOOTPRINT* aFp, const wxString& aNumber, PAD_ATTRIB aAttr, const LSET& aLayers,
             const VECTOR2I& aPos )
{
    PAD* pad = new PAD( aFp );
    pad->SetAttribute( aAttr );
    pad->SetLayerSet( aLayers );
    // After SetAttribute(): switching to NPTH clears the number, like the GUI does.
    pad->SetNumber( aNumber );
    pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::CIRCLE );
    pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( 1000000, 1000000 ) );

    if( aAttr != PAD_ATTRIB::SMD )
        pad->SetDrillSize( VECTOR2I( 800000, 800000 ) );

    pad->SetPosition( aPos );
    aFp->Add( pad );
    return pad;
}


FOOTPRINT* makeConnector( BOARD* aBoard )
{
    FOOTPRINT* fp = new FOOTPRINT( aBoard );
    fp->SetReference( wxS( "CN602" ) );
    LIB_ID id;
    BOOST_REQUIRE_EQUAL( id.Parse( wxS( "TestLib:CONN" ) ), -1 );
    fp->SetFPID( id );
    addPad( fp, wxS( "1" ), PAD_ATTRIB::PTH, PAD::PTHMask(), VECTOR2I( 0, 0 ) );
    addPad( fp, wxS( "7" ), PAD_ATTRIB::NPTH, PAD::UnplatedHoleMask(), VECTOR2I( 3000000, 0 ) );
    addPad( fp, wxS( "8" ), PAD_ATTRIB::NPTH, PAD::UnplatedHoleMask(), VECTOR2I( 6000000, 0 ) );
    addPad( fp, wxS( "9" ), PAD_ATTRIB::SMD, LSET( { F_Paste } ), VECTOR2I( 9000000, 0 ) );
    return fp;
}


PAD* padAt( FOOTPRINT* aFp, int aX )
{
    for( PAD* pad : aFp->Pads() )
    {
        if( pad->GetPosition().x == aX )
            return pad;
    }

    return nullptr;
}

} // namespace


BOOST_AUTO_TEST_SUITE( KemiNpthPadNumber )


BOOST_AUTO_TEST_CASE( ParserKeepsNpthNumberClearsAperture )
{
    BOARD board;
    std::unique_ptr<FOOTPRINT> fp( makeConnector( &board ) );

    PCB_IO_KICAD_SEXPR io;
    io.Format( fp.get() );
    std::string text = io.GetStringOutput( true );

    std::unique_ptr<BOARD_ITEM> item( io.Parse( wxString::FromUTF8( text ) ) );
    FOOTPRINT* parsed = dynamic_cast<FOOTPRINT*>( item.get() );
    BOOST_REQUIRE( parsed );

    BOOST_CHECK_EQUAL( padAt( parsed, 0 )->GetNumber(), wxS( "1" ) );
    BOOST_CHECK_EQUAL( padAt( parsed, 3000000 )->GetNumber(), wxS( "7" ) );
    BOOST_CHECK_EQUAL( padAt( parsed, 6000000 )->GetNumber(), wxS( "8" ) );
    BOOST_CHECK( padAt( parsed, 3000000 )->GetAttribute() == PAD_ATTRIB::NPTH );

    // Aperture pads are still cleaned.
    BOOST_CHECK( padAt( parsed, 9000000 )->IsAperturePad() );
    BOOST_CHECK( padAt( parsed, 9000000 )->GetNumber().IsEmpty() );

    // Second round trip is stable.
    io.Format( parsed );
    std::unique_ptr<BOARD_ITEM> item2( io.Parse( wxString::FromUTF8( io.GetStringOutput( true ) ) ) );
    FOOTPRINT* parsed2 = dynamic_cast<FOOTPRINT*>( item2.get() );
    BOOST_REQUIRE( parsed2 );
    BOOST_CHECK_EQUAL( padAt( parsed2, 6000000 )->GetNumber(), wxS( "8" ) );
}


BOOST_AUTO_TEST_CASE( NpthStaysMechanical )
{
    BOARD board;
    std::unique_ptr<FOOTPRINT> fp( makeConnector( &board ) );
    PAD* npth = padAt( fp.get(), 3000000 );

    // Schematic parity / numbering predicate is unchanged: NPTH is not an electrical number.
    BOOST_CHECK( !npth->CanHaveNumber() );
    BOOST_CHECK( padAt( fp.get(), 0 )->CanHaveNumber() );

    std::set<wxString> numbers = fp->GetUniquePadNumbers();
    BOOST_CHECK_EQUAL( numbers.size(), 1 );
    BOOST_CHECK( numbers.count( wxS( "1" ) ) == 1 );
    BOOST_CHECK_EQUAL( fp->GetNumberedPadCount(), 1 );

    // ImportSettingsFrom keeps the NPTH number, clears aperture numbers.
    PAD copy( fp.get() );
    copy.SetNumber( wxS( "7" ) );
    copy.ImportSettingsFrom( *npth );
    BOOST_CHECK_EQUAL( copy.GetNumber(), wxS( "7" ) );

    PAD copy2( fp.get() );
    copy2.SetNumber( wxS( "9" ) );
    copy2.ImportSettingsFrom( *padAt( fp.get(), 9000000 ) );
    BOOST_CHECK( copy2.GetNumber().IsEmpty() );
}


struct NPTH_UPDATER_FIXTURE
{
    NPTH_UPDATER_FIXTURE()
    {
        m_settingsManager.LoadProject( "" );
        m_board = std::make_unique<BOARD>();
        m_board->SetProject( &m_settingsManager.Prj() );

        m_toolMgr.SetEnvironment( m_board.get(), nullptr, nullptr, nullptr, nullptr );
        m_toolMgr.RegisterTool( new KI_TEST::DUMMY_TOOL() );
    }

    SETTINGS_MANAGER       m_settingsManager;
    std::unique_ptr<BOARD> m_board;
    TOOL_MANAGER           m_toolMgr;
};


// Update PCB from schematic: symbol has pins 1, 7, 8 (7/8 are the mounting pins).  The
// numbered NPTH pads must not pick up nets and must not raise warnings or changes.
BOOST_FIXTURE_TEST_CASE( UpdateFromSchematicLeavesNumberedNpthUnconnected, NPTH_UPDATER_FIXTURE )
{
    FOOTPRINT* fp = makeConnector( m_board.get() );
    m_board->Add( fp );

    KIID    kiid;
    LIB_ID  id;
    BOOST_REQUIRE_EQUAL( id.Parse( wxS( "TestLib:CONN" ) ), -1 );

    NETLIST    netlist;
    COMPONENT* comp = new COMPONENT( id, wxS( "CN602" ), wxS( "CONN" ), KIID_PATH(),
                                     std::vector<KIID>{ kiid } );
    comp->AddNet( wxS( "1" ), wxS( "SIG" ), wxS( "SIG" ), wxS( "passive" ) );
    comp->AddNet( wxS( "7" ), wxS( "GND" ), wxS( "SHIELD" ), wxS( "passive" ) );
    comp->AddNet( wxS( "8" ), wxS( "GND" ), wxS( "SHIELD" ), wxS( "passive" ) );
    netlist.AddComponent( comp );

    BOARD_NETLIST_UPDATER updater( &m_toolMgr, m_board.get() );
    updater.SetIsDryRun( false );
    updater.SetWillDrag( false );
    updater.SetLookupByTimestamp( false );
    updater.SetReplaceFootprints( false );
    updater.SetDeleteUnusedFootprints( false );

    BOOST_REQUIRE( updater.UpdateNetlist( netlist ) );

    BOOST_CHECK_EQUAL( updater.GetErrorCount(), 0 );
    BOOST_CHECK_EQUAL( updater.GetWarningCount(), 0 );
    BOOST_REQUIRE_EQUAL( m_board->Footprints().size(), 1 );

    FOOTPRINT* out = m_board->Footprints().front();
    BOOST_CHECK_EQUAL( padAt( out, 0 )->GetNetname(), wxS( "SIG" ) );
    BOOST_CHECK_EQUAL( padAt( out, 3000000 )->GetNumber(), wxS( "7" ) );
    BOOST_CHECK( padAt( out, 3000000 )->GetNetname().IsEmpty() );
    BOOST_CHECK( padAt( out, 6000000 )->GetNetname().IsEmpty() );
    BOOST_CHECK( padAt( out, 3000000 )->GetPinFunction().IsEmpty() );
}


BOOST_AUTO_TEST_SUITE_END()
