/*
 * This file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <boost/test/unit_test.hpp>
#include <board.h>
#include <drc/rule_editor/drc_re_rule_loader.h>
#include <kemi_format_profile.h>
#include <ki_exception.h>
#include <pcb_io/pads/pcb_io_pads.h>
#include <pcb_io/pads/pcb_io_pads_binary.h>
#include <pcbnew_utils/board_file_utils.h>
#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <filesystem>
#include <map>
#include <string>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>


namespace
{
void WriteText( const wxString& aPath, const wxString& aText );

using DIRECTORY_SNAPSHOT = std::map<std::string, std::string>;


wxString StageSample( const KI_TEST::SCOPED_TEMP_DIR& aSource, const wxString& aRelativePath )
{
    const wxString sample = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() )
                            + wxS( "plugins/pads/" ) + aRelativePath;
    const wxString staged = aSource.ChildPathStr( wxFileName( sample ).GetFullName() );

    BOOST_REQUIRE( wxCopyFile( sample, staged ) );

    // The shared fixture spells its pair in a layout the ASCII parser skips, so stage the inline
    // DIFFPAIR form the parser reads to make the import carry a real rule
    if( wxFileName( staged ).GetExt().Lower() == wxS( "asc" ) )
    {
        wxString text = KI_TEST::LoadStringData( staged );

        if( text.Replace( wxS( "\nDIFF_PAIR1" ), wxS( "\nDIFFPAIR DIFF_PAIR1 DIFF_P DIFF_N 10.0 5.0" ) ) > 0 )
            WriteText( staged, text );
    }

    return staged;
}


DIRECTORY_SNAPSHOT SnapshotDirectory( const std::filesystem::path& aDirectory )
{
    DIRECTORY_SNAPSHOT snapshot;

    for( const auto& entry : std::filesystem::recursive_directory_iterator( aDirectory ) )
    {
        if( entry.is_regular_file() )
        {
            snapshot.emplace( entry.path().lexically_relative( aDirectory ).generic_string(),
                              KI_TEST::LoadStringData( wxString::FromUTF8( entry.path().string() ) ) );
        }
    }

    return snapshot;
}


void CheckNoOfficialFiles( const std::filesystem::path& aDirectory )
{
    for( const auto& entry : std::filesystem::recursive_directory_iterator( aDirectory ) )
    {
        BOOST_CHECK_MESSAGE(
                !wxString::FromUTF8( entry.path().extension().string() ).Lower().StartsWith( wxS( ".kicad_" ) ),
                "Unexpected official KiCad file: " << entry.path().string() );
    }
}


void WriteText( const wxString& aPath, const wxString& aText )
{
    wxFFile file( aPath, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE( file.Write( aText, wxConvUTF8 ) );
    BOOST_REQUIRE( file.Close() );
}


template<typename PLUGIN>
void CheckImportedRules( const wxString& aRelativePath )
{
    KI_TEST::SCOPED_TEMP_DIR source( wxS( "kemi-pads-source" ) );
    KI_TEST::SCOPED_TEMP_DIR output( wxS( "kemi-pads-output" ) );
    const wxString input = StageSample( source, aRelativePath );
    const DIRECTORY_SNAPSHOT original = SnapshotDirectory( source.Path() );
    PLUGIN plugin;

    std::unique_ptr<BOARD> board = plugin.LoadBoard( input );
    BOOST_REQUIRE( board );
    BOOST_CHECK( SnapshotDirectory( source.Path() ) == original );

    const wxString rules = plugin.GetImportedDesignRules();
    BOOST_REQUIRE( !rules.IsEmpty() );

    DRC_RULE_LOADER memoryLoader;
    const auto memoryEntries = memoryLoader.LoadFromString( rules, wxS( "imported.kicad_dru" ) );
    BOOST_REQUIRE_MESSAGE( memoryLoader.GetLoadError().IsEmpty(), memoryLoader.GetLoadError() );
    BOOST_REQUIRE( !memoryEntries.empty() );

    KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY policy( output.PathStr() );
    plugin.WriteImportedDesignRules( policy, wxS( "project" ) );

    const wxString target = output.ChildPathStr( wxS( "project.kemi_dru" ) );
    BOOST_REQUIRE( wxFileExists( target ) );
    BOOST_REQUIRE_EQUAL( policy.Manifest().size(), 1 );
    BOOST_CHECK( policy.Manifest().front().Document() == KEMI_FORMAT::DOC::DESIGN_RULES );
    BOOST_CHECK_EQUAL( policy.Manifest().front().FinalPath(), target );

    DRC_RULE_LOADER fileLoader;
    const auto entries = fileLoader.LoadFile( target );
    BOOST_REQUIRE_MESSAGE( fileLoader.GetLoadError().IsEmpty(), fileLoader.GetLoadError() );
    BOOST_REQUIRE_EQUAL( entries.size(), memoryEntries.size() );

    for( size_t i = 0; i < entries.size(); ++i )
        BOOST_CHECK_EQUAL( entries[i].ruleName, memoryEntries[i].ruleName );

    const DIRECTORY_SNAPSHOT written = SnapshotDirectory( output.Path() );
    BOOST_CHECK_EQUAL( written.size(), 1 );
    BOOST_CHECK_THROW( plugin.WriteImportedDesignRules( policy, wxS( "project" ) ), IO_ERROR );
    BOOST_CHECK( SnapshotDirectory( output.Path() ) == written );
    BOOST_CHECK_EQUAL( policy.Manifest().size(), 1 );
    BOOST_CHECK_THROW( plugin.WriteImportedDesignRules( policy, wxS( "project.kicad_dru" ) ), IO_ERROR );
    BOOST_CHECK( SnapshotDirectory( output.Path() ) == written );
    BOOST_CHECK_EQUAL( policy.Manifest().size(), 1 );
    BOOST_CHECK_EQUAL( plugin.GetImportedDesignRules(), rules );
    BOOST_CHECK( SnapshotDirectory( source.Path() ) == original );
    CheckNoOfficialFiles( source.Path() );
    CheckNoOfficialFiles( output.Path() );
}


template<typename PLUGIN>
void CheckReusedImporter( const wxString& aRulesSample, const wxString& aEmptySample )
{
    KI_TEST::SCOPED_TEMP_DIR source( wxS( "kemi-pads-reuse-source" ) );
    KI_TEST::SCOPED_TEMP_DIR output( wxS( "kemi-pads-reuse-output" ) );
    const wxString input = StageSample( source, aRulesSample );
    const wxString emptyInput = StageSample( source, aEmptySample );
    const DIRECTORY_SNAPSHOT original = SnapshotDirectory( source.Path() );
    const wxString target = output.ChildPathStr( wxS( "project.kemi_dru" ) );
    WriteText( target, wxS( "(kemi_dru (format_version 1) (version 2)\n"
                           "(rule \"main-board\" (constraint clearance (min 0.3mm))))\n" ) );
    const DIRECTORY_SNAPSHOT existingRules = SnapshotDirectory( output.Path() );
    KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY policy( output.PathStr() );
    PLUGIN plugin;

    std::unique_ptr<BOARD> board = plugin.LoadBoard( input );
    BOOST_REQUIRE( board );
    BOOST_REQUIRE( !plugin.GetImportedDesignRules().IsEmpty() );

    // An append must neither retain a previous import's rules nor replace the main board's rules.
    BOARD destination;
    destination.SetFileName( output.ChildPathStr( wxS( "project.kemi_pcb" ) ) );
    plugin.LoadAndAppendBoard( input, destination );
    BOOST_CHECK( plugin.GetImportedDesignRules().IsEmpty() );
    plugin.WriteImportedDesignRules( policy, wxS( "project" ) );
    BOOST_CHECK( SnapshotDirectory( output.Path() ) == existingRules );
    BOOST_CHECK( SnapshotDirectory( source.Path() ) == original );
    BOOST_CHECK( policy.Manifest().empty() );

    board = plugin.LoadBoard( input );
    BOOST_REQUIRE( board );
    BOOST_REQUIRE( !plugin.GetImportedDesignRules().IsEmpty() );
    board = plugin.LoadBoard( emptyInput );
    BOOST_REQUIRE( board );
    BOOST_CHECK( plugin.GetImportedDesignRules().IsEmpty() );
    plugin.WriteImportedDesignRules( policy, wxS( "empty" ) );
    BOOST_CHECK( SnapshotDirectory( output.Path() ) == existingRules );
    BOOST_CHECK( SnapshotDirectory( source.Path() ) == original );
    BOOST_CHECK( policy.Manifest().empty() );
    BOOST_CHECK_THROW( plugin.WriteImportedDesignRules( policy, wxS( "empty.kicad_dru" ) ), IO_ERROR );

    board = plugin.LoadBoard( input );
    BOOST_REQUIRE( board );
    BOOST_REQUIRE( !plugin.GetImportedDesignRules().IsEmpty() );
    const wxString missing = source.ChildPathStr( wxS( "missing." ) + wxFileName( input ).GetExt() );
    BOOST_CHECK_THROW( plugin.LoadBoard( missing ), IO_ERROR );
    BOOST_CHECK( plugin.GetImportedDesignRules().IsEmpty() );
    plugin.WriteImportedDesignRules( policy, wxS( "failed" ) );
    BOOST_CHECK( SnapshotDirectory( output.Path() ) == existingRules );
    BOOST_CHECK( SnapshotDirectory( source.Path() ) == original );
    BOOST_CHECK( policy.Manifest().empty() );
    CheckNoOfficialFiles( source.Path() );
    CheckNoOfficialFiles( output.Path() );
}
} // namespace


BOOST_AUTO_TEST_SUITE( KemiPadsRules )

// KEMI-PADS-001: ASCII import preserves source bytes and persists retained rules through the policy.
BOOST_AUTO_TEST_CASE( AsciiImportKeepsSourceAndWritesKemiRules )
{
    CheckImportedRules<PCB_IO_PADS>( wxS( "synthetic_constraints.asc" ) );
}


// KEMI-PADS-002: Binary import has the same source isolation and Kemi rules round-trip contract.
BOOST_AUTO_TEST_CASE( BinaryImportKeepsSourceAndWritesKemiRules )
{
    CheckImportedRules<PCB_IO_PADS_BINARY>( wxS( "Ems4_Rev2/Ems4_Rev2.pcb" ) );
}


// KEMI-PADS-003: Reused ASCII importers cannot persist stale, appended, empty or failed import rules.
BOOST_AUTO_TEST_CASE( AsciiReuseAppendAndFailureClearRules )
{
    CheckReusedImporter<PCB_IO_PADS>( wxS( "synthetic_constraints.asc" ), wxS( "synthetic_testpoint.asc" ) );
}


// KEMI-PADS-004: Reused binary importers preserve the main board's rules across the same boundaries.
BOOST_AUTO_TEST_CASE( BinaryReuseAppendAndFailureClearRules )
{
    CheckReusedImporter<PCB_IO_PADS_BINARY>( wxS( "Ems4_Rev2/Ems4_Rev2.pcb" ),
                                          wxS( "padstack_rows/padstack-v2022.pcb" ) );
}


// KEMI-PADS-005: Explicit legacy targets are rejected before an existing file can be overwritten.
BOOST_AUTO_TEST_CASE( LegacyTargetCannotOverwriteExistingFile )
{
    KI_TEST::SCOPED_TEMP_DIR source( wxS( "kemi-pads-reject-source" ) );
    KI_TEST::SCOPED_TEMP_DIR output( wxS( "kemi-pads-reject-output" ) );
    const wxString input = StageSample( source, wxS( "synthetic_constraints.asc" ) );
    const DIRECTORY_SNAPSHOT original = SnapshotDirectory( source.Path() );
    const wxString target = output.ChildPathStr( wxS( "project.kicad_dru" ) );
    WriteText( target, wxS( "# existing rules must survive\n(version 2)\n" ) );
    const DIRECTORY_SNAPSHOT existingRules = SnapshotDirectory( output.Path() );
    PCB_IO_PADS plugin;
    std::unique_ptr<BOARD> board = plugin.LoadBoard( input );
    BOOST_REQUIRE( board );
    BOOST_REQUIRE( !plugin.GetImportedDesignRules().IsEmpty() );

    KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY policy( output.PathStr() );
    BOOST_CHECK_THROW( plugin.WriteImportedDesignRules( policy, target ), IO_ERROR );
    BOOST_CHECK( SnapshotDirectory( output.Path() ) == existingRules );
    BOOST_CHECK( SnapshotDirectory( source.Path() ) == original );
    BOOST_CHECK( policy.Manifest().empty() );
}

BOOST_AUTO_TEST_SUITE_END()
