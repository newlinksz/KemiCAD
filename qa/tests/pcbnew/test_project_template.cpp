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

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <board.h>
#include <common.h>
#include <drc/drc_engine.h>
#include <drc/drc_rule_condition.h>
#include <drc/drc_rule_parser.h>
#include <footprint.h>
#include <import_design_rules.h>
#include <kemi_format_profile.h>
#include <nlohmann/json.hpp>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <pcb_io/pcb_io_mgr.h>
#include <project.h>
#include <project_template.h>
#include <richio.h>
#include <settings/settings_manager.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filefn.h>

#include <functional>
#include <map>
#include <memory>


namespace
{
void writeRulesTestFile( const wxString& aPath, const wxString& aText )
{
    wxFFile output( aPath, wxS( "wb" ) );
    BOOST_REQUIRE( output.IsOpened() );
    BOOST_REQUIRE( output.Write( aText, wxConvUTF8 ) );
    BOOST_REQUIRE( output.Close() );
}


std::string readRulesTestBytes( const wxString& aPath )
{
    wxFFile input( aPath, wxS( "rb" ) );
    BOOST_REQUIRE( input.IsOpened() );
    BOOST_REQUIRE( input.Length() >= 0 );
    std::string bytes( static_cast<size_t>( input.Length() ), '\0' );
    BOOST_REQUIRE_EQUAL( input.Read( bytes.data(), bytes.size() ), bytes.size() );
    return bytes;
}


std::vector<std::shared_ptr<DRC_RULE>> loadTemplateRules( const wxString& aPath,
                                                       BOARD* aBoard = nullptr )
{
    wxString text = wxString::FromUTF8( readRulesTestBytes( aPath ) );

    if( aBoard )
    {
        std::function<bool( wxString* )> resolver =
                [&]( wxString* aToken ) -> bool
                {
                    return !IsComponentClassSelector( *aToken ) && aBoard->ResolveTextVar( aToken, 0 );
                };
        STRING_LINE_READER reader( text.ToStdString( wxConvUTF8 ), aPath );
        text.clear();

        while( char* line = reader.ReadLine() )
        {
            const wxString converted = aBoard->ConvertCrossReferencesToKIIDs( wxString::FromUTF8( line ) );
            text << ExpandTextVars( converted, &resolver, INTERNAL ) << '\n';
        }
    }

    DRC_RULES_PARSER parser( text, aPath );
    std::vector<std::shared_ptr<DRC_RULE>> rules;
    parser.Parse( rules, nullptr );
    return rules;
}


class TEMPLATE_RULES_FIXTURE
{
public:
    TEMPLATE_RULES_FIXTURE() : m_directory( wxS( "project-template-rules" ) )
    {
        m_source = m_directory.ChildPathStr( wxS( "template" ) );
        m_destination = m_directory.ChildPathStr( wxS( "result" ) );
        const wxString meta = wxFileName( m_source, wxS( "meta" ) ).GetFullPath();
        BOOST_REQUIRE( wxFileName::Mkdir( meta, 0777, wxPATH_MKDIR_FULL ) );
        BOOST_REQUIRE( wxMkdir( m_destination ) );
        writeRulesTestFile( wxFileName( meta, wxS( "info.html" ) ).GetFullPath(),
                            wxS( "<html><head><title>Rules</title></head></html>" ) );
        writeRulesTestFile( SourceProject(), wxS( "{\"meta\":{\"version\":1}}\n" ) );
    }

    wxString SourceProject() const
    {
        return wxFileName( m_source, wxS( "template.kicad_pro" ) ).GetFullPath();
    }

