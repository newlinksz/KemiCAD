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

// Regression: unzipping a converted archive must locate its single .kemi_pro project.

#include <boost/test/unit_test.hpp>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <project/project_archiver.h>
#include <reporter.h>

#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include <string>
#include <utility>
#include <vector>


namespace
{

struct TEMP_DIR
{
    wxString m_path;

    explicit TEMP_DIR( const wxString& aTag )
    {
        static int counter = 0;
        m_path = wxFileName::GetTempDir() + wxFileName::GetPathSeparator()
                 + wxString::Format( wxT( "kemi-unarchive-%s-%ld-%d" ), aTag,
                                     static_cast<long>( wxGetLocalTimeMillis().GetValue() ),
                                     ++counter );
        BOOST_REQUIRE( wxFileName::Mkdir( m_path, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    }

    ~TEMP_DIR() { wxFileName::Rmdir( m_path, wxPATH_RMDIR_RECURSIVE ); }

    wxString File( const wxString& aName ) const
    {
        return m_path + wxFileName::GetPathSeparator() + aName;
    }
};


void touch( const wxString& aPath, const std::string& aContent = "{}" )
{
    wxFFile fp( aPath, wxT( "wb" ) );
    BOOST_REQUIRE( fp.IsOpened() );
    fp.Write( aContent.data(), aContent.size() );
}


void writeZip( const wxString& aZip, const std::vector<std::pair<wxString, std::string>>& aEntries )
{
    wxFFileOutputStream out( aZip );
    BOOST_REQUIRE( out.IsOk() );
    wxZipOutputStream zip( out );

    for( const auto& [name, content] : aEntries )
    {
        if( name.EndsWith( wxT( "/" ) ) )
        {
            zip.PutNextDirEntry( name );
            continue;
        }

        zip.PutNextEntry( name );
        zip.Write( content.data(), content.size() );
    }

    BOOST_REQUIRE( zip.Close() );
}


wxString fileName( const wxString& aPath )
{
    return wxFileName( aPath ).GetFullName();
}

} // namespace


BOOST_AUTO_TEST_SUITE( KemiUnarchiveProject )


BOOST_AUTO_TEST_CASE( OnlyKemiProjectIsFound )
{
    TEMP_DIR dir( wxT( "kemi" ) );
    touch( dir.File( wxT( "board.kemi_pro" ) ) );
    touch( dir.File( wxT( "board.kemi_sch" ) ) );

    BOOST_CHECK_EQUAL( fileName( PROJECT_ARCHIVER::FindExtractedProjectFile( dir.m_path ) ),
                       wxString( wxT( "board.kemi_pro" ) ) );
}


BOOST_AUTO_TEST_CASE( OnlyKicadProjectIsFound )
{
    TEMP_DIR dir( wxT( "kicad" ) );
    touch( dir.File( wxT( "board.kicad_pro" ) ) );

    BOOST_CHECK_EQUAL( fileName( PROJECT_ARCHIVER::FindExtractedProjectFile( dir.m_path ) ),
                       wxString( wxT( "board.kicad_pro" ) ) );
}


BOOST_AUTO_TEST_CASE( KemiProjectPreferredOverKicad )
{
    TEMP_DIR dir( wxT( "both" ) );
    touch( dir.File( wxT( "board.kemi_pro" ) ) );
    touch( dir.File( wxT( "board.kicad_pro" ) ) );

    BOOST_CHECK_EQUAL( fileName( PROJECT_ARCHIVER::FindExtractedProjectFile( dir.m_path ) ),
                       wxString( wxT( "board.kemi_pro" ) ) );
}


BOOST_AUTO_TEST_CASE( TwoProjectsAreNotAutoOpened )
{
    TEMP_DIR kemi( wxT( "two-kemi" ) );
    touch( kemi.File( wxT( "a.kemi_pro" ) ) );
    touch( kemi.File( wxT( "b.kemi_pro" ) ) );
    BOOST_CHECK( PROJECT_ARCHIVER::FindExtractedProjectFile( kemi.m_path ).IsEmpty() );

    TEMP_DIR kicad( wxT( "two-kicad" ) );
    touch( kicad.File( wxT( "a.kicad_pro" ) ) );
    touch( kicad.File( wxT( "b.kicad_pro" ) ) );
    BOOST_CHECK( PROJECT_ARCHIVER::FindExtractedProjectFile( kicad.m_path ).IsEmpty() );
}


BOOST_AUTO_TEST_CASE( NoProjectOrMissingDir )
{
    TEMP_DIR dir( wxT( "none" ) );
    touch( dir.File( wxT( "readme.txt" ) ) );
    BOOST_CHECK( PROJECT_ARCHIVER::FindExtractedProjectFile( dir.m_path ).IsEmpty() );
    BOOST_CHECK( PROJECT_ARCHIVER::FindExtractedProjectFile( dir.File( wxT( "nope" ) ) ).IsEmpty() );
}


BOOST_AUTO_TEST_CASE( SingleTopFolderDetection )
{
    TEMP_DIR dir( wxT( "top" ) );

    const wxString single = dir.File( wxT( "single.zip" ) );
    writeZip( single, { { wxT( "demo_20261028/" ), "" },
                        { wxT( "demo_20261028/demo.kemi_pro" ), "{}" },
                        { wxT( "demo_20261028/sub/x.txt" ), "x" } } );
    BOOST_CHECK_EQUAL( PROJECT_ARCHIVER::FindSingleTopFolder( single ),
                       wxString( wxT( "demo_20261028" ) ) );

    const wxString flat = dir.File( wxT( "flat.zip" ) );
    writeZip( flat, { { wxT( "demo.kemi_pro" ), "{}" } } );
    BOOST_CHECK( PROJECT_ARCHIVER::FindSingleTopFolder( flat ).IsEmpty() );

    const wxString two = dir.File( wxT( "two.zip" ) );
    writeZip( two, { { wxT( "a/a.kemi_pro" ), "{}" }, { wxT( "b/b.kemi_pro" ), "{}" } } );
    BOOST_CHECK( PROJECT_ARCHIVER::FindSingleTopFolder( two ).IsEmpty() );
}


// End-to-end: a converter-style zip with one top folder, extracted with that folder stripped,
// must leave exactly one .kemi_pro at the top level of the target folder.
BOOST_AUTO_TEST_CASE( ConvertedZipLocatesUniqueKemiProject )
{
    TEMP_DIR dir( wxT( "e2e" ) );

    const wxString zipPath = dir.File( wxT( "demo_20261028.zip" ) );
    writeZip( zipPath, { { wxT( "demo_20261028/" ), "" },
                         { wxT( "demo_20261028/demo.kemi_pro" ), "{}" },
                         { wxT( "demo_20261028/demo.kemi_sch" ), "(kicad_sch)" },
                         { wxT( "demo_20261028/demo.kemi_pcb" ), "(kicad_pcb)" } } );

    const wxString top = PROJECT_ARCHIVER::FindSingleTopFolder( zipPath );
    BOOST_REQUIRE_EQUAL( top, wxString( wxT( "demo_20261028" ) ) );

    const wxString target = dir.File( wxT( "out" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( target, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    NULL_REPORTER reporter;
    BOOST_REQUIRE( PROJECT_ARCHIVER::Unarchive( zipPath,
                                                wxFileName::DirName( target ).GetPathWithSep(),
                                                reporter, top ) );

    const wxString project = PROJECT_ARCHIVER::FindExtractedProjectFile( target );
    BOOST_CHECK_EQUAL( fileName( project ), wxString( wxT( "demo.kemi_pro" ) ) );
    BOOST_CHECK( wxFileExists( project ) );
}


BOOST_AUTO_TEST_SUITE_END()