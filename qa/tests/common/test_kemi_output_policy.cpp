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
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <drawing_sheet/ds_data_model.h>
#include <kemi_format_profile.h>
#include <ki_exception.h>

#include <wx/filefn.h>
#include <wx/filename.h>

#include <array>

namespace
{
using KEMI_FORMAT::DOC;
using KEMI_FORMAT::FLAVOR;
using KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY;
using KEMI_FORMAT::OUTPUT_TARGET;

struct EXPECTED_DOCUMENT
{
    DOC         document;
    const char* nativeSuffix;
    const char* officialSuffix;
};

// Independent expectations catch accidental changes to the profile's extension table.
const std::array<EXPECTED_DOCUMENT, 9> documents = {{
        { DOC::PROJECT, "kemi_pro", "kicad_pro" },
        { DOC::PROJECT_LOCAL, "kemi_prl", "kicad_prl" },
        { DOC::JOBSET, "kemi_jobset", "kicad_jobset" },
        { DOC::SCHEMATIC, "kemi_sch", "kicad_sch" },
        { DOC::PCB, "kemi_pcb", "kicad_pcb" },
        { DOC::SYMBOL_LIB, "kemi_sym", "kicad_sym" },
        { DOC::FOOTPRINT, "kemi_mod", "kicad_mod" },
        { DOC::DRAWING_SHEET, "kemi_wks", "kicad_wks" },
        { DOC::DESIGN_RULES, "kemi_dru", "kicad_dru" },
}};


bool promoteOutput( const wxString& aStorage, const OUTPUT_TARGET& aTarget )
{
    if( aStorage != aTarget.FinalPath() && !wxRenameFile( aStorage, aTarget.FinalPath(), true ) )
        THROW_IO_ERROR( wxS( "Could not promote the test output to its final target." ) );

    return true;
}


struct KEMI_OUTPUT_POLICY_FIXTURE
{
    KEMI_OUTPUT_POLICY_FIXTURE() :
            directory( wxS( "kemi-output-policy" ) ),
            policy( directory.PathStr() )
    {}

    KI_TEST::SCOPED_TEMP_DIR directory;
    KEMI_ONLY_OUTPUT_POLICY policy;
};
}


BOOST_FIXTURE_TEST_SUITE( KemiOutputPolicy, KEMI_OUTPUT_POLICY_FIXTURE )

BOOST_AUTO_TEST_CASE( MapsEveryDocumentToNativeTarget )
{
    BOOST_CHECK_EQUAL( KEMI_FORMAT::AllProfiles().size(), documents.size() );

    for( const EXPECTED_DOCUMENT& expected : documents )
    {
        BOOST_TEST_CONTEXT( expected.nativeSuffix )
        {
            const OUTPUT_TARGET target = policy.Target( expected.document, wxS( "document.tmp" ) );
            BOOST_CHECK( target.Document() == expected.document );
            BOOST_CHECK( target.Flavor() == FLAVOR::KEMI );
            BOOST_CHECK_EQUAL( target.FinalPath(), directory.ChildPathStr(
                    wxS( "document." ) + wxString( expected.nativeSuffix ) ) );
            BOOST_CHECK_EQUAL( policy.Target( expected.document, target.FinalPath() ).FinalPath(),
                               target.FinalPath() );
        }
    }

    BOOST_CHECK( policy.Manifest().empty() );
}


BOOST_AUTO_TEST_CASE( PrettyDirectoryKeepsItsNameAndUsesNativeFootprints )
{
    const wxString pretty = directory.CreateChildDirStr( wxS( "Parts.pretty" ) );
    const OUTPUT_TARGET target = policy.Target( DOC::FOOTPRINT, wxS( "Parts.pretty/Pad" ) );
    BOOST_CHECK_EQUAL( target.FinalPath(), wxFileName( pretty, wxS( "Pad.kemi_mod" ) ).GetFullPath() );
    BOOST_CHECK( target.Document() == DOC::FOOTPRINT );
    BOOST_CHECK( target.Flavor() == FLAVOR::KEMI );
    BOOST_CHECK( !wxDirExists( directory.ChildPathStr( wxS( "Parts.kemi_pretty" ) ) ) );
}