    void CheckCommentedRules( bool aNative )
    {
        const wxString source = wxFileName( m_source,
                aNative ? wxS( "template.kemi_dru" ) : wxS( "template.kicad_dru" ) ).GetFullPath();
        const std::string projectBefore = readRulesTestBytes( SourceProject() );

        for( bool crlf : { false, true } )
        {
            BOOST_TEST_CONTEXT( ( aNative ? "native" : "official" ) << ( crlf ? " CRLF" : " LF" ) )
            {
                wxString contents = wxS(
                        "# ordinary header comment\n"
                        "(version 1)\n"
                        "  # ) (( \" comment delimiters must not terminate the container\n"
                        "(rule \"Gap # \\\"quoted\\\"\"\n"
                        " # another ( comment with an unmatched \"\n"
                        " (condition \"A.NetName == 'Net#1'\")\n"
                        " (constraint clearance (min 0.2mm) (opt 0.25mm) (max 0.3mm)))\n"
                        "# )\n"
                        "(rule \"Width#2\" (constraint track_width (min 0.3mm) (max 0.5mm)))\n"
                        "# trailing comment ) (( \"" );

                if( aNative )
                {
                    contents = wxS( "# outside wrapper ) \"\n(kemi_dru\n"
                                    " # wrapper comment (( \"\n(format_version 1)\n" )
                               + contents + wxS( "\n)\n# outside trailer ( \"" );
                }

                if( crlf )
                    contents.Replace( wxS( "\n" ), wxS( "\r\n" ) );

                writeRulesTestFile( source, contents );
                const std::string sourceBefore = readRulesTestBytes( source );
                const auto expected = loadTemplateRules( source );
                BOOST_REQUIRE_EQUAL( expected.size(), 2u );
                BOOST_REQUIRE_EQUAL( expected[0]->m_Constraints.size(), 1u );
                BOOST_CHECK_EQUAL( expected[0]->m_Constraints[0].GetValue().Min(), 200000 );

                PROJECT_TEMPLATE projectTemplate( m_source,
                        []( const wxString& aSource, const wxString& aTarget )
                        {
                            SaveNativeDesignRules( aSource, aTarget );
                            return true;
                        } );

                // Each line-ending variant has its own fresh destination, so no user output
                // is removed or overwritten in order to run the next variant.
                const wxString directory = wxFileName( m_destination,
                        crlf ? wxS( "crlf" ) : wxS( "lf" ) ).GetFullPath();
                wxFileName project( directory, wxS( "result.kemi_pro" ) );
                wxString error;
                BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( project, &error ), error );
                const wxString output = wxFileName( directory, wxS( "result.kemi_dru" ) ).GetFullPath();
                const std::string written = readRulesTestBytes( output );

                if( aNative )
                    BOOST_CHECK_EQUAL( written, sourceBefore );
                else
                    BOOST_CHECK( written.find( sourceBefore ) != std::string::npos );

                for( int reload = 0; reload < 2; ++reload )
                {
                    const auto actual = loadTemplateRules( output );
                    BOOST_REQUIRE_EQUAL( actual.size(), expected.size() );

                    for( size_t i = 0; i < expected.size(); ++i )
                    {
                        BOOST_CHECK_EQUAL( actual[i]->m_Name, expected[i]->m_Name );
                        BOOST_CHECK( actual[i]->m_Severity == expected[i]->m_Severity );
                        BOOST_CHECK( actual[i]->m_LayerCondition == expected[i]->m_LayerCondition );
                        BOOST_REQUIRE_EQUAL( actual[i]->m_Condition != nullptr,
                                             expected[i]->m_Condition != nullptr );

                        if( expected[i]->m_Condition )
                        {
                            BOOST_CHECK_EQUAL( actual[i]->m_Condition->GetExpression(),
                                                 expected[i]->m_Condition->GetExpression() );
                        }

                        BOOST_REQUIRE_EQUAL( actual[i]->m_Constraints.size(), expected[i]->m_Constraints.size() );

                        for( size_t j = 0; j < expected[i]->m_Constraints.size(); ++j )
                        {
                            const auto& actualConstraint = actual[i]->m_Constraints[j];
                            const auto& expectedConstraint = expected[i]->m_Constraints[j];
                            BOOST_CHECK( actualConstraint.m_Type == expectedConstraint.m_Type );
                            const auto& actualValue = actualConstraint.GetValue();
                            const auto& expectedValue = expectedConstraint.GetValue();
                            BOOST_CHECK_EQUAL( actualValue.HasMin(), expectedValue.HasMin() );
                            BOOST_CHECK_EQUAL( actualValue.HasOpt(), expectedValue.HasOpt() );
                            BOOST_CHECK_EQUAL( actualValue.HasMax(), expectedValue.HasMax() );

                            if( expectedValue.HasMin() )
                                BOOST_CHECK_EQUAL( actualValue.Min(), expectedValue.Min() );

                            if( expectedValue.HasOpt() )
                                BOOST_CHECK_EQUAL( actualValue.Opt(), expectedValue.Opt() );

                            if( expectedValue.HasMax() )
                                BOOST_CHECK_EQUAL( actualValue.Max(), expectedValue.Max() );
                        }
                    }
                }

                BOOST_CHECK_EQUAL( readRulesTestBytes( source ), sourceBefore );
                BOOST_CHECK_EQUAL( readRulesTestBytes( SourceProject() ), projectBefore );
                BOOST_CHECK( !wxFileExists( wxFileName( directory, wxS( "result.kicad_dru" ) ).GetFullPath() ) );
            }
        }
    }

