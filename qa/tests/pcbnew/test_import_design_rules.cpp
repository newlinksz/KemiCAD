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

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <reporter.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <import_design_rules.h>


BOOST_AUTO_TEST_SUITE( ImportDesignRules )


// A board with no name has nowhere to put rules but a hidden file, which must not be written
BOOST_AUTO_TEST_CASE( NamelessBoardWritesNothing )
{
    wxFileName dir = wxFileName::DirName( wxStandardPaths::Get().GetTempDir() );
    dir.AppendDir( wxString::Format( wxS( "import_design_rules-qa-%lu" ), wxGetProcessId() ) );
    dir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    wxFileName board( dir.GetPath(), wxEmptyString, wxS( "kicad_pcb" ) );
    wxFileName hidden( dir.GetPath(), wxEmptyString, wxS( "kicad_dru" ) );

    BOOST_CHECK( !SaveImportedDesignRules( wxS( "(version 1)" ), board, NULL_REPORTER::GetInstance() ) );
    BOOST_CHECK( !hidden.FileExists() );
}


// Rules land beside the board as a Kemi container; no official sidecar and no overwrite
BOOST_AUTO_TEST_CASE( WritesKemiRulesOnly )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "import_design_rules-kemi" ) );

    wxFileName board( dir.ChildPathStr( wxS( "imported.kicad_pcb" ) ) );
    wxFileName kemi( dir.ChildPathStr( wxS( "imported.kemi_dru" ) ) );
    wxFileName legacy( dir.ChildPathStr( wxS( "imported.kicad_dru" ) ) );

    BOOST_CHECK( SaveImportedDesignRules( wxS( "(version 1)" ), board, NULL_REPORTER::GetInstance() ) );
    BOOST_CHECK( kemi.FileExists() );
    BOOST_CHECK( !legacy.FileExists() );

    const wxString first = KI_TEST::LoadStringData( kemi.GetFullPath() );
    BOOST_CHECK( first.Contains( wxS( "(version 1)" ) ) );
    BOOST_CHECK( !first.StartsWith( wxS( "(version" ) ) );

    // A second import must keep the user's existing file
    BOOST_CHECK( !SaveImportedDesignRules( wxS( "(version 2)" ), board, NULL_REPORTER::GetInstance() ) );
    BOOST_CHECK( KI_TEST::LoadStringData( kemi.GetFullPath() ) == first );
}

BOOST_AUTO_TEST_SUITE_END()
