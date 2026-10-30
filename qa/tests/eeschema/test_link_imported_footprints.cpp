/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
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

#include <memory>
#include <iterator>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <schematic_utils/schematic_file_util.h>

#include <lib_symbol.h>
#include <pgm_base.h>
#include <project.h>
#include <reporter.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_label.h>
#include <sch_pin.h>
#include <ki_exception.h>
#include <import_proj_properties.h>
#include <settings/settings_manager.h>

#include <link_imported_footprints.h>


BOOST_AUTO_TEST_SUITE( LinkImportedFootprints_ )


// Two unannotated symbols share R?, so only the annotated one may take the board footprint
BOOST_AUTO_TEST_CASE( UnannotatedReferencesAreNotLinked )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();

    std::unique_ptr<SCHEMATIC> schematic = std::make_unique<SCHEMATIC>( &project );
    schematic->Reset();

    BOOST_REQUIRE_EQUAL( schematic->Hierarchy().size(), 1U );

    SCH_SHEET_PATH path = schematic->Hierarchy().front();
    SCH_SCREEN*    screen = path.LastScreen();
    LIB_SYMBOL     lib( wxS( "R" ), nullptr );

    std::vector<SCH_SYMBOL*> symbols;

    for( const wxString& ref : { wxS( "R?" ), wxS( "R?" ), wxS( "R1" ) } )
    {
        SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, 1 );
        symbol->SetRef( &path, ref );
        screen->Append( symbol );
        symbols.push_back( symbol );
    }

    std::map<wxString, wxString> footprints = { { wxS( "R?" ), wxS( "lib:R_0603" ) },
                                                { wxS( "R1" ), wxS( "lib:R_0805" ) } };

    BOOST_CHECK_EQUAL( LinkImportedFootprints( *schematic, footprints, NULL_REPORTER::GetInstance() ), 1 );

    BOOST_CHECK( symbols[0]->GetField( FIELD_T::FOOTPRINT )->GetText().IsEmpty() );
    BOOST_CHECK( symbols[1]->GetField( FIELD_T::FOOTPRINT )->GetText().IsEmpty() );
    BOOST_CHECK_EQUAL( symbols[2]->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "lib:R_0805" ) );
}


// The import cache renames a footprint two libraries supply differently, which only the board knows, but a field in
// another library is the user's choice
BOOST_AUTO_TEST_CASE( CacheRenamesReachTheFields )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();

    std::unique_ptr<SCHEMATIC> schematic = std::make_unique<SCHEMATIC>( &project );
    schematic->Reset();

    SCH_SHEET_PATH path = schematic->Hierarchy().front();
    SCH_SCREEN*    screen = path.LastScreen();
    LIB_SYMBOL     lib( wxS( "R" ), nullptr );

    std::vector<SCH_SYMBOL*> symbols;

    for( const wxString& ref : { wxS( "R1" ), wxS( "R2" ) } )
    {
        SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, 1 );
        symbol->SetRef( &path, ref );
        screen->Append( symbol );
        symbols.push_back( symbol );
    }

    symbols[0]->SetFootprintFieldText( wxS( "cache:SOIC8" ) );
    symbols[1]->SetFootprintFieldText( wxS( "mine:SOIC8" ) );

    std::map<wxString, wxString> footprints = { { wxS( "R1" ), wxS( "cache:SOIC8_1" ) },
                                                { wxS( "R2" ), wxS( "cache:SOIC8_1" ) } };

    BOOST_CHECK_EQUAL( LinkImportedFootprints( *schematic, footprints, NULL_REPORTER::GetInstance() ), 1 );

    BOOST_CHECK_EQUAL( symbols[0]->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "cache:SOIC8_1" ) );
    BOOST_CHECK_EQUAL( symbols[1]->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "mine:SOIC8" ) );
}


