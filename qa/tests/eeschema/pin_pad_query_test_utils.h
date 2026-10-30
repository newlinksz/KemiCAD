/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Licensed under the GNU General Public License, version 3 or later.
 */

#ifndef PIN_PAD_QUERY_TEST_UTILS_H
#define PIN_PAD_QUERY_TEST_UTILS_H

#include <memory>
#include <set>
#include <vector>
#include <kiface_ids.h>
#include <kiway.h>
#include <lib_symbol.h>
#include <reporter.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <settings/settings_manager.h>
#include <wx/ffile.h>
#include <wx/filename.h>

namespace PIN_PAD_QUERY_TEST
{
struct FOOTPRINT_FACE : KIFACE
{
    bool OnKifaceStart( PGM_BASE*, int, KIWAY* ) override { return true; }
    void OnKifaceEnd() override {}
    void Reset() override {}
    wxWindow* CreateKiWindow( wxWindow*, int, KIWAY*, int ) override { return nullptr; }
    void GetActions( std::vector<TOOL_ACTION*>& ) const override {}

    static bool Query( const wxString& aFootprint, PROJECT*, std::set<wxString>& aPads,
                       wxString& aError )
    {
        aPads.clear();
        aError.clear();

        if( aFootprint == wxS( "Acceptance:Empty" ) )
            return true;

        if( aFootprint == wxS( "Acceptance:Missing" ) )
        {
            aPads = { wxS( "2" ) };
            return true;
        }

        if( aFootprint == wxS( "Acceptance:Good" ) )
        {
            aPads = { wxS( "1" ) };
            return true;
        }

        if( aFootprint == wxS( "Acceptance:Stacked" ) )
        {
            aPads = { wxS( "[2-1]" ), wxS( "2" ), wxS( "3" ) };
            return true;
        }

        aError = wxS( "Footprint could not be loaded: " ) + aFootprint;
        return false;
    }

    static void Legacy( const wxString& aFootprint, PROJECT* aProject,
                        std::set<wxString>& aPads )
    {
        wxString error;
        Query( aFootprint, aProject, aPads, error );
    }

    void* IfaceOrAddress( int aId ) override
    {
        if( aId == KIFACE_FOOTPRINT_PAD_NUMBERS )
            return reinterpret_cast<void*>( &Legacy );

        // The new query is appended after the legacy ID.  Express this relative to the old
        // public ID so this regression also compiles against the unfixed production headers.
        if( aId == KIFACE_FOOTPRINT_PAD_NUMBERS + 1 )
            return reinterpret_cast<void*>( &Query );

        return nullptr;
    }
};

struct TEST_KIWAY : KIWAY
{
    TEST_KIWAY() : KIWAY( KFCTL_CLI ) {}

    KIFACE* KiFACE( FACE_T aFace, bool = true ) override
    {
        return aFace == FACE_CVPCB ? &face : nullptr;
    }

    FOOTPRINT_FACE face;
};

struct REPORT : REPORTER
{
    REPORTER& Report( const wxString& aText, SEVERITY aSeverity ) override
    {
        messages += aText + wxS( "\n" );
        errors += aSeverity == RPT_SEVERITY_ERROR;
        warnings += aSeverity == RPT_SEVERITY_WARNING;
        return *this;
    }

    wxString messages;
    int errors = 0;
    int warnings = 0;
};

struct OUTPUT_FILE
{
    OUTPUT_FILE() : path( wxFileName::CreateTempFileName( wxS( "pin_pad_query_" ) ) ) {}
    ~OUTPUT_FILE() { wxRemoveFile( path ); }

    void Write( const wxString& aText )
    {
        wxFFile file( path, wxS( "w" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aText, wxConvUTF8 ) );
    }

    wxString Read() const
    {
        wxFFile file( path, wxS( "r" ) );
        wxString text;
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.ReadAll( &text, wxConvUTF8 ) );
        return text;
    }

    wxString path;
};

struct DESIGN
{
    DESIGN()
    {
        settings.LoadProject( wxEmptyString );
        schematic = std::make_unique<SCHEMATIC>( &settings.Prj() );
        schematic->Reset();
        SCH_SHEET* old = schematic->GetTopLevelSheet( 0 );
        auto* root = new SCH_SHEET( schematic.get() );
        root->SetScreen( new SCH_SCREEN( schematic.get() ) );
        schematic->AddTopLevelSheet( root );
        schematic->RemoveTopLevelSheet( old );
        delete old;
    }

    SCH_SYMBOL* Add( const wxString& aRef, const wxString& aFootprint, const VECTOR2I& aPosition )
    {
        LIB_SYMBOL lib( wxS( "PhysicalPin" ), nullptr );
        auto* pin = new SCH_PIN( &lib );
        pin->SetNumber( wxS( "1" ) );
        pin->SetType( ELECTRICAL_PINTYPE::PT_PASSIVE );
        pin->SetPosition( VECTOR2I( 0, 0 ) );
        lib.AddDrawItem( pin );
        SCH_SHEET_PATH path;
        path.push_back( &schematic->Root() );
        auto* symbol = new SCH_SYMBOL( lib, lib.GetLibId(), &path, 0, 0, aPosition );
        symbol->GetField( FIELD_T::REFERENCE )->SetText( aRef );
        symbol->SetFootprintFieldText( aFootprint );
        symbol->UpdatePins();
        schematic->RootScreen()->Append( symbol );
        return symbol;
    }

    SETTINGS_MANAGER settings;
    std::unique_ptr<SCHEMATIC> schematic;
};
} // namespace PIN_PAD_QUERY_TEST

#endif