BOOST_AUTO_TEST_CASE( RejectsOfficialTargetsBeforeInvokingWriter )
{
    int calls = 0;
    DS_DATA_MODEL model;
    model.SetDefaultLayout();
    const auto writer = [&]( const wxString& aStorage, const OUTPUT_TARGET& aTarget )
    {
        ++calls;
        model.Save( aStorage, aTarget.Flavor() );
        return promoteOutput( aStorage, aTarget );
    };

    for( const EXPECTED_DOCUMENT& document : documents )
    {
        for( const EXPECTED_DOCUMENT& official : documents )
        {
            for( const wxString& suffix : { wxString( official.officialSuffix ),
                                           wxString( official.officialSuffix ).Upper() } )
            {
                const wxString name = wxS( "blocked." ) + suffix;
                BOOST_TEST_CONTEXT( document.nativeSuffix << ": " << name )
                {
                    BOOST_CHECK_THROW( policy.Target( document.document, name ), IO_ERROR );
                    BOOST_CHECK_THROW( policy.Write( document.document, name, writer ), IO_ERROR );
                    BOOST_CHECK( !wxFileExists( directory.ChildPathStr( name ) ) );
                    BOOST_CHECK( !wxFileExists( directory.ChildPathStr(
                            wxS( "blocked." ) + wxString( document.nativeSuffix ) ) ) );
                }
            }
        }
    }

    const wxString storage = directory.ChildPathStr( wxS( "blocked.KICAD_WKS.tmp" ) );
    BOOST_CHECK_THROW( policy.Write( DOC::DRAWING_SHEET, wxS( "drawing" ), writer, storage ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( storage ) );
    BOOST_CHECK_EQUAL( calls, 0 );
    BOOST_CHECK( policy.Manifest().empty() );
}


BOOST_AUTO_TEST_CASE( TemporaryPathsKeepPolicyFlavor )
{
    for( const EXPECTED_DOCUMENT& document : documents )
    {
        for( const wxString& name : { wxS( "output.tmp" ), wxS( "extensionless" ),
                                     wxS( "output.other" ) } )
        {
            const wxString path = directory.ChildPathStr( name );
            BOOST_TEST_CONTEXT( document.nativeSuffix << ": " << name )
            {
                BOOST_CHECK( KEMI_FORMAT::FlavorForPath( document.document, path, policy )
                             == FLAVOR::KEMI );
                BOOST_CHECK( KEMI_FORMAT::FlavorForPath( document.document, path )
                             == FLAVOR::LEGACY );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( TemporaryDrawingSheetUsesExplicitFlavorAndReopens )
{
    DS_DATA_MODEL model;
    model.SetDefaultLayout();
    model.SetLeftMargin( 13.25 );
    const wxString storage = directory.ChildPathStr( wxS( "drawing.tmp" ) );
    const wxString final = directory.ChildPathStr( wxS( "drawing.kemi_wks" ) );

    policy.Write( DOC::DRAWING_SHEET, wxS( "drawing" ),
            [&]( const wxString& aStorage, const OUTPUT_TARGET& aTarget )
            {
                model.Save( aStorage, aTarget.Flavor() );
                const std::string bytes = KI_TEST::LoadStringData( aStorage );
                BOOST_CHECK( bytes.find( "(kemi_wks" ) == 0 );
                BOOST_CHECK( bytes.find( "(format_version 1)" ) != std::string::npos );
                BOOST_CHECK( policy.Manifest().empty() );
                BOOST_CHECK( !wxFileExists( aTarget.FinalPath() ) );
                DS_DATA_MODEL temporary;
                wxString error;

                if( !temporary.LoadDrawingSheet( aStorage, &error ) )
                    THROW_IO_ERROR( error );

                BOOST_CHECK_EQUAL( temporary.GetCount(), model.GetCount() );
                BOOST_CHECK_EQUAL( temporary.GetLeftMargin(), 13.25 );
                return promoteOutput( aStorage, aTarget );
            }, storage );

    BOOST_CHECK( !wxFileExists( storage ) );
    DS_DATA_MODEL reopened;
    wxString error;
    BOOST_REQUIRE_MESSAGE( reopened.LoadDrawingSheet( final, &error ), error );
    BOOST_CHECK_EQUAL( reopened.GetCount(), model.GetCount() );
    BOOST_CHECK_EQUAL( reopened.GetLeftMargin(), 13.25 );
    BOOST_REQUIRE_EQUAL( policy.Manifest().size(), 1 );
    BOOST_CHECK_EQUAL( policy.Manifest().front().FinalPath(), final );
    BOOST_CHECK( policy.Manifest().front().Document() == DOC::DRAWING_SHEET );
    BOOST_CHECK( policy.Manifest().front().Flavor() == FLAVOR::KEMI );
}


BOOST_AUTO_TEST_CASE( ManifestRecordsOnlySuccessfulFinalTargetsOnce )
{
    DS_DATA_MODEL model;
    model.SetDefaultLayout();
    const wxString storage = directory.ChildPathStr( wxS( "retry.tmp" ) );
    const wxString final = directory.ChildPathStr( wxS( "retry.kemi_wks" ) );

    BOOST_CHECK_THROW( policy.Write( DOC::DRAWING_SHEET, wxS( "retry" ),
            [&]( const wxString& aStorage, const OUTPUT_TARGET& aTarget ) -> bool
            {
                model.Save( aStorage, aTarget.Flavor() );
                THROW_IO_ERROR( wxS( "Simulated promotion failure." ) );
            }, storage ), IO_ERROR );
    BOOST_CHECK( wxFileExists( storage ) );
    BOOST_CHECK( !wxFileExists( final ) );
    BOOST_CHECK( policy.Manifest().empty() );
    BOOST_CHECK_THROW( policy.Write( DOC::DRAWING_SHEET, wxS( "retry" ),
            []( const wxString&, const OUTPUT_TARGET& ) { return false; } ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( final ) );
    BOOST_CHECK( policy.Manifest().empty() );

    const auto writer = [&]( const wxString& aStorage, const OUTPUT_TARGET& aTarget )
    {
        model.Save( aStorage, aTarget.Flavor() );
        return promoteOutput( aStorage, aTarget );
    };
    policy.Write( DOC::DRAWING_SHEET, wxS( "retry" ), writer, storage );
    model.SetLeftMargin( 17.5 );
    policy.Write( DOC::DRAWING_SHEET, final, writer, storage );
#ifdef __WXMSW__
    // Windows path aliases must not add a second manifest entry for the same output.
    policy.Write( DOC::DRAWING_SHEET, wxS( "RETRY.kemi_wks" ), writer, storage );
#endif
    BOOST_REQUIRE_EQUAL( policy.Manifest().size(), 1 );
    BOOST_CHECK_EQUAL( policy.Manifest().front().FinalPath(), final );
    BOOST_CHECK( !wxFileExists( storage ) );
    policy.Write( DOC::DRAWING_SHEET, wxS( "second" ), writer );
    BOOST_REQUIRE_EQUAL( policy.Manifest().size(), 2 );
    BOOST_CHECK_EQUAL( policy.Manifest().back().FinalPath(),
                       directory.ChildPathStr( wxS( "second.kemi_wks" ) ) );
    BOOST_CHECK( wxFileExists( policy.Manifest().back().FinalPath() ) );
    DS_DATA_MODEL reopened;
    wxString error;
    BOOST_REQUIRE_MESSAGE( reopened.LoadDrawingSheet( final, &error ), error );
    BOOST_CHECK_EQUAL( reopened.GetLeftMargin(), 17.5 );
}


BOOST_AUTO_TEST_CASE( RejectsEscapingTargetsAndWrongDocumentExtensions )
{
    const wxString outside = directory.CreateChildDirStr( wxS( "outside" ) );
    const wxString inside = directory.CreateChildDirStr( wxS( "inside" ) );
    KEMI_ONLY_OUTPUT_POLICY nested( inside );
    BOOST_CHECK_THROW( nested.Target( DOC::PCB, wxS( "../escaped" ) ), IO_ERROR );
    BOOST_CHECK_THROW( nested.Target( DOC::PCB,
                                     wxFileName( outside, wxS( "escaped.kemi_pcb" ) ).GetFullPath() ),
                       IO_ERROR );

    int calls = 0;
    const auto writer = [&]( const wxString&, const OUTPUT_TARGET& )
    {
        ++calls;
        return true;
    };
    BOOST_CHECK_THROW( nested.Write( DOC::PCB, wxS( "../escaped" ), writer ), IO_ERROR );
    BOOST_CHECK( !wxFileExists( directory.ChildPathStr( wxS( "escaped.kemi_pcb" ) ) ) );

    for( size_t i = 0; i < documents.size(); ++i )
    {
        const EXPECTED_DOCUMENT& document = documents[i];
        const wxString wrong = wxS( "wrong." )
                               + wxString( documents[( i + 1 ) % documents.size()].nativeSuffix );
        BOOST_TEST_CONTEXT( document.nativeSuffix )
        {
            BOOST_CHECK_THROW( policy.Target( document.document, wrong ), IO_ERROR );
            BOOST_CHECK_THROW( policy.Write( document.document, wrong, writer ), IO_ERROR );
            BOOST_CHECK_THROW( policy.Write( document.document, wxS( "valid" ), writer,
                                            directory.ChildPathStr( wrong ) ), IO_ERROR );
        }
    }

    BOOST_CHECK_EQUAL( calls, 0 );
    BOOST_CHECK( policy.Manifest().empty() );
    BOOST_CHECK( nested.Manifest().empty() );
}

BOOST_AUTO_TEST_SUITE_END()