// A sheet used twice shares its symbols, so a rename that reaches only one of the two parts must leave the field
BOOST_AUTO_TEST_CASE( SharedSheetInstancesMustAgree )
{
    SETTINGS_MANAGER           settings;
    std::unique_ptr<SCHEMATIC> schematic;
    KI_TEST::LoadSchematic( settings, "issue23840/BusAndVectors", schematic );

    std::vector<SCH_SHEET_PATH> paths;

    for( const SCH_SHEET_PATH& path : schematic->Hierarchy() )
    {
        if( path.LastScreen()->GetFileName().EndsWith( "LEDs.kicad_sch" ) )
            paths.push_back( path );
    }

    BOOST_REQUIRE_EQUAL( paths.size(), 2 );

    SCH_SYMBOL* shared = nullptr;

    for( SCH_ITEM* item : paths[0].LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->GetRef( &paths[0] ) != symbol->GetRef( &paths[1] ) )
        {
            shared = symbol;
            break;
        }
    }

    BOOST_REQUIRE( shared );

    const wxString first = shared->GetRef( &paths[0] );
    const wxString second = shared->GetRef( &paths[1] );

    shared->SetFootprintFieldText( wxS( "cache:SOIC8" ) );

    std::map<wxString, wxString> split = { { first, wxS( "cache:SOIC8_1" ) }, { second, wxS( "mine:SOIC8" ) } };

    BOOST_CHECK_EQUAL( LinkImportedFootprints( *schematic, split, NULL_REPORTER::GetInstance() ), 0 );
    BOOST_CHECK_EQUAL( shared->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "cache:SOIC8" ) );

    std::map<wxString, wxString> agreed = { { first, wxS( "cache:SOIC8_1" ) }, { second, wxS( "cache:SOIC8_1" ) } };

    BOOST_CHECK_GT( LinkImportedFootprints( *schematic, agreed, NULL_REPORTER::GetInstance() ), 0 );
    BOOST_CHECK_EQUAL( shared->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "cache:SOIC8_1" ) );
}


// An accidental duplicate reference is not evidence that two symbol objects are one physical part.
BOOST_AUTO_TEST_CASE( DuplicateAnnotatedReferencesAreNotLinked )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();
    SCHEMATIC schematic( &project );
    schematic.Reset();
    SCH_SHEET_PATH path = schematic.Hierarchy().front();
    LIB_SYMBOL lib( wxS( "R" ), nullptr );
    std::vector<SCH_SYMBOL*> symbols;

    for( int i = 0; i < 2; ++i )
    {
        SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, 1 );
        symbol->SetRef( &path, wxS( "R1" ) );
        path.LastScreen()->Append( symbol );
        symbols.push_back( symbol );
    }

    const std::map<wxString, wxString> footprints = { { wxS( "R1" ), wxS( "cache:R_0603" ) } };
    BOOST_CHECK_EQUAL( LinkImportedFootprints( schematic, footprints, NULL_REPORTER::GetInstance() ), 0 );

    for( const SCH_SYMBOL* symbol : symbols )
        BOOST_CHECK( symbol->GetField( FIELD_T::FOOTPRINT )->GetText().IsEmpty() );
}


BOOST_AUTO_TEST_CASE( MalformedFootprintIdsAreNotLinked )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();
    SCHEMATIC schematic( &project );
    schematic.Reset();
    SCH_SHEET_PATH path = schematic.Hierarchy().front();
    LIB_SYMBOL lib( wxS( "R" ), nullptr );
    const std::vector<wxString> ids = { wxString(), wxS( "bare_item" ), wxS( ":item" ),
                                       wxS( "cache:" ), wxS( "cache:R_0603" ) };
    std::map<wxString, wxString> footprints;
    std::vector<SCH_SYMBOL*> symbols;

    for( size_t i = 0; i < ids.size(); ++i )
    {
        SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, 1 );
        const wxString ref = wxString::Format( wxS( "R%zu" ), i + 1 );
        symbol->SetRef( &path, ref );
        path.LastScreen()->Append( symbol );
        symbols.push_back( symbol );
        footprints.emplace( ref, ids[i] );
    }

    BOOST_CHECK_EQUAL( LinkImportedFootprints( schematic, footprints, NULL_REPORTER::GetInstance() ), 1 );

    for( size_t i = 0; i + 1 < symbols.size(); ++i )
        BOOST_CHECK( symbols[i]->GetField( FIELD_T::FOOTPRINT )->GetText().IsEmpty() );

    BOOST_CHECK_EQUAL( symbols.back()->GetField( FIELD_T::FOOTPRINT )->GetText(), ids.back() );
}


