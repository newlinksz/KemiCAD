/*
 * This file is part of KemiCAD, based on KiCad, a free EDA CAD application.
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

// Regression: user-facing "file is too new" messages must only ever show the KemiCAD
// version, never the upstream KiCad version (e.g. "10.99"), and saved files must carry
// the KemiCAD brand version in (generator_version ...).

#include <brand.h>
#include <kemi_format_profile.h>
#include <ki_exception.h>
#include <boost/test/unit_test.hpp>


BOOST_AUTO_TEST_SUITE( KemiGeneratorVersion )

BOOST_AUTO_TEST_CASE( GeneratorVersionIsBrandVersion )
{
    BOOST_CHECK_EQUAL( KEMI_FORMAT::GeneratorVersion(), wxString( BRAND_VERSION_W ) );
}

BOOST_AUTO_TEST_CASE( ClassifiesGeneratorVersions )
{
    BOOST_CHECK( KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "0.9.7" ) ) );
    BOOST_CHECK( KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "1.0.1" ) ) );
    BOOST_CHECK( KEMI_FORMAT::IsKemiGeneratorVersion( wxString( BRAND_VERSION_W ) ) );

    BOOST_CHECK( !KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "10.99" ) ) );
    BOOST_CHECK( !KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "9.0" ) ) );
    BOOST_CHECK( !KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "test" ) ) );
    BOOST_CHECK( !KEMI_FORMAT::IsKemiGeneratorVersion( wxEmptyString ) );
    BOOST_CHECK( !KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "1" ) ) );
}

BOOST_AUTO_TEST_CASE( RejectsMalformedGeneratorVersions )
{
    for( const wxChar* bad : { wxS( "1.0.1-rc1" ), wxS( "1.0.1 " ), wxS( " 1.0.1" ),
                               wxS( "v1.0.1" ), wxS( "1.0.1.2" ), wxS( "01.0.1" ),
                               wxS( "1.00.1" ), wxS( "1..1" ), wxS( "1.0." ), wxS( ".1.0" ),
                               wxS( "-1.0.1" ), wxS( "+1.0.1" ), wxS( "1.0.x" ),
                               wxS( "KemiCAD 1.0.1" ), wxS( "1,0,1" ),
                               wxS( "1234567890.0.0" ) } )
    {
        BOOST_CHECK_MESSAGE( !KEMI_FORMAT::IsKemiGeneratorVersion( bad ), bad );
    }
}

BOOST_AUTO_TEST_CASE( OldUpstreamLowVersionsAreNotKemi )
{
    // Upstream KiCad writes MAJOR.MINOR; the old major<5 heuristic wrongly accepted these.
    for( const wxChar* up : { wxS( "4.0" ), wxS( "5.1" ), wxS( "6.0" ), wxS( "7.0" ),
                              wxS( "8.0" ), wxS( "0.9" ), wxS( "1.0" ) } )
    {
        BOOST_CHECK_MESSAGE( !KEMI_FORMAT::IsKemiGeneratorVersion( up ), up );
    }
}

BOOST_AUTO_TEST_CASE( FutureKemiMajorsAreKemi )
{
    BOOST_CHECK( KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "5.0.0" ) ) );
    BOOST_CHECK( KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "12.3.4" ) ) );
    BOOST_CHECK( KEMI_FORMAT::IsKemiGeneratorVersion( wxS( "0.0.0" ) ) );
}

BOOST_AUTO_TEST_CASE( VersionShownOnlyWithKemiOrigin )
{
    BOOST_CHECK( KEMI_FORMAT::ShowKemiGeneratorVersion( wxS( "1.2.0" ), true ) );
    BOOST_CHECK( !KEMI_FORMAT::ShowKemiGeneratorVersion( wxS( "1.2.0" ), false ) );
    BOOST_CHECK( !KEMI_FORMAT::ShowKemiGeneratorVersion( wxS( "10.99" ), true ) );
}

BOOST_AUTO_TEST_CASE( FutureFormatErrorShowsKemiVersionOnly )
{
    FUTURE_FORMAT_ERROR kemi( wxS( "99991231" ), wxS( "1.2.0" ), true );
    BOOST_CHECK( kemi.Problem().Contains( wxS( "KemiCAD 1.2.0" ) ) );
    BOOST_CHECK( kemi.Problem().Contains( wxS( "99991231" ) ) );

    // Same value but origin unclear (no kemi_* root): generic text only.
    FUTURE_FORMAT_ERROR unclear( wxS( "99991231" ), wxS( "1.2.0" ) );
    BOOST_CHECK( !unclear.Problem().Contains( wxS( "1.2.0" ) ) );
    BOOST_CHECK( unclear.Problem().Contains( wxS( "upgrade KemiCAD" ) ) );

    // Kemi root but old upstream / malformed generator: generic text.
    FUTURE_FORMAT_ERROR oldUp( wxS( "99991231" ), wxS( "5.1" ), true );
    BOOST_CHECK( !oldUp.Problem().Contains( wxS( "5.1" ) ) );
    FUTURE_FORMAT_ERROR bad( wxS( "99991231" ), wxS( "1.0.1-rc1" ), true );
    BOOST_CHECK( !bad.Problem().Contains( wxS( "rc1" ) ) );

    // Wrapping keeps the origin.
    PARSE_ERROR         inner( wxS( "syntax" ), __FILE__, __FUNCTION__, __LINE__, wxS( "f" ), "", 1, 0 );
    FUTURE_FORMAT_ERROR wrapped( kemi, wxS( "1" ) );
    BOOST_CHECK( wrapped.kemiOrigin );
    BOOST_CHECK( wrapped.Problem().Contains( wxS( "KemiCAD 1.2.0" ) ) );
    FUTURE_FORMAT_ERROR fresh( inner, wxS( "99991231" ), wxS( "2.0.0" ), true );
    BOOST_CHECK( fresh.Problem().Contains( wxS( "KemiCAD 2.0.0" ) ) );

    // Upstream / old-KemiCAD generator values must not leak into the message.
    FUTURE_FORMAT_ERROR upstream( wxS( "99991231" ), wxS( "10.99" ) );
    BOOST_CHECK( upstream.Problem().Contains( wxS( "KemiCAD" ) ) );
    BOOST_CHECK( !upstream.Problem().Contains( wxS( "10.99" ) ) );
    BOOST_CHECK( !upstream.Problem().Contains( wxS( "KiCad" ) ) );

    FUTURE_FORMAT_ERROR none( wxS( "99991231" ), wxEmptyString );
    BOOST_CHECK( none.Problem().Contains( wxS( "upgrade KemiCAD" ) ) );
}

BOOST_AUTO_TEST_CASE( NewerFormatVersionRejectedWithKemiWording )
{
    const wxString err = KEMI_FORMAT::CheckFormatVersion( KEMI_FORMAT::DOC::PCB,
                                                          KEMI_FORMAT::FORMAT_VERSION + 1 );
    BOOST_CHECK( !err.IsEmpty() );
    BOOST_CHECK( err.Contains( wxS( "upgrade KemiCAD" ) ) );
    BOOST_CHECK( !err.Contains( wxS( "KiCad" ) ) );

    BOOST_CHECK( KEMI_FORMAT::CheckFormatVersion( KEMI_FORMAT::DOC::PCB,
                                                  KEMI_FORMAT::FORMAT_VERSION ).IsEmpty() );
}

BOOST_AUTO_TEST_SUITE_END()