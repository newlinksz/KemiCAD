/*
 * This file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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

#include <boost/test/unit_test.hpp>
#include <board.h>
#include <drc/drc_rule_parser.h>
#include <drc/rule_editor/drc_re_rule_loader.h>
#include <drc/rule_editor/drc_re_rule_saver.h>
#include <drc/rule_editor/drc_rule_editor_utils.h>
#include <ki_exception.h>
#include <project.h>
#include <project/project_file.h>
#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>
#include <settings/settings_manager.h>
#include <wx/ffile.h>


BOOST_AUTO_TEST_SUITE( KemiRules )

BOOST_AUTO_TEST_CASE( RulesFollowBoardsInSubdirectoriesAndOutsideProject )
{
    SETTINGS_MANAGER manager;
    KI_TEST::SCOPED_TEMP_PROJECT project( manager, wxS( "kemi-rules-location" ), wxS( "board" ) );
    KI_TEST::SCOPED_TEMP_DIR outside( wxS( "kemi-rules-outside" ) );
    const wxString subdir = wxFileName( project.DirStr(), wxS( "boards" ) ).GetFullPath();
    BOOST_REQUIRE( wxMkdir( subdir ) );

    auto writeRules = []( const wxString& aPath, const wxString& aText )
    {
        wxFFile file( aPath, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aText, wxConvUTF8 ) );
        BOOST_REQUIRE( file.Close() );
    };

    // A tempting, wrong rules file must never be used for another board of the same name.
    writeRules( wxFileName( project.DirStr(), wxS( "board.kemi_dru" ) ).GetFullPath(),
                wxS( "(kemi_dru (format_version 1) (version 2)"
                     " (rule \"wrong\" (constraint clearance (min 9mm))))" ) );

    BOARD board;
    board.SetProject( &project.Project() );

    for( const wxString& directory : { subdir, outside.PathStr() } )
    {
        const wxString legacy = wxFileName( directory, wxS( "board.kicad_dru" ) ).GetFullPath();
        const wxString native = wxFileName( directory, wxS( "board.kemi_dru" ) ).GetFullPath();
        wxFileName boardPath( directory, wxS( "board.kemi_pcb" ) );

        if( directory == subdir )
            BOOST_REQUIRE( boardPath.MakeRelativeTo( project.DirStr() ) );

        board.SetFileName( boardPath.GetFullPath() );
        BOOST_CHECK_EQUAL( board.GetDesignRulesPath(), native );
        writeRules( legacy, wxS( "(version 2) (rule \"local\" (constraint clearance (min 0.3mm)))" ) );
        BOOST_CHECK_EQUAL( board.GetDesignRulesPath(), legacy );
        writeRules( native, wxS( "(kemi_dru (format_version 1) (version 2)"
                                " (rule \"native\" (constraint clearance (min 0.4mm))))" ) );
        BOOST_CHECK_EQUAL( board.GetDesignRulesPath(), native );
        BOOST_CHECK_EQUAL( board.GetDesignRulesWritePath( true ), native );
        DRC_RULE_LOADER loader;
        const auto rules = loader.LoadFile( board.GetDesignRulesPath() );
        BOOST_REQUIRE( loader.GetLoadError().IsEmpty() );
        BOOST_REQUIRE_EQUAL( rules.size(), 1u );
        BOOST_CHECK_EQUAL( rules.front().ruleName, wxS( "native" ) );
    }

    board.ClearProject();
}

BOOST_AUTO_TEST_CASE( RejectsInvalidContainerBoundaries )
{
    const std::vector<wxString> invalid = {
        wxS( "" ),
        wxS( "# empty container file\n" ),
        wxS( "(kemi_dru (format_version 1) (version 2)" ),
        wxS( "(kemi_dru (format_version 1) (version 2)) (version 2)" ),
        wxS( "(kemi_dru (format_version 1))" ),
        wxS( "(kemi_dru (version 2))" ),
        wxS( "(kemi_dru (format_version 1.5) (version 2))" ),
        wxS( "(kemi_dru (format_version 2) (version 2))" ),
        wxS( "(kemi_dru (format_version 4294967297) (version 2))" ),
        wxS( "(kemi_dru (format_version 1) (format_version 1) (version 2))" ),
        wxS( "(version 2)" )
    };

    for( const wxString& text : invalid )
    {
        BOOST_TEST_CONTEXT( text )
        {
            DRC_RULES_PARSER parser( text, wxS( "invalid.kemi_dru" ) );
            std::vector<std::shared_ptr<DRC_RULE>> rules;
            BOOST_CHECK_THROW( parser.Parse( rules, nullptr ), IO_ERROR );
        }
    }
}


BOOST_AUTO_TEST_CASE( FileRoundTripRejectsLegacyExport )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-rules" ) );
    DRC_RULE_LOADER loader;
    DRC_RULE_SAVER saver;
    const wxString source = dir.ChildPathStr( wxS( "source.kicad_dru" ) );
    {
        wxFFile file( source, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write(
                wxS( "# header\n(kemi_dru\n# container comment\n(format_version 1)\n(version 2)\n"
                     "# rule comment\n(rule \"clearance\" (constraint clearance (min 0.2mm)))\n)\n" ),
                wxConvUTF8 ) );
    }

    // The real GUI loader must accept a valid Kemi container under a legacy extension.
    auto entries = loader.LoadFile( source );
    BOOST_REQUIRE( loader.GetLoadError().IsEmpty() );
    BOOST_REQUIRE_EQUAL( entries.size(), 1 );
    BOOST_REQUIRE( loader.GetFileTrivia().kemiOnly );
    const std::string original = KI_TEST::LoadStringData( source );
    BOOST_CHECK( !saver.SaveFile( source, entries, nullptr, loader.GetFileTrivia() ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), original );
    BOOST_CHECK_THROW( saver.GenerateRulesText( entries, nullptr, loader.GetFileTrivia() ), IO_ERROR );

    const wxString target = dir.ChildPathStr( wxS( "rules.kemi_dru" ) );
    BOOST_REQUIRE( saver.SaveFile( target, entries, nullptr, loader.GetFileTrivia() ) );

    for( int attempt = 0; attempt < 2; ++attempt )
    {
        const std::string bytes = KI_TEST::LoadStringData( target );
        const size_t root = bytes.find( "(kemi_dru" );
        BOOST_REQUIRE( root != std::string::npos );
        BOOST_CHECK_EQUAL( bytes.find( "(kemi_dru", root + 1 ), std::string::npos );
        BOOST_CHECK( bytes.find( "# header" ) != std::string::npos );
        BOOST_CHECK( bytes.find( "# rule comment" ) != std::string::npos );
        BOOST_CHECK( bytes.find( "# container comment" ) != std::string::npos );

        DRC_RULE_LOADER reopened;
        auto reloaded = reopened.LoadFile( target );
        BOOST_REQUIRE( reopened.GetLoadError().IsEmpty() );
        BOOST_REQUIRE_EQUAL( reloaded.size(), 1 );
        BOOST_CHECK_EQUAL( reloaded[0].ruleName, entries[0].ruleName );
        BOOST_CHECK_EQUAL( reloaded[0].originalRuleText, entries[0].originalRuleText );
        BOOST_REQUIRE( saver.SaveFile( target, reloaded, nullptr, reopened.GetFileTrivia() ) );
        BOOST_CHECK_EQUAL( reopened.LoadFile( target ).size(), 1 );
    }

    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), original );
}


BOOST_AUTO_TEST_CASE( NativeBoardRulesTextPreservesLegacySourceAndSingleWrapper )
{
    SETTINGS_MANAGER manager;
    KI_TEST::SCOPED_TEMP_PROJECT project( manager, wxS( "kemi-rules-text" ), wxS( "board" ) );
    BOARD board;
    board.SetProject( &project.Project() );
    board.SetFileName( wxFileName( project.DirStr(), wxS( "board.kemi_pcb" ) ).GetFullPath() );
    const wxString official = wxFileName( project.DirStr(), wxS( "board.kicad_dru" ) ).GetFullPath();
    const wxString original = wxS( "# user header with (kemi_dru in a comment\n(version 2)\n"
                                  "# user rule\n(rule \"clearance\" (constraint clearance (min 0.2mm)))\n"
                                  "# final comment without newline" );
    {
        wxFFile file( official, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( original, wxConvUTF8 ) );
    }

    BOOST_CHECK_EQUAL( board.GetDesignRulesPath(), official );
    const wxString target = board.GetDesignRulesWritePath();
    BOOST_CHECK( target.EndsWith( wxS( ".kemi_dru" ) ) );
    DRC_RULE_SAVER saver;
    BOOST_CHECK( !saver.SaveTextFile( official, original, &board ) );
    BOOST_CHECK( !DRC_RULE_EDITOR_UTILS::SaveRules( official, {}, &board ) );
    BOOST_REQUIRE( saver.SaveTextFile( target, original, &board ) );
    const std::string native = KI_TEST::LoadStringData( target );
    BOOST_CHECK( native.starts_with( "(kemi_dru (format_version 1)" ) );
    BOOST_CHECK( native.find( original.ToStdString( wxConvUTF8 ) ) != std::string::npos );
    DRC_RULE_LOADER loader;
    BOOST_CHECK_EQUAL( loader.LoadFile( target ).size(), 1U );
    BOOST_REQUIRE( loader.GetLoadError().IsEmpty() );

    for( int attempt = 0; attempt < 2; ++attempt )
    {
        BOOST_REQUIRE( saver.SaveTextFile( target, wxString::FromUTF8( native ), &board ) );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( target ), native );
        BOOST_CHECK( !saver.SaveTextFile( official, wxString::FromUTF8( native ) ) );
        BOOST_CHECK( !saver.SaveTextFile( official, original, nullptr, loader.GetFileTrivia() ) );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( official ), original.ToStdString( wxConvUTF8 ) );
    }

    // Incomplete text remains saveable without an uncaught lexer exception or a legacy output.
    const wxString incomplete = wxS( "\"unfinished rule" );
    BOOST_CHECK( !DRC_RULE_SAVER::IsKemiText( incomplete ) );
    BOOST_CHECK( !saver.SaveTextFile( official, incomplete, &board ) );
    BOOST_REQUIRE( saver.SaveTextFile( target, incomplete, &board ) );
    BOOST_CHECK( KI_TEST::LoadStringData( target ).starts_with( "(kemi_dru" ) );
    BOOST_CHECK( KI_TEST::LoadStringData( target ).find( "\"unfinished rule" ) != std::string::npos );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( official ), original.ToStdString( wxConvUTF8 ) );
    board.ClearProject();
}


BOOST_AUTO_TEST_CASE( MarkedProjectSelectsNativeRulesForLegacyNamedBoard )
{
    SETTINGS_MANAGER manager;
    KI_TEST::SCOPED_TEMP_PROJECT project( manager, wxS( "kemi-rules-project" ), wxS( "board" ) );
    project.Project().GetProjectFile().SetKemiOnly();
    BOARD board;
    board.SetProject( &project.Project() );
    board.SetFileName( wxFileName( project.DirStr(), wxS( "board.kicad_pcb" ) ).GetFullPath() );
    BOOST_REQUIRE( board.IsKemiOnly() );
    const wxString target = board.GetDesignRulesWritePath();
    BOOST_CHECK( target.EndsWith( wxS( ".kemi_dru" ) ) );
    DRC_RULE_SAVER saver;
    BOOST_CHECK_THROW( saver.GenerateRulesText( {}, &board ), IO_ERROR );
    BOOST_REQUIRE( saver.SaveFile( target, {}, &board ) );
    BOOST_CHECK( KI_TEST::LoadStringData( target ).starts_with( "(kemi_dru" ) );
    BOOST_CHECK( !wxFileExists( wxFileName( project.DirStr(), wxS( "board.kicad_dru" ) ).GetFullPath() ) );
    board.ClearProject();
}


BOOST_AUTO_TEST_CASE( RejectedFileCannotBecomeEmptySave )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-rules-rejected" ) );
    const wxString target = dir.PathStr() + wxS( "/rules.kicad_dru" );
    const std::string original = "(kemi_dru (format_version 2) (version 2))\n";
    {
        wxFFile file( target, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE_EQUAL( file.Write( original.data(), original.size() ), original.size() );
    }

    DRC_RULE_LOADER loader;
    DRC_RULE_SAVER saver;
    for( int attempt = 0; attempt < 2; ++attempt )
    {
        auto entries = loader.LoadFile( target );
        BOOST_CHECK( entries.empty() );
        BOOST_CHECK( !loader.GetLoadError().IsEmpty() );
        BOOST_CHECK( loader.GetFileTrivia().loadFailed );
        BOOST_CHECK( !saver.SaveFile( target, entries, nullptr, loader.GetFileTrivia() ) );
        BOOST_CHECK_THROW( saver.GenerateRulesText( entries, nullptr, loader.GetFileTrivia() ),
                           IO_ERROR );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( target ), original );
    }
}

BOOST_AUTO_TEST_CASE( EmptyLegacyFileRemainsValid )
{
    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kemi-rules-empty-legacy" ) );
    const wxString source = dir.ChildPathStr( wxS( "empty.kicad_dru" ) );
    const wxString target = dir.ChildPathStr( wxS( "empty.kemi_dru" ) );

    for( const wxString& text : { wxS( "" ), wxS( "# only a comment" ) } )
    {
        {
            wxFFile file( source, wxS( "wb" ) );
            BOOST_REQUIRE( file.IsOpened() );
            BOOST_REQUIRE( file.Write( text, wxConvUTF8 ) );
        }

        DRC_RULE_LOADER loader;
        auto entries = loader.LoadFile( source );
        BOOST_CHECK( entries.empty() );
        BOOST_REQUIRE( loader.GetLoadError().IsEmpty() );
        BOOST_CHECK( !loader.GetFileTrivia().loadFailed );
        const std::string original = KI_TEST::LoadStringData( source );
        DRC_RULE_SAVER saver;
        BOOST_CHECK( !saver.SaveFile( source, entries, nullptr, loader.GetFileTrivia() ) );
        BOOST_REQUIRE( saver.SaveFile( target, entries, nullptr, loader.GetFileTrivia() ) );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), original );
        BOOST_CHECK( DRC_RULE_SAVER::IsKemiText( wxString::FromUTF8( KI_TEST::LoadStringData( target ) ) ) );
        BOOST_CHECK( loader.LoadFile( target ).empty() );
        BOOST_CHECK( loader.GetLoadError().IsEmpty() );

        if( !text.IsEmpty() )
            BOOST_CHECK( KI_TEST::LoadStringData( target ).find( "# only a comment" ) != std::string::npos );
    }
}

BOOST_AUTO_TEST_SUITE_END()