BOOST_AUTO_TEST_CASE( DistinctUnitsOfOneComponentCanLink )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();
    SCHEMATIC schematic( &project );
    schematic.Reset();
    SCH_SHEET_PATH path = schematic.Hierarchy().front();
    LIB_SYMBOL lib( wxS( "Dual" ), nullptr );
    lib.SetUnitCount( 2, false );
    std::vector<SCH_SYMBOL*> symbols;

    for( int unit : { 1, 2 } )
    {
        SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, unit );
        symbol->SetRef( &path, wxS( "U1" ) );
        path.LastScreen()->Append( symbol );
        symbols.push_back( symbol );
    }

    const std::map<wxString, wxString> footprints = { { wxS( "U1" ), wxS( "cache:DUAL" ) } };
    BOOST_CHECK_EQUAL( LinkImportedFootprints( schematic, footprints, NULL_REPORTER::GetInstance() ), 2 );

    for( const SCH_SYMBOL* symbol : symbols )
        BOOST_CHECK_EQUAL( symbol->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "cache:DUAL" ) );
}


BOOST_AUTO_TEST_CASE( MultiUnitUserChoiceVetoesOtherUnits )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();
    SCHEMATIC schematic( &project );
    schematic.Reset();
    SCH_SHEET_PATH path = schematic.Hierarchy().front();
    LIB_SYMBOL lib( wxS( "Dual" ), nullptr );
    lib.SetUnitCount( 2, false );
    std::vector<SCH_SYMBOL*> symbols;

    for( int unit : { 1, 2 } )
    {
        SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, unit );
        symbol->SetRef( &path, wxS( "U1" ) );
        path.LastScreen()->Append( symbol );
        symbols.push_back( symbol );
    }

    symbols.front()->SetFootprintFieldText( wxS( "user:CHOSEN_DUAL" ) );
    const std::map<wxString, wxString> footprints = { { wxS( "U1" ), wxS( "cache:DUAL" ) } };
    BOOST_CHECK_EQUAL( LinkImportedFootprints( schematic, footprints, NULL_REPORTER::GetInstance() ), 0 );
    BOOST_CHECK_EQUAL( symbols.front()->GetField( FIELD_T::FOOTPRINT )->GetText(),
                       wxS( "user:CHOSEN_DUAL" ) );
    BOOST_CHECK( symbols.back()->GetField( FIELD_T::FOOTPRINT )->GetText().IsEmpty() );
}


BOOST_AUTO_TEST_CASE( AutomaticPairKeepsEveryNonemptyField )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();
    SCHEMATIC schematic( &project );
    schematic.Reset();
    SCH_SHEET_PATH path = schematic.Hierarchy().front();
    LIB_SYMBOL lib( wxS( "R" ), nullptr );
    std::vector<SCH_SYMBOL*> symbols;

    for( const wxString& ref : { wxS( "R1" ), wxS( "R2" ), wxS( "R3" ) } )
    {
        SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, 1 );
        symbol->SetRef( &path, ref );
        path.LastScreen()->Append( symbol );
        symbols.push_back( symbol );
    }

    symbols[0]->SetFootprintFieldText( wxS( "cache:USER_CHOICE" ) );
    symbols[1]->SetFootprintFieldText( wxS( "other:USER_CHOICE" ) );
    const std::map<wxString, wxString> ids = {
        { wxS( "R1" ), wxS( "cache:IMPORTED" ) }, { wxS( "R2" ), wxS( "cache:IMPORTED" ) },
        { wxS( "R3" ), wxS( "cache:IMPORTED" ) }
    };
    BOOST_CHECK_EQUAL( LinkImportedFootprints( schematic, ids, NULL_REPORTER::GetInstance(),
                                              IMPORTED_FOOTPRINT_LINK_POLICY::FILL_EMPTY_ONLY ), 1 );
    BOOST_CHECK_EQUAL( symbols[0]->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "cache:USER_CHOICE" ) );
    BOOST_CHECK_EQUAL( symbols[1]->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "other:USER_CHOICE" ) );
    BOOST_CHECK_EQUAL( symbols[2]->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "cache:IMPORTED" ) );
}


