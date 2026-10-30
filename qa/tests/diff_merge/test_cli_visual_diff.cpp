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
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/gpl-3.0.html
 * or you may search the http://www.gnu.org website for the version 3 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <boost/test/unit_test.hpp>

#include <jobs/job_pcb_diff.h>
#include <diff_merge/diff_doc_kind.h>
#include <cli/exit_codes.h>
#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/process.h>
#include <wx/stdpaths.h>
#include <wx/txtstrm.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>
#include <utility>


#ifndef QA_KICAD_CLI_PATH
#define QA_KICAD_CLI_PATH "kemicad-cli"
#endif


namespace
{

struct TEMP_DIR
{
    TEMP_DIR()
    {
        static int counter = 0;
        m_path = wxFileName::GetTempDir() + wxFILE_SEP_PATH
                 + wxString::Format( wxS( "kicad_cli_visual_diff_%ld_%d" ),
                                     static_cast<long>( wxGetProcessId() ), ++counter );

        BOOST_REQUIRE( wxFileName::Mkdir( m_path, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    }

    ~TEMP_DIR()
    {
        if( !m_path.IsEmpty() && wxFileName::DirExists( m_path ) )
            wxFileName::Rmdir( m_path, wxPATH_RMDIR_RECURSIVE );
    }

    wxString Path( const wxString& aName ) const
    {
        return m_path + wxFILE_SEP_PATH + aName;
    }

    wxString m_path;
};


struct COMMAND_RESULT
{
    int      exitCode = -1;
    wxString output;
    wxString error;
};


wxString readStream( wxInputStream* aStream )
{
    wxString output;

    if( !aStream )
        return output;

    wxTextInputStream textStream( *aStream );

    while( !aStream->Eof() )
    {
        wxString line = textStream.ReadLine();

        if( !line.IsEmpty() || !aStream->Eof() )
        {
            if( !output.IsEmpty() )
                output += wxS( "\n" );

            output += line;
        }
    }

    return output;
}


COMMAND_RESULT runCli( const std::vector<wxString>& aArgs )
{
    wxProcess process;
    process.Redirect();

    std::vector<const wchar_t*> argv;
    argv.reserve( aArgs.size() + 2 );
    argv.push_back( wxS( QA_KICAD_CLI_PATH ) );

    for( const wxString& arg : aArgs )
        argv.push_back( arg.wc_str() );

    argv.push_back( nullptr );

    COMMAND_RESULT result;
    result.exitCode = static_cast<int>( wxExecute( const_cast<wchar_t**>( argv.data() ), wxEXEC_SYNC, &process ) );
    result.output = readStream( process.GetInputStream() );
    result.error = readStream( process.GetErrorStream() );

    return result;
}


// The A/B fixtures live in qa/data; kicad-cli writes its diff output next to its inputs so
// each run gets a scratch copy rather than touching the source tree.
void copyFixture( const wxString& aName, const wxString& aDest )
{
    wxString src = wxString::FromUTF8( KI_TEST::GetTestDataRootDir() )
                   + wxS( "diff_merge/visual_diff/" ) + aName;

    BOOST_REQUIRE_MESSAGE( wxCopyFile( src, aDest ), "Could not copy " << src );
}


void expectCleanExit( const wxString& aName, const COMMAND_RESULT& aResult, int aExpectedExitCode )
{
    BOOST_TEST_CONTEXT( aName )
    {
        BOOST_CHECK_EQUAL( aResult.exitCode, aExpectedExitCode );
        BOOST_CHECK_MESSAGE( aResult.output.IsEmpty(), "Unexpected stdout: " << aResult.output );
        BOOST_CHECK_MESSAGE( aResult.error.IsEmpty(), "Unexpected stderr: " << aResult.error );
    }
}


void expectInvalidExit( const wxString& aName, const COMMAND_RESULT& aResult, int aExpectedExitCode )
{
    BOOST_TEST_CONTEXT( aName )
    {
        BOOST_CHECK_EQUAL( aResult.exitCode, aExpectedExitCode );
    }
}


void expectSvg( const wxString& aName, const wxString& aPath )
{
    BOOST_TEST_CONTEXT( aName )
    {
        std::string bytes = KI_TEST::LoadStringData( aPath );
        BOOST_REQUIRE( !bytes.empty() );
        BOOST_CHECK( bytes.find( "<svg" ) != std::string::npos );
    }
}


void expectPng( const wxString& aName, const wxString& aPath )
{
    static constexpr std::array<unsigned char, 8> PNG_HEADER = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

    BOOST_TEST_CONTEXT( aName )
    {
        std::string bytes = KI_TEST::LoadStringData( aPath );
        BOOST_REQUIRE_GE( bytes.size(), PNG_HEADER.size() );

        for( size_t i = 0; i < PNG_HEADER.size(); ++i )
            BOOST_CHECK_EQUAL( static_cast<unsigned char>( bytes[i] ), PNG_HEADER[i] );
    }
}


void expectFilesDiffer( const wxString& aName, const wxString& aPathA, const wxString& aPathB )
{
    BOOST_TEST_CONTEXT( aName )
    {
        BOOST_CHECK( KI_TEST::LoadStringData( aPathA ) != KI_TEST::LoadStringData( aPathB ) );
    }
}


struct CLI_FIXTURES
{
    explicit CLI_FIXTURES( TEMP_DIR& aDir )
    {
        pcbA = aDir.Path( wxS( "pcb_a.kicad_pcb" ) );
        pcbB = aDir.Path( wxS( "pcb_b.kicad_pcb" ) );
        schA = aDir.Path( wxS( "sch_a.kicad_sch" ) );
        schB = aDir.Path( wxS( "sch_b.kicad_sch" ) );
        fpA = aDir.Path( wxS( "fp_a.pretty" ) );
        fpB = aDir.Path( wxS( "fp_b.pretty" ) );
        symA = aDir.Path( wxS( "sym_a.kicad_sym" ) );
        symB = aDir.Path( wxS( "sym_b.kicad_sym" ) );

        writePcbFixtures();
        writeSchFixtures();
        writeFpFixtures();
        writeSymFixtures();
    }

    void writePcbFixtures()
    {
        copyFixture( wxS( "pcb_a.kicad_pcb" ), pcbA );
        copyFixture( wxS( "pcb_b.kicad_pcb" ), pcbB );
    }

    void writeSchFixtures()
    {
        copyFixture( wxS( "sch_a.kicad_sch" ), schA );
        copyFixture( wxS( "sch_b.kicad_sch" ), schB );
    }

    void writeFpFixtures()
    {
        BOOST_REQUIRE( wxFileName::Mkdir( fpA, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
        BOOST_REQUIRE( wxFileName::Mkdir( fpB, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

        // fp_a stays empty so the diff reports VisualDiff as added
        copyFixture( wxS( "fp_b.pretty/VisualDiff.kicad_mod" ),
                     fpB + wxFILE_SEP_PATH + wxS( "VisualDiff.kicad_mod" ) );
    }

    void writeSymFixtures()
    {
        copyFixture( wxS( "sym_a.kicad_sym" ), symA );
        copyFixture( wxS( "sym_b.kicad_sym" ), symB );
    }

    wxString pcbA;
    wxString pcbB;
    wxString schA;
    wxString schB;
    wxString fpA;
    wxString fpB;
    wxString symA;
    wxString symB;
};


struct CLI_CASE
{
    wxString commandGroup;
    wxString commandName;
    wxString refPath;
    wxString changedPath;
};


void expectVisualDifference( TEMP_DIR& aDir, const CLI_CASE& aCase, const wxString& aFormat )
{
    const wxString caseName = aCase.commandGroup + wxS( " " ) + aFormat;
    const wxString sameOut = aDir.Path( aCase.commandGroup + wxS( "_same." ) + aFormat );
    const wxString diffOut = aDir.Path( aCase.commandGroup + wxS( "_diff." ) + aFormat );

    std::vector<wxString> sameArgs = { aCase.commandGroup, aCase.commandName, aCase.refPath, aCase.refPath,
                                       wxS( "--format" ), aFormat, wxS( "--output" ), sameOut };
    std::vector<wxString> diffArgs = { aCase.commandGroup, aCase.commandName, aCase.refPath, aCase.changedPath,
                                       wxS( "--format" ), aFormat, wxS( "--output" ), diffOut };

    expectCleanExit( caseName + wxS( " identical" ), runCli( sameArgs ), 0 );
    expectCleanExit( caseName + wxS( " changed" ), runCli( diffArgs ), 5 );

    if( aFormat == wxS( "svg" ) )
    {
        expectSvg( caseName + wxS( " identical" ), sameOut );
        expectSvg( caseName + wxS( " changed" ), diffOut );
    }
    else
    {
        expectPng( caseName + wxS( " identical" ), sameOut );
        expectPng( caseName + wxS( " changed" ), diffOut );
    }

    expectFilesDiffer( caseName, sameOut, diffOut );
}

} // namespace


BOOST_AUTO_TEST_SUITE( DiffJobConfig )


// The diff jobs once declared their own m_outputPath, shadowing JOB::m_outputPath and
// registering a duplicate "output" JSON param alongside the base "output_filename" key.
// Serialization must now expose exactly one output key, routed through the base member.
BOOST_AUTO_TEST_CASE( DiffJobSerializesSingleOutputKey )
{
    JOB_PCB_DIFF job;
    job.SetConfiguredOutputPath( wxS( "diff-out.json" ) );

    BOOST_CHECK_EQUAL( job.GetConfiguredOutputPath(), wxString( wxS( "diff-out.json" ) ) );

    nlohmann::json j;
    job.ToJson( j );

    BOOST_CHECK( j.contains( "output_filename" ) );
    BOOST_CHECK_EQUAL( j["output_filename"].get<wxString>(), wxString( wxS( "diff-out.json" ) ) );
    BOOST_CHECK( !j.contains( "output" ) );
}


BOOST_AUTO_TEST_CASE( NativeAndOfficialExtensionsSelectTheSameMergeKind )
{
    using namespace KICAD_DIFF;
    for( const wxString& prefix : { wxS( "kemi" ), wxS( "kicad" ) } )
    {
        BOOST_CHECK( DocKindFromExtension( wxS( "a." ) + prefix + wxS( "_pcb" ) ) == DOC_KIND::PCB );
        BOOST_CHECK( DocKindFromExtension( wxS( "a." ) + prefix + wxS( "_sch" ) ) == DOC_KIND::SCH );
        BOOST_CHECK( DocKindFromExtension( wxS( "a." ) + prefix + wxS( "_sym" ) ) == DOC_KIND::SYM_LIB );
        BOOST_CHECK( DocKindFromExtension( wxS( "a." ) + prefix + wxS( "_mod" ) ) == DOC_KIND::FOOTPRINT );
    }
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( CliVisualDiff )


BOOST_AUTO_TEST_CASE( RejectsInvalidVisualDiffArguments )
{
    TEMP_DIR     dir;
    CLI_FIXTURES fixtures( dir );

    const std::vector<CLI_CASE> cases = {
        { wxS( "pcb" ), wxS( "diff" ), fixtures.pcbA, fixtures.pcbB },
        { wxS( "sch" ), wxS( "diff" ), fixtures.schA, fixtures.schB },
        { wxS( "fp" ), wxS( "diff" ), fixtures.fpA, fixtures.fpB },
        { wxS( "sym" ), wxS( "diff" ), fixtures.symA, fixtures.symB },
    };

    for( const CLI_CASE& c : cases )
    {
        BOOST_TEST_CONTEXT( c.commandGroup )
        {
            expectInvalidExit( wxS( "invalid format" ),
                               runCli( { c.commandGroup, c.commandName, c.refPath, c.refPath, wxS( "--format" ),
                                         wxS( "bogus" ) } ),
                               1 );

            expectInvalidExit( wxS( "svg requires output" ),
                               runCli( { c.commandGroup, c.commandName, c.refPath, c.refPath, wxS( "--format" ),
                                         wxS( "svg" ) } ),
                               1 );

            expectInvalidExit( wxS( "png requires output" ),
                               runCli( { c.commandGroup, c.commandName, c.refPath, c.refPath, wxS( "--format" ),
                                         wxS( "png" ) } ),
                               1 );
        }
    }
}


BOOST_AUTO_TEST_CASE( RendersChangedPngAndSvgContentForEveryCommand )
{
    TEMP_DIR     dir;
    CLI_FIXTURES fixtures( dir );

    const std::vector<CLI_CASE> cases = {
        { wxS( "pcb" ), wxS( "diff" ), fixtures.pcbA, fixtures.pcbB },
        { wxS( "sch" ), wxS( "diff" ), fixtures.schA, fixtures.schB },
        { wxS( "fp" ), wxS( "diff" ), fixtures.fpA, fixtures.fpB },
        { wxS( "sym" ), wxS( "diff" ), fixtures.symA, fixtures.symB },
    };

    for( const CLI_CASE& c : cases )
    {
        expectVisualDifference( dir, c, wxS( "svg" ) );
        expectVisualDifference( dir, c, wxS( "png" ) );
    }
}


BOOST_AUTO_TEST_CASE( GitMergeDriverWritesNativeBlobsAndRejectsOfficialIdentity )
{
    TEMP_DIR dir;
    CLI_FIXTURES fixtures( dir );
    for( const auto& item : { std::make_pair( wxS( "pcb" ), fixtures.pcbA ),
                             std::make_pair( wxS( "sch" ), fixtures.schA ),
                             std::make_pair( wxS( "sym" ), fixtures.symA ),
                             std::make_pair( wxS( "fp" ), fixtures.fpB + wxFILE_SEP_PATH
                                                        + wxS( "VisualDiff.kicad_mod" ) ) } )
    {
        const wxString extension = item.first == wxS( "fp" ) ? wxS( "mod" ) : item.first;
        const wxString output = dir.Path( wxS( "merged.kemi_" ) + extension );
        const std::string before = KI_TEST::LoadStringData( item.second );
        COMMAND_RESULT result = runCli( { wxS( "git-mergedriver" ), wxS( "--kind" ), item.first,
                item.second, item.second, item.second, wxS( "--output" ), output } );
        BOOST_CHECK_MESSAGE( result.exitCode == 0, result.error );
        BOOST_REQUIRE( wxFileExists( output ) );
        BOOST_CHECK( KI_TEST::LoadStringData( output ).starts_with( "(kemi_" ) );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( item.second ), before );

        const wxString blob = dir.Path( wxS( "git-blob-" ) + item.first );
        BOOST_REQUIRE( wxCopyFile( output, blob ) );
        result = runCli( { wxS( "git-mergedriver" ), wxS( "--kind" ), item.first,
                blob, blob, blob, wxS( "--path" ), output, wxS( "--output" ), blob } );
        BOOST_CHECK_MESSAGE( result.exitCode == 0, result.error );
        BOOST_CHECK( KI_TEST::LoadStringData( blob ).starts_with( "(kemi_" ) );

        result = runCli( { wxS( "git-mergedriver" ), wxS( "--kind" ), item.first,
                item.second, item.second, item.second, wxS( "--output" ), item.second } );
        BOOST_CHECK_EQUAL( result.exitCode, CLI::EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( item.second ), before );
    }
}


BOOST_AUTO_TEST_CASE( GitMergeDriverRejectsSymlinksToOfficialSources )
{
    TEMP_DIR dir;
    CLI_FIXTURES fixtures( dir );
    const std::string before = KI_TEST::LoadStringData( fixtures.pcbA );

    for( const wxString& name : { wxS( "linked.kemi_pcb" ), wxS( "git-blob" ) } )
    {
        const wxString link = dir.Path( name );
        std::error_code error;
        std::filesystem::create_symlink( std::filesystem::path( fixtures.pcbA.wc_str() ),
                                        std::filesystem::path( link.wc_str() ), error );

        if( error )
        {
            BOOST_TEST_MESSAGE( "Symlink creation unavailable: " << error.message() );
            return;
        }

        COMMAND_RESULT result = runCli( { wxS( "git-mergedriver" ), wxS( "--kind" ), wxS( "pcb" ),
                fixtures.pcbA, fixtures.pcbA, fixtures.pcbA,
                wxS( "--path" ), dir.Path( wxS( "board.kemi_pcb" ) ), wxS( "--output" ), link } );
        BOOST_CHECK_EQUAL( result.exitCode, CLI::EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( fixtures.pcbA ), before );
    }
}


BOOST_AUTO_TEST_CASE( SymbolUpgradeUsesNativeDefaultAndPreservesOfficialSource )
{
    TEMP_DIR dir;
    CLI_FIXTURES fixtures( dir );
    const std::string before = KI_TEST::LoadStringData( fixtures.symA );
    const wxString native = dir.Path( wxS( "sym_a.kemi_sym" ) );
    COMMAND_RESULT result = runCli( { wxS( "sym" ), wxS( "upgrade" ), wxS( "--force" ), fixtures.symA } );
    BOOST_CHECK_MESSAGE( result.exitCode == 0, result.error );
    BOOST_REQUIRE( wxFileExists( native ) );
    const std::string saved = KI_TEST::LoadStringData( native );
    BOOST_CHECK( saved.starts_with( "(kemi_symbol_lib" ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( fixtures.symA ), before );

    result = runCli( { wxS( "sym" ), wxS( "upgrade" ), wxS( "--force" ), fixtures.symA } );
    BOOST_CHECK_EQUAL( result.exitCode, CLI::EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( native ), saved );

    result = runCli( { wxS( "sym" ), wxS( "upgrade" ), wxS( "--force" ), native } );
    BOOST_CHECK_MESSAGE( result.exitCode == 0, result.error );
    BOOST_CHECK( KI_TEST::LoadStringData( native ).starts_with( "(kemi_symbol_lib" ) );

    const wxString official = dir.Path( wxS( "rejected.kicad_sym" ) );
    result = runCli( { wxS( "sym" ), wxS( "upgrade" ), wxS( "--force" ), fixtures.symA,
                       wxS( "--output" ), official } );
    BOOST_CHECK_EQUAL( result.exitCode, CLI::EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT );
    BOOST_CHECK( !wxFileExists( official ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( fixtures.symA ), before );
}


BOOST_AUTO_TEST_SUITE_END()