    KI_TEST::SCOPED_TEMP_DIR m_directory;
    wxString m_source;
    wxString m_destination;
};
} // namespace


// MF1/MF2; execution instructions and status: ../common/project_template_regressions.md.
BOOST_FIXTURE_TEST_SUITE( ProjectTemplate, TEMPLATE_RULES_FIXTURE )

BOOST_AUTO_TEST_CASE( OfficialRulesPreserveCommentsAndConstraints )
{
    CheckCommentedRules( false );
}


BOOST_AUTO_TEST_CASE( NativeRulesPreserveCommentsAndConstraints )
{
    CheckCommentedRules( true );
}


BOOST_AUTO_TEST_CASE( RulesUseProjectTextVariablesWithoutExpandingOutput )
{
    for( bool native : { false, true } )
    {
        for( bool defined : { true, false } )
        {
            BOOST_TEST_CONTEXT( ( native ? "native" : "official" )
                                << ( defined ? " defined variable" : " missing variable" ) )
            {
                TEMPLATE_RULES_FIXTURE fixture;
                nlohmann::json variables = { { "RULE_WIDTH", "0.3mm" } };

                if( defined )
                    variables["RULE_NET"] = "GND";

                nlohmann::json document = { { "meta", { { "version", 1 } } },
                                           { "text_variables", variables } };
                wxString projectSource = fixture.SourceProject();

                if( native )
                {
                    // The retained official project has no variables: validation must use
                    // the native counterpart, just as template publication does.
                    KEMI_FORMAT::StampJsonMarker( document, KEMI_FORMAT::DOC::PROJECT );
                    projectSource = wxFileName( fixture.m_source, wxS( "template.kemi_pro" ) ).GetFullPath();
                }

                writeRulesTestFile( projectSource, wxString::FromUTF8( document.dump( 2 ) ) );
                const wxString source = wxFileName( fixture.m_source,
                        native ? wxS( "template.kemi_dru" ) : wxS( "template.kicad_dru" ) ).GetFullPath();
                wxString contents = wxS(
                        "# keep ${RULE_NET} and comment punctuation ) (( \"\n"
                        "(version 1)\n"
                        "(rule \"Gap\" (condition \"A.NetName == '${RULE_NET}'\")\n"
                        " (constraint clearance (min 0.2mm)))\n"
                        "(rule \"Width\" (condition \"A.NetName == '${RULE_NET}'\")\n"
                        " (constraint track_width (min ${RULE_WIDTH})))\n" );

                if( native )
                    contents = wxS( "(kemi_dru (format_version 1)\n" ) + contents + wxS( ")\n" );

                writeRulesTestFile( source, contents );
                const std::string rulesBefore = readRulesTestBytes( source );
                const wxString sentinel = wxFileName( fixture.m_destination, wxS( "user.txt" ) ).GetFullPath();
                writeRulesTestFile( sentinel, wxS( "pre-existing user file" ) );
                PROJECT_TEMPLATE projectTemplate( fixture.m_source,
                        []( const wxString& aSource, const wxString& aTarget )
                        {
                            SaveNativeDesignRules( aSource, aTarget );
                            return true;
                        } );
                std::map<wxString, std::string> sourceBefore;

                for( const wxFileName& file : projectTemplate.GetFileList() )
                {
                    if( file.FileExists() )
                        sourceBefore.emplace( file.GetFullPath(), readRulesTestBytes( file.GetFullPath() ) );
                }

                wxFileName project( fixture.m_destination, wxS( "result.kemi_pro" ) );
                const wxString output = wxFileName( fixture.m_destination, wxS( "result.kemi_dru" ) ).GetFullPath();
                wxString error;

                if( defined )
                {
                    BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( project, &error ), error );
                    const auto writtenProject = nlohmann::json::parse( readRulesTestBytes( project.GetFullPath() ) );
                    BOOST_CHECK( writtenProject["text_variables"] == variables );
                    const std::string written = readRulesTestBytes( output );

                    if( native )
                        BOOST_CHECK_EQUAL( written, rulesBefore );
                    else
                        BOOST_CHECK( written.find( rulesBefore ) != std::string::npos );

                    for( const char* variable : { "${RULE_NET}", "${RULE_WIDTH}" } )
                        BOOST_CHECK( written.find( variable ) != std::string::npos );

                    // Reopen the final project, independently of the template's validation
                    // context, then exercise both the real DRC loader and explicit rule counts.
                    SETTINGS_MANAGER settings;
                    BOOST_REQUIRE( settings.LoadProject( project.GetFullPath(), false ) );
                    BOARD board;
                    board.SetProject( settings.GetProject( project.GetFullPath() ) );
                    board.SetFileName( wxFileName( fixture.m_destination, wxS( "result.kemi_pcb" ) ).GetFullPath() );
                    DRC_ENGINE engine( &board, &board.GetDesignSettings() );
                    engine.InitEngine( wxFileName( output ) );
                    BOOST_CHECK( engine.RulesValid() );
                    const auto rules = loadTemplateRules( output, &board );
                    BOOST_REQUIRE_EQUAL( rules.size(), 2u );

                    for( const auto& rule : rules )
                    {
                        BOOST_REQUIRE( rule->m_Condition );
                        BOOST_CHECK_EQUAL( rule->m_Condition->GetExpression(), wxS( "A.NetName == 'GND'" ) );
                        BOOST_REQUIRE_EQUAL( rule->m_Constraints.size(), 1u );
                    }

                    BOOST_CHECK_EQUAL( rules[0]->m_Name, wxS( "Gap" ) );
                    BOOST_CHECK( rules[0]->m_Constraints[0].m_Type == CLEARANCE_CONSTRAINT );
                    BOOST_CHECK_EQUAL( rules[0]->m_Constraints[0].GetValue().Min(), 200000 );
                    BOOST_CHECK_EQUAL( rules[1]->m_Name, wxS( "Width" ) );
                    BOOST_CHECK( rules[1]->m_Constraints[0].m_Type == TRACK_WIDTH_CONSTRAINT );
                    BOOST_CHECK_EQUAL( rules[1]->m_Constraints[0].GetValue().Min(), 300000 );
                }
                else
                {
                    BOOST_CHECK( !projectTemplate.CreateProject( project, &error ) );
                    BOOST_CHECK( !error.IsEmpty() );
                    BOOST_CHECK( !project.FileExists() );
                    BOOST_CHECK( !wxFileExists( output ) );
                }

                std::map<wxString, std::string> sourceAfter;

                for( const wxFileName& file : projectTemplate.GetFileList() )
                {
                    if( file.FileExists() )
                        sourceAfter.emplace( file.GetFullPath(), readRulesTestBytes( file.GetFullPath() ) );
                }

                BOOST_CHECK( sourceAfter == sourceBefore );
                BOOST_CHECK_EQUAL( readRulesTestBytes( sentinel ), "pre-existing user file" );
                BOOST_CHECK( !wxFileExists( wxFileName( fixture.m_destination,
                                                       wxS( "result.kicad_dru" ) ).GetFullPath() ) );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( RulesUseBoardTextVariablesWithoutExpandingOutput )
{
    for( bool native : { false, true } )
    {
        BOOST_TEST_CONTEXT( ( native ? "native" : "official" ) )
        {
            TEMPLATE_RULES_FIXTURE fixture;
            nlohmann::json document = { { "meta", { { "version", 1 } } },
                                       { "text_variables", { { "RULE_WIDTH", "0.3mm" } } } };
            wxString projectSource = fixture.SourceProject();

            if( native )
            {
                KEMI_FORMAT::StampJsonMarker( document, KEMI_FORMAT::DOC::PROJECT );
                projectSource = wxFileName( fixture.m_source, wxS( "template.kemi_pro" ) ).GetFullPath();
            }

            writeRulesTestFile( projectSource, wxString::FromUTF8( document.dump( 2 ) ) );

            BOARD sourceBoard;
            sourceBoard.GetTitleBlock().SetComment( 0, wxS( "GND" ) );
            sourceBoard.GetTitleBlock().SetComment( 1, wxS( "${RULE_WIDTH}" ) );
            sourceBoard.SetProperties( { { wxS( "BOARD_NET" ), wxS( "GND" ) } } );
            sourceBoard.AddVariant( wxS( "Assembly" ) );
            sourceBoard.SetVariantDescription( wxS( "Assembly" ), wxS( "Assembly description" ) );
            FOOTPRINT* footprint = new FOOTPRINT( &sourceBoard );
            footprint->SetReference( wxS( "U1" ) );
            footprint->SetValue( wxS( "GND" ) );
            sourceBoard.Add( footprint );
            PCB_IO_KICAD_SEXPR io;
            STRING_FORMATTER formatted;
            io.FormatBoardToFormatter( &formatted, &sourceBoard, nullptr, KEMI_FORMAT::FLAVOR::KEMI );
            wxString boardContents = wxString::FromUTF8( formatted.GetString() );

            if( !native )
            {
                const wxString header = wxString::Format( wxS( "(kemi_pcb (format_version %d)" ),
                                                           KEMI_FORMAT::FORMAT_VERSION );
                wxString body;
                BOOST_REQUIRE( boardContents.StartsWith( header, &body ) );
                boardContents = wxS( "(kicad_pcb" ) + body;
            }

            const wxString boardSource = wxFileName( fixture.m_source,
                    native ? wxS( "template.kemi_pcb" ) : wxS( "template.kicad_pcb" ) ).GetFullPath();
            writeRulesTestFile( boardSource, boardContents );

            if( native )
            {
                writeRulesTestFile( wxFileName( fixture.m_source, wxS( "template.kicad_pcb" ) ).GetFullPath(),
                                    wxS( "(kicad_pcb (version 20260901) (generator pcbnew) (general))\n" ) );
            }

            const wxString source = wxFileName( fixture.m_source,
                    native ? wxS( "template.kemi_dru" ) : wxS( "template.kicad_dru" ) ).GetFullPath();
            wxString contents = wxS(
                    "# keep ${COMMENT1} and comment punctuation ) (( \"\n"
                    "(version 1)\n"
                    "(rule \"Gap\"\n"
                    " (condition \"A.NetName == '${COMMENT1}' && A.NetName == '${BOARD_NET}'"
                    " && A.NetName == '${U1:VALUE}'\")\n"
                    " (constraint clearance (min 0.2mm) (opt 0.25mm) (max 0.3mm)))\n"
                    "(rule \"Width\"\n"
                    " (condition \"'${FILENAME}' != '' && '${VARIANT}' != 'missing'"
                    " && '${VARIANT_DESC}' != 'missing'\")\n"
                    " (constraint track_width (min ${COMMENT2})))\n" );

            if( native )
                contents = wxS( "(kemi_dru (format_version 1)\n" ) + contents + wxS( ")\n" );

            writeRulesTestFile( source, contents );
            const std::string rulesBefore = readRulesTestBytes( source );
            auto snapshotSource = [&]()
            {
                wxArrayString files;
                wxDir::GetAllFiles( fixture.m_source, &files );
                BOOST_REQUIRE( !files.empty() );
                std::map<wxString, std::string> snapshot;

                for( const wxString& file : files )
                    snapshot.emplace( file, readRulesTestBytes( file ) );

                return snapshot;
            };
            const auto sourceBefore = snapshotSource();
            PROJECT_TEMPLATE projectTemplate( fixture.m_source,
                    []( const wxString& aSource, const wxString& aTarget )
                    {
                        SaveNativeDesignRules( aSource, aTarget );
                        return true;
                    } );
            wxFileName project( fixture.m_destination, wxS( "result.kemi_pro" ) );
            wxString error;
            BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( project, &error ), error );
            const wxString output = wxFileName( fixture.m_destination, wxS( "result.kemi_dru" ) ).GetFullPath();
            const std::string written = readRulesTestBytes( output );

            if( native )
                BOOST_CHECK_EQUAL( written, rulesBefore );
            else
                BOOST_CHECK( written.find( rulesBefore ) != std::string::npos );

            for( const char* variable : { "${COMMENT1}", "${COMMENT2}", "${BOARD_NET}", "${U1:VALUE}",
                                         "${FILENAME}", "${VARIANT}", "${VARIANT_DESC}" } )
            {
                BOOST_CHECK( written.find( variable ) != std::string::npos );
            }

            SETTINGS_MANAGER settings;
            BOOST_REQUIRE( settings.LoadProject( project.GetFullPath(), false ) );
            PROJECT* reopenedProject = settings.GetProject( project.GetFullPath() );
            reopenedProject->SetReadOnly();
            const wxString boardOutput = wxFileName( fixture.m_destination, wxS( "result.kemi_pcb" ) ).GetFullPath();
            std::unique_ptr<BOARD> board = PCB_IO_MGR::Load( PCB_IO_MGR::KICAD_SEXP, boardOutput,
                                                           nullptr, reopenedProject );
            board->SetProject( reopenedProject );

            for( const wxString& variant : { wxString(), wxString( "Assembly" ) } )
            {
                board->SetCurrentVariant( variant );
                const wxString description = variant.IsEmpty() ? wxString() : wxString( "Assembly description" );
                DRC_ENGINE engine( board.get(), &board->GetDesignSettings() );
                engine.InitEngine( wxFileName( output ) );
                BOOST_CHECK( engine.RulesValid() );
                const auto rules = loadTemplateRules( output, board.get() );
                BOOST_REQUIRE_EQUAL( rules.size(), 2u );
                BOOST_CHECK_EQUAL( rules[0]->m_Name, wxS( "Gap" ) );
                BOOST_REQUIRE( rules[0]->m_Condition );
                BOOST_CHECK_EQUAL( rules[0]->m_Condition->GetExpression(),
                        wxS( "A.NetName == 'GND' && A.NetName == 'GND' && A.NetName == 'GND'" ) );
                BOOST_REQUIRE_EQUAL( rules[0]->m_Constraints.size(), 1u );
                const auto& clearance = rules[0]->m_Constraints[0];
                BOOST_CHECK( clearance.m_Type == CLEARANCE_CONSTRAINT );
                BOOST_CHECK_EQUAL( clearance.GetValue().Min(), 200000 );
                BOOST_CHECK_EQUAL( clearance.GetValue().Opt(), 250000 );
                BOOST_CHECK_EQUAL( clearance.GetValue().Max(), 300000 );
                BOOST_CHECK_EQUAL( rules[1]->m_Name, wxS( "Width" ) );
                BOOST_REQUIRE( rules[1]->m_Condition );
                BOOST_CHECK_EQUAL( rules[1]->m_Condition->GetExpression(),
                        wxString::Format( wxS( "'result.kemi_pcb' != '' && '%s' != 'missing'"
                                               " && '%s' != 'missing'" ),
                                          variant, description ) );
                BOOST_REQUIRE_EQUAL( rules[1]->m_Constraints.size(), 1u );
                BOOST_CHECK( rules[1]->m_Constraints[0].m_Type == TRACK_WIDTH_CONSTRAINT );
                BOOST_CHECK_EQUAL( rules[1]->m_Constraints[0].GetValue().Min(), 300000 );
            }

            BOOST_CHECK( snapshotSource() == sourceBefore );
            BOOST_CHECK( !wxFileExists( wxFileName( fixture.m_destination,
                                                   wxS( "result.kicad_dru" ) ).GetFullPath() ) );
            BOOST_CHECK( !wxFileExists( wxFileName( fixture.m_destination,
                                                   wxS( "result.kicad_pcb" ) ).GetFullPath() ) );
        }
    }
}


BOOST_AUTO_TEST_CASE( InvalidRulesDoNotPublishTemplateFiles )
{
    const wxString source = wxFileName( m_source, wxS( "template.kemi_dru" ) ).GetFullPath();
    writeRulesTestFile( source, wxS( "(kemi_dru (format_version 1) (version 1))\n"
                                    "# legal comment ) (( \"\n"
                                    "(rule \"outside\" (constraint clearance (min 0.2mm)))\n" ) );
    const std::string sourceBefore = readRulesTestBytes( source );
    const std::string projectBefore = readRulesTestBytes( SourceProject() );
    const wxString sentinel = wxFileName( m_destination, wxS( "user.txt" ) ).GetFullPath();
    writeRulesTestFile( sentinel, wxS( "pre-existing user file" ) );
    PROJECT_TEMPLATE projectTemplate( m_source,
            []( const wxString& aSource, const wxString& aTarget )
            {
                SaveNativeDesignRules( aSource, aTarget );
                return true;
            } );
    wxFileName project( m_destination, wxS( "result.kemi_pro" ) );
    wxString error;
    BOOST_CHECK( !projectTemplate.CreateProject( project, &error ) );
    BOOST_CHECK( !error.IsEmpty() );
    BOOST_CHECK( !project.FileExists() );
    BOOST_CHECK( !wxFileExists( wxFileName( m_destination, wxS( "result.kemi_dru" ) ).GetFullPath() ) );
    BOOST_CHECK_EQUAL( readRulesTestBytes( sentinel ), "pre-existing user file" );
    BOOST_CHECK_EQUAL( readRulesTestBytes( source ), sourceBefore );
    BOOST_CHECK_EQUAL( readRulesTestBytes( SourceProject() ), projectBefore );
}


BOOST_AUTO_TEST_CASE( CommentOnlyOfficialRulesGainNativeVersion )
{
    const wxString source = wxFileName( m_source, wxS( "template.kicad_dru" ) ).GetFullPath();
    writeRulesTestFile( source, wxS( "# no rules ) (( \"" ) );
    const std::string sourceBefore = readRulesTestBytes( source );
    PROJECT_TEMPLATE projectTemplate( m_source,
            []( const wxString& aSource, const wxString& aTarget )
            {
                SaveNativeDesignRules( aSource, aTarget );
                return true;
            } );
    wxFileName project( m_destination, wxS( "result.kemi_pro" ) );
    wxString error;
    BOOST_REQUIRE_MESSAGE( projectTemplate.CreateProject( project, &error ), error );
    const wxString output = wxFileName( m_destination, wxS( "result.kemi_dru" ) ).GetFullPath();
    BOOST_CHECK( loadTemplateRules( output ).empty() );
    BOOST_CHECK( readRulesTestBytes( output ).find( sourceBefore ) != std::string::npos );
    BOOST_CHECK_EQUAL( readRulesTestBytes( source ), sourceBefore );
}


BOOST_AUTO_TEST_SUITE_END()