BOOST_AUTO_TEST_CASE( ConnectedMissingPadAbortsBeforeAnyFieldChanges )
{
    for( int noConnectKind : { 0, 1, 2 } )
    {
        Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
        PROJECT& project = Pgm().GetSettingsManager().Prj();
        SCHEMATIC schematic( &project );
        schematic.Reset();
        SCH_SHEET_PATH path = schematic.Hierarchy().front();
        SCH_SCREEN* screen = path.LastScreen();
        LIB_SYMBOL lib( wxS( "R" ), nullptr );
        SCH_PIN* libraryPin = new SCH_PIN( &lib );
        libraryPin->SetNumber( wxS( "1" ) );
        libraryPin->SetType( noConnectKind == 1 ? ELECTRICAL_PINTYPE::PT_NC
                                               : ELECTRICAL_PINTYPE::PT_PASSIVE );
        lib.AddDrawItem( libraryPin );
        std::vector<SCH_SYMBOL*> symbols;

        for( int i = 0; i < 2; ++i )
        {
            SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, 1 );
            symbol->SetRef( &path, wxString::Format( wxS( "R%d" ), i + 1 ) );
            symbol->SetPosition( VECTOR2I( i * 2000000, 0 ) );
            screen->Append( symbol );
            symbols.push_back( symbol );
        }

        const VECTOR2I firstPin = symbols[0]->GetPins( &path ).front()->GetPosition();
        const VECTOR2I secondPin = symbols[1]->GetPins( &path ).front()->GetPosition();
        SCH_LINE* wire = new SCH_LINE( firstPin, LAYER_WIRE );
        wire->SetEndPoint( secondPin );
        screen->Append( wire );

        if( noConnectKind == 2 )
            screen->Append( new SCH_NO_CONNECT( firstPin ) );

        const std::map<wxString, wxString> ids = {
            { wxS( "R1" ), wxS( "cache:R" ) }, { wxS( "R2" ), wxS( "cache:R" ) }
        };
        const std::map<wxString, std::set<wxString>> pads = {
            { wxS( "R1" ), {} }, { wxS( "R2" ), { wxS( "1" ) } }
        };
        BOOST_TEST_MESSAGE( "noConnectKind=" << noConnectKind );
        BOOST_CHECK_THROW( LinkImportedFootprints( schematic, ids, NULL_REPORTER::GetInstance(),
                                                   IMPORTED_FOOTPRINT_LINK_POLICY::FILL_EMPTY_ONLY, &pads ), IO_ERROR );

        for( const SCH_SYMBOL* symbol : symbols )
        {
            BOOST_CHECK( symbol->GetField( FIELD_T::FOOTPRINT )->GetText().IsEmpty() );
            BOOST_REQUIRE_EQUAL( symbol->GetPins( &path ).size(), 1U );
            BOOST_CHECK_EQUAL( symbol->GetPins( &path ).front()->GetNumber(), wxS( "1" ) );
        }
    }
}


BOOST_AUTO_TEST_CASE( LabeledSinglePinWithoutPadIsNotTreatedAsUnconnected )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();
    SCHEMATIC schematic( &project );
    schematic.Reset();
    SCH_SHEET_PATH path = schematic.Hierarchy().front();
    LIB_SYMBOL lib( wxS( "R" ), nullptr );
    SCH_PIN* libraryPin = new SCH_PIN( &lib );
    libraryPin->SetNumber( wxS( "1" ) );
    libraryPin->SetType( ELECTRICAL_PINTYPE::PT_PASSIVE );
    lib.AddDrawItem( libraryPin );
    SCH_SYMBOL* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, 1 );
    symbol->SetRef( &path, wxS( "R1" ) );
    path.LastScreen()->Append( symbol );
    path.LastScreen()->Append( new SCH_LABEL( symbol->GetPins( &path ).front()->GetPosition(), wxS( "SOURCE_NET" ) ) );
    const std::map<wxString, wxString> ids = { { wxS( "R1" ), wxS( "cache:R" ) } };
    const std::map<wxString, std::set<wxString>> pads = { { wxS( "R1" ), {} } };
    BOOST_CHECK_THROW( LinkImportedFootprints( schematic, ids, NULL_REPORTER::GetInstance(),
                                               IMPORTED_FOOTPRINT_LINK_POLICY::FILL_EMPTY_ONLY, &pads ), IO_ERROR );
    BOOST_CHECK( symbol->GetField( FIELD_T::FOOTPRINT )->GetText().IsEmpty() );
}


BOOST_AUTO_TEST_CASE( MissingUnconnectedPadsKeepSourcePinsAndDistinguishNoConnect )
{
    Pgm().GetSettingsManager().LoadProject( wxS( "" ) );
    PROJECT& project = Pgm().GetSettingsManager().Prj();
    SCHEMATIC schematic( &project );
    schematic.Reset();
    SCH_SHEET_PATH path = schematic.Hierarchy().front();
    SCH_SCREEN* screen = path.LastScreen();
    LIB_SYMBOL lib( wxS( "R" ), nullptr );
    SCH_PIN* libraryPin = new SCH_PIN( &lib );
    libraryPin->SetNumber( wxS( "1" ) );
    libraryPin->SetType( ELECTRICAL_PINTYPE::PT_PASSIVE );
    lib.AddDrawItem( libraryPin );
    LIB_SYMBOL noConnectLib( lib );
    BOOST_REQUIRE_EQUAL( noConnectLib.GetPinsByNumber( wxS( "1" ) ).size(), 1U );
    noConnectLib.GetPinsByNumber( wxS( "1" ) ).front()->SetType( ELECTRICAL_PINTYPE::PT_NC );
    std::vector<SCH_SYMBOL*> symbols;

    for( int i = 0; i < 2; ++i )
    {
        const LIB_SYMBOL& selectedLib = i == 0 ? noConnectLib : lib;
        SCH_SYMBOL* symbol = new SCH_SYMBOL( selectedLib, selectedLib.GetLibId(), &path, 1 );
        symbol->SetRef( &path, wxString::Format( wxS( "R%d" ), i + 1 ) );
        symbol->SetPosition( VECTOR2I( i * 2000000, 0 ) );
        screen->Append( symbol );
        symbols.push_back( symbol );
    }

    const std::map<wxString, wxString> ids = {
        { wxS( "R1" ), wxS( "cache:HOLE" ) }, { wxS( "R2" ), wxS( "cache:HOLE" ) }
    };
    const std::map<wxString, std::set<wxString>> pads = { { wxS( "R1" ), {} }, { wxS( "R2" ), {} } };
    WX_STRING_REPORTER reporter;
    BOOST_CHECK_EQUAL( LinkImportedFootprints( schematic, ids, reporter,
                                              IMPORTED_FOOTPRINT_LINK_POLICY::FILL_EMPTY_ONLY, &pads ), 2 );
    BOOST_CHECK( reporter.GetMessages().Contains( wxS( "IMPORT_LINK_NC_PIN: R1 pin 1" ) ) );
    BOOST_CHECK( reporter.GetMessages().Contains( wxS( "IMPORT_LINK_UNCONNECTED_PIN: R2 pin 1" ) ) );

    for( const SCH_SYMBOL* symbol : symbols )
    {
        BOOST_REQUIRE_EQUAL( symbol->GetPins( &path ).size(), 1U );
        BOOST_CHECK_EQUAL( symbol->GetPins( &path ).front()->GetNumber(), wxS( "1" ) );
        BOOST_CHECK_EQUAL( symbol->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "cache:HOLE" ) );
    }

    auto noConnects = screen->Items().OfType( SCH_NO_CONNECT_T );
    BOOST_CHECK_EQUAL( std::distance( noConnects.begin(), noConnects.end() ), 0 );
}


BOOST_AUTO_TEST_CASE( ProjectBoundMetadataKeepsKnownEmptyPadsAndRejectsDuplicateReferences )
{
    IMPORT_PROJ_PROPS::FOOTPRINT_LINK_DATA data;
    data.projectFile = wxS( "paired.kemi_pro" );
    data.ids = { { wxS( "H1" ), wxS( "cache:HOLE" ) }, { wxS( "R1" ), wxS( "cache:R" ) } };
    data.pads = { { wxS( "H1" ), {} }, { wxS( "R1" ), { wxS( "1" ), wxS( "2" ) } } };
    const auto decoded = IMPORT_PROJ_PROPS::SplitFootprintLinkData( IMPORT_PROJ_PROPS::JoinFootprintLinkData( data ) );
    BOOST_REQUIRE( decoded );
    BOOST_CHECK( decoded->ids == data.ids );
    BOOST_CHECK( decoded->pads == data.pads );
    BOOST_CHECK_EQUAL( decoded->projectFile, data.projectFile );
    wxArrayString duplicates;

    for( const wxString& field : { wxS( "import-footprints" ), wxS( "paired.kemi_pro" ), wxS( "2" ),
                                   wxS( "R1" ), wxS( "cache:R" ), wxS( "0" ),
                                   wxS( "R1" ), wxS( "cache:OTHER" ), wxS( "0" ) } )
    {
        duplicates.Add( field );
    }

    BOOST_CHECK( !IMPORT_PROJ_PROPS::SplitFootprintLinkData( IMPORT_PROJ_PROPS::JoinList( duplicates ) ) );
    data.pads.erase( wxS( "H1" ) );
    BOOST_CHECK( IMPORT_PROJ_PROPS::JoinFootprintLinkData( data ).IsEmpty() );
}


BOOST_AUTO_TEST_SUITE_END()
