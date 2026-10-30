/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <base_units.h>
#include <board.h>
#include <board_design_settings.h>
#include <board_loader.h>
#include <cli/exit_codes.h>
#include <drawing_sheet/ds_data_model.h>
#include <drc/rule_editor/drc_re_rule_loader.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <gestfich.h>
#include <jobs/job_pcb_import.h>
#include <json_common.h>
#include <kemi_format_profile.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <pad.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <pcb_io/pads/pcb_io_pads.h>
#include <pcb_io/pads/pcb_io_pads_binary.h>
#include <pcb_track.h>
#include <pcbnew_jobs_handler.h>
#include <pcbnew_utils/board_file_utils.h>
#include <pgm_base.h>
#include <project.h>
#include <project/project_local_settings.h>
#include <project_pcb.h>
#include <reporter.h>
#include <settings/settings_manager.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <wx/ffile.h>
#include <wx/filefn.h>


namespace
{
using DIRECTORY_SNAPSHOT = std::map<std::filesystem::path, std::string>;


std::string readBytes( const wxString& aPath )
{
    std::ifstream file( ToFsPath( aPath ), std::ios::binary );
    BOOST_REQUIRE( file.good() );
    return { std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() };
}


void writeText( const wxString& aPath, const wxString& aText )
{
    wxFFile file( aPath, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE( file.Write( aText, wxConvUTF8 ) );
    BOOST_REQUIRE( file.Close() );
}


DIRECTORY_SNAPSHOT snapshotDirectory( const wxString& aDirectory )
{
    const std::filesystem::path directory = ToFsPath( aDirectory );
    DIRECTORY_SNAPSHOT snapshot;

    for( const auto& entry : std::filesystem::recursive_directory_iterator( directory ) )
    {
        if( entry.is_regular_file() )
            snapshot.emplace( entry.path().lexically_relative( directory ), readBytes( FromFsPath( entry.path() ) ) );
    }

    return snapshot;
}


void checkNoOfficialFiles( const wxString& aDirectory )
{
    for( const auto& entry : std::filesystem::recursive_directory_iterator( ToFsPath( aDirectory ) ) )
    {
        // Match the whole filename, including official staging names and project lock files.
        BOOST_CHECK_MESSAGE( !FromFsPath( entry.path().filename() ).Lower().Contains( wxS( ".kicad_" ) ),
                             "Official-format output: " << entry.path() );
    }
}


void checkKemiDocument( const wxString& aPath, KEMI_FORMAT::DOC aDoc )
{
    const std::string bytes = readBytes( aPath );
    const std::string root = "(" + KEMI_FORMAT::RootToken( aDoc, KEMI_FORMAT::FLAVOR::KEMI );
    BOOST_CHECK_EQUAL( bytes.find( root ), 0u );
    BOOST_CHECK( bytes.find( "(format_version " + std::to_string( KEMI_FORMAT::FORMAT_VERSION ) + ")" )
                 != std::string::npos );
}


struct BOARD_PROJECT_GUARD
{
    explicit BOARD_PROJECT_GUARD( BOARD& aBoard ) : board( aBoard ) {}
    ~BOARD_PROJECT_GUARD() { board.ClearProject(); }

    BOARD& board;
};


struct KEMI_PCB_IMPORT_FIXTURE
{
    KEMI_PCB_IMPORT_FIXTURE() :
            m_directory( wxS( "kemi-pcb-import" ) ),
            m_sourceDirectory( m_directory.CreateChildDirStr( wxS( "source" ) ) ),
            m_outputDirectory( m_directory.CreateChildDirStr( wxS( "output" ) ) )
    {
        DS_DATA_MODEL::SetAltInstance( &m_drawingSheet );
    }

    ~KEMI_PCB_IMPORT_FIXTURE()
    {
        if( m_project )
            Pgm().GetSettingsManager().UnloadProject( m_project, false );

        DS_DATA_MODEL::SetAltInstance( m_previousDrawingSheet );
    }

    void loadProject( const wxString& aDirectory, const wxString& aStem )
    {
        const wxString path = wxFileName( aDirectory, aStem + wxS( ".kemi_pro" ) ).GetFullPath();
        SETTINGS_MANAGER& settings = Pgm().GetSettingsManager();
        settings.LoadProject( path, true );
        m_project = settings.GetProject( path );
        BOOST_REQUIRE( m_project );
        const wxString boardPath = wxFileName( aDirectory, aStem + wxS( ".kemi_pcb" ) ).GetFullPath();
        BOOST_CHECK_EQUAL( settings.GetProject( boardPath ), m_project );
        Pgm().GetLibraryManager().LoadProjectTables( aDirectory );
    }

    wxString stageSample( const wxString& aRelativePath, const wxString& aDirectory )
    {
        const wxString source = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() )
                                + wxS( "plugins/pads/" ) + aRelativePath;
        const wxString staged = wxFileName( aDirectory, wxFileName( source ).GetFullName() ).GetFullPath();
        BOOST_REQUIRE( wxCopyFile( source, staged ) );

        if( wxFileName( staged ).GetExt().Lower() == wxS( "asc" ) )
        {
            wxString text = KI_TEST::LoadStringData( staged );

            // Exercise rule persistence with real pads/nets, rather than a footprint-free rule fixture.
            BOOST_REQUIRE( text.Replace( wxS( "*END*" ),
                    wxS( "*DIFFPAIR*\nDIFFPAIR KemiImportPair RXP1 RXN1 100000 100000\n*END*" ) ) );
            writeText( staged, text );
        }

        return staged;
    }

    void checkCache( BOARD& aBoard, const wxString& aDirectory )
    {
        FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( m_project );
        BOOST_REQUIRE( adapter );
        LIBRARY_TABLE* table = adapter->ProjectTable().value_or( nullptr );
        BOOST_REQUIRE( table );
        BOOST_REQUIRE( wxFileExists( wxFileName( aDirectory, wxS( "fp-lib-table" ) ).GetFullPath() ) );
        size_t count = 0;

        for( const auto& entry : std::filesystem::recursive_directory_iterator( ToFsPath( aDirectory ) ) )
        {
            if( entry.path().extension() != ".kemi_mod" )
                continue;

            BOOST_CHECK( entry.path().parent_path().extension() == ".pretty" );
            checkKemiDocument( FromFsPath( entry.path() ), KEMI_FORMAT::DOC::FOOTPRINT );
            PCB_IO_KICAD_SEXPR reader;
            std::unique_ptr<FOOTPRINT> footprint = reader.FootprintLoad(
                    FromFsPath( entry.path().parent_path() ), FromFsPath( entry.path().stem() ) );
            BOOST_REQUIRE( footprint );
            ++count;
        }

        BOOST_CHECK_GT( count, 0u );

        for( FOOTPRINT* footprint : aBoard.Footprints() )
        {
            const wxString nickname = footprint->GetFPID().GetUniStringLibNickname();
            BOOST_REQUIRE( !nickname.IsEmpty() );
            LIBRARY_TABLE_ROW* row = table->Row( nickname ).value_or( nullptr );
            BOOST_REQUIRE( row );
            BOOST_CHECK_EQUAL( row->Type(), wxS( "KiCad" ) );
            BOOST_CHECK_EQUAL( row->URI(), wxS( "${KIPRJMOD}/" ) + nickname + wxS( ".pretty" ) );
            const wxString library = wxFileName( aDirectory, nickname + wxS( ".pretty" ) ).GetFullPath();
            const wxString name = footprint->GetFPID().GetUniStringLibItemName();
            BOOST_CHECK( wxFileExists( wxFileName( library, name + wxS( ".kemi_mod" ) ).GetFullPath() ) );
        }
    }

    template<typename PLUGIN>
    void importAndRoundTrip( const wxString& aRelativePath, bool aDefaultOutput )
    {
        const wxString input = stageSample( aRelativePath, aDefaultOutput ? m_outputDirectory : m_sourceDirectory );
        const std::string inputBefore = readBytes( input );
        const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );
        const wxString stem = aDefaultOutput ? wxFileName( input ).GetName() : wxString( wxS( "converted" ) );
        const wxString output = wxFileName( m_outputDirectory, stem + wxS( ".kemi_pcb" ) ).GetFullPath();

        // The default-output case must exercise JobImport's own project creation and teardown.
        if( !aDefaultOutput )
            loadProject( m_outputDirectory, stem );

        PLUGIN importer;
        std::unique_ptr<BOARD> expected = importer.LoadBoard( input );
        BOOST_REQUIRE( expected );
        BOOST_REQUIRE_GT( expected->Footprints().size(), 0u );
        BOOST_REQUIRE_GT( expected->GetNetCount(), 0u );
        DRC_RULE_LOADER memoryLoader;
        const auto expectedRules = memoryLoader.LoadFromString( importer.GetImportedDesignRules(),
                                                                wxS( "imported.kicad_dru" ) );
        BOOST_REQUIRE_MESSAGE( memoryLoader.GetLoadError().IsEmpty(), memoryLoader.GetLoadError() );
        BOOST_REQUIRE( !expectedRules.empty() );

        JOB_PCB_IMPORT job;
        job.m_inputFile = input;
        job.m_format = JOB_PCB_IMPORT::FORMAT::PADS_ASCII;

        if( !aDefaultOutput )
            job.SetConfiguredOutputPath( output );

        WX_STRING_REPORTER reporter;
        PCBNEW_JOBS_HANDLER handler( nullptr );
        handler.SetReporter( &reporter );
        BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );
        checkNoOfficialFiles( m_outputDirectory );
        BOOST_REQUIRE( wxFileExists( output ) );
        checkKemiDocument( output, KEMI_FORMAT::DOC::PCB );

        for( const auto& document : { KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::DOC::PROJECT_LOCAL } )
        {
            const wxString path = wxFileName( m_outputDirectory, stem + wxS( "." )
                                             + KEMI_FORMAT::Ext( document, KEMI_FORMAT::FLAVOR::KEMI ) ).GetFullPath();
            BOOST_REQUIRE( wxFileExists( path ) );
            const nlohmann::json json = nlohmann::json::parse( readBytes( path ) );
            BOOST_CHECK( json.contains( "kemicad" ) );
            BOOST_CHECK( KEMI_FORMAT::ValidateJsonMarker( json, document, path ).IsEmpty() );
        }

        if( aDefaultOutput )
            loadProject( m_outputDirectory, stem );

        std::unique_ptr<BOARD> loaded = BOARD_LOADER::Load( output, PCB_IO_MGR::KICAD_SEXP, m_project );
        BOOST_REQUIRE( loaded );
        BOARD_PROJECT_GUARD projectGuard( *loaded );
        BOOST_CHECK_EQUAL( loaded->Footprints().size(), expected->Footprints().size() );
        BOOST_CHECK_EQUAL( loaded->GetNetCount(), expected->GetNetCount() );
        checkCache( *loaded, m_outputDirectory );

        const wxString rules = wxFileName( m_outputDirectory, stem + wxS( ".kemi_dru" ) ).GetFullPath();
        BOOST_CHECK_EQUAL( loaded->GetDesignRulesPath(), rules );
        DRC_RULE_LOADER loader;
        const auto actualRules = loader.LoadFile( loaded->GetDesignRulesPath() );
        BOOST_REQUIRE_MESSAGE( loader.GetLoadError().IsEmpty(), loader.GetLoadError() );
        BOOST_REQUIRE_EQUAL( actualRules.size(), expectedRules.size() );

        for( size_t i = 0; i < actualRules.size(); ++i )
        {
            BOOST_CHECK_EQUAL( actualRules[i].ruleName, expectedRules[i].ruleName );
            BOOST_CHECK_EQUAL( actualRules[i].originalRuleText, expectedRules[i].originalRuleText );
        }

        BOOST_CHECK_EQUAL( readBytes( input ), inputBefore );
        BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
        checkNoOfficialFiles( m_sourceDirectory );
        checkNoOfficialFiles( m_outputDirectory );
    }

    KI_TEST::SCOPED_TEMP_DIR m_directory;
    wxString                 m_sourceDirectory;
    wxString                 m_outputDirectory;
    PROJECT*                 m_project = nullptr;
    DS_DATA_MODEL*           m_previousDrawingSheet = &DS_DATA_MODEL::GetTheInstance();
    DS_DATA_MODEL            m_drawingSheet;
};
} // namespace


BOOST_FIXTURE_TEST_SUITE( KemiPcbImport, KEMI_PCB_IMPORT_FIXTURE )

BOOST_AUTO_TEST_CASE( RulesConflictPreservesExistingBoardAndCreatesNoCache )
{
    const wxString input = stageSample( wxS( "Ems4_Rev2/Ems4_Rev2.asc" ), m_sourceDirectory );
    loadProject( m_outputDirectory, wxS( "converted" ) );
    const wxString output = wxFileName( m_outputDirectory, wxS( "converted.kemi_pcb" ) ).GetFullPath();
    const wxString rules = wxFileName( m_outputDirectory, wxS( "converted.kemi_dru" ) ).GetFullPath();
    writeText( rules, wxS( "(kemi_dru (format_version 1) (version 2))\n" ) );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );
    WX_STRING_REPORTER reporter;
    PCBNEW_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );

    // Cover both a fresh output and a pre-existing board that must not be replaced.
    for( bool existingBoard : { false, true } )
    {
        if( existingBoard )
            writeText( output, wxS( "original board bytes\n" ) );

        const DIRECTORY_SNAPSHOT before = snapshotDirectory( m_outputDirectory );
        JOB_PCB_IMPORT job;
        job.m_inputFile = input;
        job.m_format = JOB_PCB_IMPORT::FORMAT::PADS_ASCII;
        job.SetConfiguredOutputPath( output );
        BOOST_CHECK_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::ERR_UNKNOWN );
        BOOST_CHECK( snapshotDirectory( m_outputDirectory ) == before );
        BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
    }

    BOOST_CHECK( reporter.GetMessages().Contains( wxS( "already exists" ) ) );
}

// PI01: ASCII uses the CLI's default path and writes every native artifact directly as Kemi.
BOOST_AUTO_TEST_CASE( PadsAsciiDefaultOutputRoundTrip )
{
    importAndRoundTrip<PCB_IO_PADS>( wxS( "Ems4_Rev2/Ems4_Rev2.asc" ), true );
}


// PI-CONNECT01: The first CLI save must already contain the GUI's propagated track/via nets.
BOOST_AUTO_TEST_CASE( FirstImportSaveContainsStableConnectedNets )
{
    const wxString input = wxFileName( m_sourceDirectory, wxS( "source.kemi_pcb" ) ).GetFullPath();
    const wxString output = wxFileName( m_outputDirectory, wxS( "converted.kemi_pcb" ) ).GetFullPath();
    loadProject( m_outputDirectory, wxS( "converted" ) );

    BOARD source;
    source.SetCopperLayerCount( 2 );
    NETINFO_ITEM* signal = new NETINFO_ITEM( &source, wxS( "SIGNAL" ), 1 );
    NETINFO_ITEM* ground = new NETINFO_ITEM( &source, wxS( "AGND" ), 2 );
    source.Add( signal );
    source.Add( ground );

    const VECTOR2I padPosition( pcbIUScale.mmToIU( 10.0 ), pcbIUScale.mmToIU( 10.0 ) );
    const VECTOR2I viaPosition( pcbIUScale.mmToIU( 15.0 ), pcbIUScale.mmToIU( 10.0 ) );
    const VECTOR2I isolatedPosition( pcbIUScale.mmToIU( 20.0 ), pcbIUScale.mmToIU( 20.0 ) );
    FOOTPRINT* footprint = new FOOTPRINT( &source );
    footprint->SetReference( wxS( "U1" ) );
    footprint->SetPosition( padPosition );
    PAD* pad = new PAD( footprint );
    pad->SetAttribute( PAD_ATTRIB::SMD );
    pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::RECTANGLE );
    pad->SetSize( PADSTACK::ALL_LAYERS,
                  VECTOR2I( pcbIUScale.mmToIU( 1.0 ), pcbIUScale.mmToIU( 1.0 ) ) );
    pad->SetLayerSet( LSET( { F_Cu, F_Mask, F_Paste } ) );
    pad->SetPosition( padPosition );
    pad->SetNumber( wxS( "1" ) );
    pad->SetNet( signal );
    footprint->Add( pad );
    source.Add( footprint );

    // The pad is the net authority; the touching track/via deliberately carry a stale net.
    PCB_TRACK* track = new PCB_TRACK( &source );
    track->SetStart( padPosition );
    track->SetEnd( viaPosition );
    track->SetLayer( F_Cu );
    track->SetWidth( pcbIUScale.mmToIU( 0.2 ) );
    track->SetNet( ground );
    source.Add( track );

    std::array<PCB_VIA*, 2> vias;

    for( size_t i = 0; i < vias.size(); ++i )
    {
        PCB_VIA* via = new PCB_VIA( &source );
        via->SetPosition( i == 0 ? viaPosition : isolatedPosition );
        via->SetLayerPair( F_Cu, B_Cu );
        via->SetDrill( pcbIUScale.mmToIU( 0.3 ) );
        via->SetWidth( PADSTACK::ALL_LAYERS, pcbIUScale.mmToIU( 0.6 ) );
        via->SetNet( ground );
        source.Add( via );
        vias[i] = via;
    }

    using ITEM_NETS = std::map<wxString, wxString>;
    auto itemNets = []( const BOARD& aBoard )
    {
        ITEM_NETS nets;

        for( const PCB_TRACK* item : aBoard.Tracks() )
            nets.emplace( item->m_Uuid.AsString(), item->GetNetname() );

        for( const FOOTPRINT* fp : aBoard.Footprints() )
        {
            for( const PAD* item : fp->Pads() )
                nets.emplace( item->m_Uuid.AsString(), item->GetNetname() );
        }

        return nets;
    };

    const ITEM_NETS inputNets = itemNets( source );
    const ITEM_NETS expectedNets = {
        { pad->m_Uuid.AsString(), wxS( "SIGNAL" ) },
        { track->m_Uuid.AsString(), wxS( "SIGNAL" ) },
        { vias[0]->m_Uuid.AsString(), wxS( "SIGNAL" ) },
        { vias[1]->m_Uuid.AsString(), wxS( "AGND" ) }
    };
    BOOST_REQUIRE( inputNets != expectedNets );

    auto checkNets = [&]( const BOARD& aBoard )
    {
        const ITEM_NETS actual = itemNets( aBoard );
        BOOST_REQUIRE_EQUAL( actual.size(), expectedNets.size() );

        for( const auto& [id, net] : expectedNets )
        {
            BOOST_TEST_CONTEXT( "Item " << id )
            {
                const auto found = actual.find( id );
                BOOST_REQUIRE( found != actual.end() );
                BOOST_CHECK_EQUAL( found->second, net );
            }
        }
    };

    // A direct serializer preserves the deliberately stale input; BOARD_LOADER would repair it.
    PCB_IO_KICAD_SEXPR io;
    io.SaveBoard( input, source );
    std::unique_ptr<BOARD> rawInput = io.LoadBoard( input );
    BOOST_REQUIRE( rawInput );
    BOOST_REQUIRE( itemNets( *rawInput ) == inputNets );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );

    JOB_PCB_IMPORT job;
    job.m_inputFile = input;
    job.m_format = JOB_PCB_IMPORT::FORMAT::AUTO;
    job.SetConfiguredOutputPath( output );
    WX_STRING_REPORTER reporter;
    PCBNEW_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );

    std::unique_ptr<BOARD> loaded = io.LoadBoard( output );
    BOOST_REQUIRE( loaded );
    checkNets( *loaded );
    BOARD_PROJECT_GUARD projectGuard( *loaded );
    loaded->SetProject( m_project );

    for( int rebuild = 0; rebuild < 2; ++rebuild )
    {
        BOOST_REQUIRE( loaded->BuildConnectivity() );
        checkNets( *loaded );
    }

    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
    checkKemiDocument( output, KEMI_FORMAT::DOC::PCB );
    checkNoOfficialFiles( m_directory.PathStr() );
}


// GI-PADS-PIN-MAP01: One decal can have different logical pin names in placed instances.
BOOST_AUTO_TEST_CASE( NamedPinVariantsKeepDistinctNativeFootprints )
{
    const wxString input = stageSample( wxS( "synthetic_named_route_pins.asc" ), m_sourceDirectory );
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( m_sourceDirectory );
    const wxString output = wxFileName( m_outputDirectory, wxS( "converted.kemi_pcb" ) ).GetFullPath();
    loadProject( m_outputDirectory, wxS( "converted" ) );

    JOB_PCB_IMPORT job;
    job.m_inputFile = input;
    job.m_format = JOB_PCB_IMPORT::FORMAT::PADS_ASCII;
    job.SetConfiguredOutputPath( output );
    WX_STRING_REPORTER reporter;
    PCBNEW_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );

    PCB_IO_KICAD_SEXPR native;
    std::unique_ptr<BOARD> board = native.LoadBoard( output );
    BOOST_REQUIRE( board );
    const std::map<wxString, std::set<wxString>> expected = {
        { "QTOP", { "", "G", "S", "D" } },
        { "QBOT", { "", "G", "S", "D" } },
        { "QNUM", { "", "1", "2", "3" } },
        { "QALT", { "", "A", "B", "C" } },
        { "QCONFLICT", { "", "1", "2", "3" } },
        { "QAMBIG", { "1", "2" } },
        { "QBOUNDIN", { "", "A", "2", "3" } },
        { "QBOUNDOUT", { "", "1", "2", "3" } }
    };
    std::map<wxString, wxString> ids;
    BOOST_REQUIRE_EQUAL( board->Footprints().size(), expected.size() );

    for( FOOTPRINT* footprint : board->Footprints() )
    {
        BOOST_TEST_CONTEXT( footprint->GetReference() )
        {
            const auto found = expected.find( footprint->GetReference() );
            BOOST_REQUIRE( found != expected.end() );
            const wxString nickname = footprint->GetFPID().GetUniStringLibNickname();
            const wxString name = footprint->GetFPID().GetUniStringLibItemName();
            BOOST_REQUIRE( !nickname.IsEmpty() );
            const wxString library = wxFileName( m_outputDirectory, nickname + wxS( ".pretty" ) ).GetFullPath();
            const wxString member = wxFileName( library, name + wxS( ".kemi_mod" ) ).GetFullPath();
            BOOST_REQUIRE( wxFileExists( member ) );
            checkKemiDocument( member, KEMI_FORMAT::DOC::FOOTPRINT );
            std::unique_ptr<FOOTPRINT> cached = native.FootprintLoad( library, name );
            BOOST_REQUIRE( cached );
            std::set<wxString> boardNumbers;
            std::set<wxString> cachedNumbers;

            for( const PAD* pad : footprint->Pads() )
                boardNumbers.insert( pad->GetNumber() );

            for( const PAD* pad : cached->Pads() )
                cachedNumbers.insert( pad->GetNumber() );

            BOOST_CHECK( boardNumbers == found->second );
            BOOST_CHECK( cachedNumbers == found->second );
            BOOST_CHECK( !footprint->FootprintNeedsUpdate( cached.get(), BOARD_ITEM::COMPARE_FLAGS::DRC ) );
            ids.emplace( footprint->GetReference(), footprint->GetFPID().Format() );
        }
    }

    BOOST_CHECK( ids.at( "QTOP" ) != ids.at( "QNUM" ) );
    BOOST_CHECK( ids.at( "QTOP" ) != ids.at( "QALT" ) );
    BOOST_CHECK( ids.at( "QALT" ) != ids.at( "QNUM" ) );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == before );
    checkKemiDocument( output, KEMI_FORMAT::DOC::PCB );
    checkNoOfficialFiles( m_directory.PathStr() );
}


// GI-PADS-PIN-MAP01: Shared decals retain complete per-part-type pin maps in native libraries.
BOOST_AUTO_TEST_CASE( AlphanumericPinMapsKeepDistinctNativeFootprints )
{
    const wxString input = stageSample( wxS( "synthetic_alphanumeric_pin_map.asc" ), m_sourceDirectory );
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( m_sourceDirectory );
    const wxString output = wxFileName( m_outputDirectory, wxS( "converted.kemi_pcb" ) ).GetFullPath();
    loadProject( m_outputDirectory, wxS( "converted" ) );

    JOB_PCB_IMPORT job;
    job.m_inputFile = input;
    job.m_format = JOB_PCB_IMPORT::FORMAT::PADS_ASCII;
    job.SetConfiguredOutputPath( output );
    WX_STRING_REPORTER reporter;
    PCBNEW_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );

    PCB_IO_KICAD_SEXPR native;
    std::unique_ptr<BOARD> board = native.LoadBoard( output );
    BOOST_REQUIRE( board );
    const std::set<wxString> numeric = { "", "1", "2", "3" };
    const std::map<wxString, std::set<wxString>> expected = {
        { "QFULL", { "", "G", "S", "D" } },
        { "QFLIP", { "", "G", "S", "D" } },
        { "QALT", { "", "A", "B", "C" } },
        { "QNUM", numeric }, { "QCOUNT", numeric }, { "QDUP", numeric },
        { "QGAP", { "1", "2", "3", "5" } }, { "QCLASH", numeric }, { "QNETNUM", numeric },
        { "QAMBIG", { "1", "2", "3", "4" } },
        { "QTRUNC", numeric }, { "QLONG", numeric }, { "QEXTRA", numeric }
    };
    std::map<wxString, wxString> ids;
    BOOST_REQUIRE_EQUAL( board->Footprints().size(), expected.size() );

    for( FOOTPRINT* footprint : board->Footprints() )
    {
        BOOST_TEST_CONTEXT( footprint->GetReference() )
        {
            const auto found = expected.find( footprint->GetReference() );
            BOOST_REQUIRE( found != expected.end() );
            const wxString nickname = footprint->GetFPID().GetUniStringLibNickname();
            const wxString name = footprint->GetFPID().GetUniStringLibItemName();
            BOOST_REQUIRE( !nickname.IsEmpty() );
            const wxString library = wxFileName( m_outputDirectory, nickname + wxS( ".pretty" ) ).GetFullPath();
            const wxString member = wxFileName( library, name + wxS( ".kemi_mod" ) ).GetFullPath();
            BOOST_REQUIRE( wxFileExists( member ) );
            checkKemiDocument( member, KEMI_FORMAT::DOC::FOOTPRINT );
            std::unique_ptr<FOOTPRINT> cached = native.FootprintLoad( library, name );
            BOOST_REQUIRE( cached );
            std::set<wxString> boardNumbers;
            std::set<wxString> cachedNumbers;

            for( const PAD* pad : footprint->Pads() )
                boardNumbers.insert( pad->GetNumber() );

            for( const PAD* pad : cached->Pads() )
                cachedNumbers.insert( pad->GetNumber() );

            BOOST_CHECK( boardNumbers == found->second );
            BOOST_CHECK( cachedNumbers == found->second );
            BOOST_CHECK( !footprint->FootprintNeedsUpdate( cached.get(), BOARD_ITEM::COMPARE_FLAGS::DRC ) );
            ids.emplace( footprint->GetReference(), footprint->GetFPID().Format() );
        }
    }

    BOOST_CHECK( ids.at( "QFULL" ) != ids.at( "QNUM" ) );
    BOOST_CHECK( ids.at( "QFULL" ) != ids.at( "QALT" ) );
    BOOST_CHECK( ids.at( "QALT" ) != ids.at( "QNUM" ) );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == before );
    checkKemiDocument( output, KEMI_FORMAT::DOC::PCB );
    checkNoOfficialFiles( m_directory.PathStr() );
}


// GI-PADS-PIN-MAP01: Numeric permutations must not reuse a library with the original pin positions.
BOOST_AUTO_TEST_CASE( AlphanumericMapEdgesSurviveNativeFootprintExport )
{
    const wxString input = stageSample( wxS( "synthetic_alphanumeric_pin_map_edges.asc" ), m_sourceDirectory );
    const DIRECTORY_SNAPSHOT before = snapshotDirectory( m_sourceDirectory );
    const wxString output = wxFileName( m_outputDirectory, wxS( "converted.kemi_pcb" ) ).GetFullPath();
    loadProject( m_outputDirectory, wxS( "converted" ) );

    JOB_PCB_IMPORT job;
    job.m_inputFile = input;
    job.m_format = JOB_PCB_IMPORT::FORMAT::PADS_ASCII;
    job.SetConfiguredOutputPath( output );
    WX_STRING_REPORTER reporter;
    PCBNEW_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );

    PCB_IO_KICAD_SEXPR native;
    std::unique_ptr<BOARD> board = native.LoadBoard( output );
    BOOST_REQUIRE( board );
    BOOST_REQUIRE_EQUAL( board->Footprints().size(), 4 );
    std::map<wxString, wxString> ids;

    for( FOOTPRINT* footprint : board->Footprints() )
    {
        BOOST_TEST_CONTEXT( footprint->GetReference() )
        {
            const wxString nickname = footprint->GetFPID().GetUniStringLibNickname();
            const wxString name = footprint->GetFPID().GetUniStringLibItemName();
            BOOST_REQUIRE( !nickname.IsEmpty() );
            const wxString library = wxFileName( m_outputDirectory, nickname + wxS( ".pretty" ) ).GetFullPath();
            const wxString member = wxFileName( library, name + wxS( ".kemi_mod" ) ).GetFullPath();
            BOOST_REQUIRE( wxFileExists( member ) );
            checkKemiDocument( member, KEMI_FORMAT::DOC::FOOTPRINT );
            std::unique_ptr<FOOTPRINT> cached = native.FootprintLoad( library, name );
            BOOST_REQUIRE( cached );
            BOOST_CHECK( !footprint->FootprintNeedsUpdate( cached.get(), BOARD_ITEM::COMPARE_FLAGS::DRC ) );

            if( footprint->GetReference() == "QSWAP" )
            {
                BOOST_REQUIRE_EQUAL( cached->Pads().size(), 3 );

                for( const PAD* pad : cached->Pads() )
                {
                    const int x = pad->GetPosition().x - cached->GetPosition().x;
                    const wxString expected = x == -1000000 ? "2" : x == 1000000 ? "1" : "3";
                    BOOST_CHECK_EQUAL( pad->GetNumber(), expected );
                }
            }
            else if( footprint->GetReference() == "QNEXT" )
            {
                BOOST_REQUIRE_EQUAL( cached->Pads().size(), 4 );
                std::set<wxString> numbers;

                for( const PAD* pad : cached->Pads() )
                    numbers.insert( pad->GetNumber() );

                const std::set<wxString> expected = { "A", "B", "C", "D" };
                BOOST_CHECK( numbers == expected );
            }

            ids.emplace( footprint->GetReference(), footprint->GetFPID().Format() );
        }
    }

    BOOST_CHECK( ids.at( "QSWAP" ) != ids.at( "QBASE" ) );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == before );
    checkKemiDocument( output, KEMI_FORMAT::DOC::PCB );
    checkNoOfficialFiles( m_directory.PathStr() );
}


// GI-RULES01: Replacing the GUI's empty board must not load its defaults over imported rules.
BOOST_AUTO_TEST_CASE( PadsAsciiSettingsSurviveGuiBoardReplacementAndReopen )
{
    const wxString input = stageSample( wxS( "Ems4_Rev2/Ems4_Rev2.asc" ), m_sourceDirectory );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );
    const wxString output = wxFileName( m_outputDirectory, wxS( "converted.kemi_pcb" ) ).GetFullPath();
    loadProject( m_outputDirectory, wxS( "converted" ) );

    const auto limits = []( const BOARD_DESIGN_SETTINGS& aSettings )
    {
        return std::array<int, 7>{ aSettings.m_MinClearance, aSettings.m_CopperEdgeClearance,
                                  aSettings.m_HoleToHoleMin, aSettings.m_SilkClearance,
                                  aSettings.m_MinThroughDrill, aSettings.m_TrackMinWidth,
                                  aSettings.m_ViasMinSize };
    };

    BOARD initialBoard;
    initialBoard.SetProject( m_project );
    BOARD_PROJECT_GUARD initialGuard( initialBoard );

    PCB_IO_PADS importer;
    std::unique_ptr<BOARD> imported = importer.LoadBoard( input );
    BOOST_REQUIRE( imported );
    const auto expectedLimits = limits( imported->GetDesignSettings() );
    const auto expectedVias = imported->GetDesignSettings().m_ViasDimensionsList;
    BOOST_REQUIRE( expectedLimits != limits( initialBoard.GetDesignSettings() ) );
    BOOST_REQUIRE( !expectedVias.empty() );
    BOOST_REQUIRE( initialBoard.GetDesignSettings().m_ViasDimensionsList.empty() );

    // PCB_EDIT_FRAME::SetBoard detaches its previous board before binding the imported one.
    // ClearProject flushes that board's default settings into the shared project JSON.
    initialBoard.ClearProject();
    imported->SetProject( m_project );
    BOARD_PROJECT_GUARD importedGuard( *imported );
    const auto checkSettings = [&]( const BOARD& aBoard )
    {
        const auto actualLimits = limits( aBoard.GetDesignSettings() );
        const std::array<const char*, 7> names = { "clearance", "copper edge", "hole to hole",
                                                 "silk", "through drill", "track width", "via size" };

        for( size_t i = 0; i < expectedLimits.size(); ++i )
        {
            BOOST_TEST_CONTEXT( names[i] )
            {
                BOOST_CHECK_EQUAL( actualLimits[i], expectedLimits[i] );
            }
        }

        BOOST_CHECK( aBoard.GetDesignSettings().m_ViasDimensionsList == expectedVias );
    };

    checkSettings( *imported );
    // The GUI must also skip its legacy edge-width inference for the explicit PADS clearance.
    BOOST_CHECK( imported->m_LegacyCopperEdgeClearanceLoaded );
    imported->SetFileName( output );
    PCB_IO_KICAD_SEXPR writer;
    writer.SaveBoard( output, *imported );
    SETTINGS_MANAGER& settings = Pgm().GetSettingsManager();
    BOOST_REQUIRE( settings.SaveProject( m_project->GetProjectFullName(), m_project ) );
    imported->ClearProject();
    BOOST_REQUIRE( settings.UnloadProject( m_project, false ) );
    m_project = nullptr;

    loadProject( m_outputDirectory, wxS( "converted" ) );
    std::unique_ptr<BOARD> reopened = BOARD_LOADER::Load( output, PCB_IO_MGR::KICAD_SEXP, m_project );
    BOOST_REQUIRE( reopened );
    BOARD_PROJECT_GUARD reopenedGuard( *reopened );
    checkSettings( *reopened );
    checkKemiDocument( output, KEMI_FORMAT::DOC::PCB );
    checkNoOfficialFiles( m_outputDirectory );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
}


// PI02: Vendor-format selection detects binary PADS and isolates an explicit output directory.
BOOST_AUTO_TEST_CASE( PadsBinaryExplicitOutputRoundTrip )
{
    importAndRoundTrip<PCB_IO_PADS_BINARY>( wxS( "Ems4_Rev2/Ems4_Rev2.pcb" ), false );
}


// PI10: GUI import mail and explicit CLI PADS selection share content-based dialect selection.
BOOST_AUTO_TEST_CASE( PadsGuiAndCliUseContentDetection )
{
    const wxString ascii = stageSample( wxS( "Ems4_Rev2/Ems4_Rev2.asc" ), m_sourceDirectory );
    const wxString binary = stageSample( wxS( "Ems4_Rev2/Ems4_Rev2.pcb" ), m_sourceDirectory );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );

    for( PCB_IO_MGR::PCB_FILE_T requested : { PCB_IO_MGR::PADS, PCB_IO_MGR::PADS_BINARY } )
    {
        BOOST_CHECK_EQUAL( PCB_IO_MGR::ResolveImportFileType( ascii, requested ), PCB_IO_MGR::PADS );
        BOOST_CHECK_EQUAL( PCB_IO_MGR::ResolveImportFileType( binary, requested ),
                           PCB_IO_MGR::PADS_BINARY );
    }

    BOOST_CHECK_EQUAL( PCB_IO_MGR::FindPluginTypeFromBoardPath( binary ), PCB_IO_MGR::PADS_BINARY );
    BOOST_CHECK_EQUAL( PCB_IO_MGR::ResolveImportFileType( binary, PCB_IO_MGR::EAGLE ),
                       PCB_IO_MGR::EAGLE );
    BOOST_CHECK( PCB_IO_MGR::ImportGeneratesProjectLibrary( PCB_IO_MGR::PADS ) );
    BOOST_CHECK( PCB_IO_MGR::ImportGeneratesProjectLibrary( PCB_IO_MGR::PADS_BINARY ) );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
    BOOST_CHECK( snapshotDirectory( m_outputDirectory ).empty() );
}


// PI11: The library-table conversion entrance writes Kemi footprints in an unchanged .pretty path.
BOOST_AUTO_TEST_CASE( FootprintLibraryConversionUsesKemiPolicy )
{
    const wxString source = m_directory.CreateChildDirStr( wxS( "source.pretty" ) );
    const wxString output = wxFileName( m_outputDirectory, wxS( "converted.pretty" ) ).GetFullPath();
    const wxString legacy = wxFileName( source, wxS( "Imported.kicad_mod" ) ).GetFullPath();
    FOOTPRINT footprint( nullptr );
    footprint.SetFPID( LIB_ID( wxEmptyString, wxS( "Imported" ) ) );
    PCB_IO_KICAD_SEXPR writer;
    writer.FootprintSave( legacy, &footprint );
    BOOST_REQUIRE( wxFileExists( legacy ) );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( source );
    WX_STRING_REPORTER reporter;

    BOOST_REQUIRE( PCB_IO_MGR::ConvertLibrary( {}, source, output, &reporter ) );
    const wxString converted = wxFileName( output, wxS( "Imported.kemi_mod" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileExists( converted ) );
    checkKemiDocument( converted, KEMI_FORMAT::DOC::FOOTPRINT );
    checkNoOfficialFiles( m_outputDirectory );
    BOOST_CHECK( snapshotDirectory( source ) == sourceBefore );
    PCB_IO_KICAD_SEXPR reader;
    BOOST_CHECK( reader.FootprintLoad( output, wxS( "Imported" ) ) );
}


// PI03: Reject official final/staging targets before creating files or loading a project.
BOOST_AUTO_TEST_CASE( RejectsOfficialOutputBeforeImport )
{
    const wxString input = stageSample( wxS( "Ems4_Rev2/Ems4_Rev2.asc" ), m_sourceDirectory );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );
    const DIRECTORY_SNAPSHOT outputBefore = snapshotDirectory( m_outputDirectory );

    for( const wxString& name : { wxString( "forbidden.kicad_pcb" ), wxString( "forbidden.kicad_pcb.tmp" ) } )
    {
        BOOST_TEST_CONTEXT( name )
        {
            JOB_PCB_IMPORT job;
            job.m_inputFile = input;
            job.m_format = JOB_PCB_IMPORT::FORMAT::PADS_ASCII;
            job.SetConfiguredOutputPath( wxFileName( m_outputDirectory, name ).GetFullPath() );
            WX_STRING_REPORTER reporter;
            PCBNEW_JOBS_HANDLER handler( nullptr );
            handler.SetReporter( &reporter );
            BOOST_CHECK_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::ERR_ARGS );
            BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
            BOOST_CHECK( snapshotDirectory( m_outputDirectory ) == outputBefore );
        }
    }
}


// PI04: Kemi rules take priority even for an official board; legacy-only projects still load.
BOOST_AUTO_TEST_CASE( DesignRulesPreferKemiAndReadLegacy )
{
    loadProject( m_outputDirectory, wxS( "rules" ) );
    BOARD board;
    board.SetProject( m_project );
    BOARD_PROJECT_GUARD projectGuard( board );
    board.SetFileName( wxFileName( m_outputDirectory, wxS( "rules.kemi_pcb" ) ).GetFullPath() );
    const wxString kemi = wxFileName( m_outputDirectory, wxS( "rules.kemi_dru" ) ).GetFullPath();
    const wxString legacy = wxFileName( m_outputDirectory, wxS( "rules.kicad_dru" ) ).GetFullPath();
    BOOST_CHECK_EQUAL( board.GetDesignRulesPath(), kemi );
    writeText( legacy, wxS( "(version 1)\n(rule \"legacy\" (constraint clearance (min 0.2mm)))\n" ) );
    BOOST_CHECK_EQUAL( board.GetDesignRulesPath(), legacy );
    DRC_RULE_LOADER legacyLoader;
    const auto legacyEntries = legacyLoader.LoadFile( board.GetDesignRulesPath() );
    BOOST_REQUIRE_MESSAGE( legacyLoader.GetLoadError().IsEmpty(), legacyLoader.GetLoadError() );
    BOOST_REQUIRE_EQUAL( legacyEntries.size(), 1u );
    BOOST_CHECK_EQUAL( legacyEntries.front().ruleName, wxS( "legacy" ) );
    writeText( kemi, wxS( "(kemi_dru (format_version 1) (version 1)\n"
                         "(rule \"native\" (constraint clearance (min 0.3mm))))\n" ) );
    BOOST_CHECK_EQUAL( board.GetDesignRulesPath(), kemi );
    board.SetFileName( wxFileName( m_outputDirectory, wxS( "rules.kicad_pcb" ) ).GetFullPath() );
    BOOST_CHECK_EQUAL( board.GetDesignRulesPath(), kemi );
    DRC_RULE_LOADER loader;
    const auto entries = loader.LoadFile( board.GetDesignRulesPath() );
    BOOST_REQUIRE_MESSAGE( loader.GetLoadError().IsEmpty(), loader.GetLoadError() );
    BOOST_REQUIRE_EQUAL( entries.size(), 1u );
    BOOST_CHECK_EQUAL( entries.front().ruleName, wxS( "native" ) );
}


// PI05: The first save, policy staging save and library reader all agree on Kemi footprints.
BOOST_AUTO_TEST_CASE( FootprintFirstSaveAndExplicitStagingTarget )
{
    using namespace KEMI_FORMAT;

    const wxString library = m_directory.CreateChildDirStr( wxS( "footprints.pretty" ) );
    PCB_IO_KICAD_SEXPR io;
    FOOTPRINT footprint( nullptr );
    footprint.SetFPID( LIB_ID( wxEmptyString, wxS( "A" ) ) );
    io.FootprintSave( library, &footprint );
    const wxString first = wxFileName( library, wxS( "A.kemi_mod" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileExists( first ) );
    checkKemiDocument( first, DOC::FOOTPRINT );
    checkNoOfficialFiles( library );

    KEMI_ONLY_OUTPUT_POLICY policy( library );
    footprint.SetFPID( LIB_ID( wxEmptyString, wxS( "B" ) ) );
    const wxString staging = wxFileName( library, wxS( "B.tmp" ) ).GetFullPath();
    policy.Write( DOC::FOOTPRINT, wxS( "B" ),
            [&]( const wxString& aStorage, const OUTPUT_TARGET& aTarget )
            {
                io.FootprintSave( aStorage, &footprint, aTarget );
                checkKemiDocument( aStorage, DOC::FOOTPRINT );
                checkNoOfficialFiles( library );
                return wxRenameFile( aStorage, aTarget.FinalPath(), false );
            }, staging );
    BOOST_REQUIRE_EQUAL( policy.Manifest().size(), 1u );
    BOOST_CHECK( !wxFileExists( staging ) );
    checkKemiDocument( policy.Manifest().front().FinalPath(), DOC::FOOTPRINT );
    PCB_IO_KICAD_SEXPR reader;
    std::unique_ptr<FOOTPRINT> loaded = reader.FootprintLoad( library, wxS( "B" ) );
    BOOST_REQUIRE( loaded );
    checkNoOfficialFiles( library );
}


// PI09: A standalone import must not borrow another active project's settings in the same directory.
BOOST_AUTO_TEST_CASE( SameDirectoryStandaloneImportKeepsActiveProject )
{
    loadProject( m_outputDirectory, wxS( "A" ) );
    SETTINGS_MANAGER& settings = Pgm().GetSettingsManager();
    BOOST_REQUIRE( settings.SaveProject( m_project->GetProjectFullName(), m_project ) );
    BOOST_REQUIRE( m_project->GetLocalSettings().SaveToFile( m_outputDirectory, true ) );
    const wxString activeProject = wxFileName( m_outputDirectory, wxS( "A.kemi_pro" ) ).GetFullPath();
    const wxString activeLocal = wxFileName( m_outputDirectory, wxS( "A.kemi_prl" ) ).GetFullPath();
    const std::string projectBefore = readBytes( activeProject );
    const std::string localBefore = readBytes( activeLocal );
    const wxString input = stageSample( wxS( "Ems4_Rev2/Ems4_Rev2.asc" ), m_sourceDirectory );
    const DIRECTORY_SNAPSHOT sourceBefore = snapshotDirectory( m_sourceDirectory );
    const wxString output = wxFileName( m_outputDirectory, wxS( "B.kemi_pcb" ) ).GetFullPath();
    JOB_PCB_IMPORT job;
    job.m_inputFile = input;
    job.m_format = JOB_PCB_IMPORT::FORMAT::PADS_ASCII;
    job.m_isPartOfMultiBoardProject = false;
    job.SetConfiguredOutputPath( output );
    WX_STRING_REPORTER reporter;
    PCBNEW_JOBS_HANDLER handler( nullptr );
    handler.SetReporter( &reporter );
    BOOST_REQUIRE_EQUAL( handler.JobImport( &job ), CLI::EXIT_CODES::SUCCESS );

    BOOST_CHECK_EQUAL( &settings.Prj(), m_project );
    BOOST_CHECK_EQUAL( readBytes( activeProject ), projectBefore );
    BOOST_CHECK_EQUAL( readBytes( activeLocal ), localBefore );
    BOOST_REQUIRE( wxFileExists( output ) );
    checkKemiDocument( output, KEMI_FORMAT::DOC::PCB );

    for( const wxString& extension : { wxString( "kemi_pro" ), wxString( "kemi_prl" ), wxString( "kemi_dru" ) } )
    {
        const wxString path = wxFileName( m_outputDirectory, wxS( "B." ) + extension ).GetFullPath();
        BOOST_REQUIRE( wxFileExists( path ) );
    }

    size_t footprintCount = 0;

    for( const auto& entry : std::filesystem::recursive_directory_iterator( ToFsPath( m_outputDirectory ) ) )
    {
        if( entry.path().extension() == ".kemi_mod" )
        {
            BOOST_CHECK( entry.path().parent_path().extension() == ".pretty" );
            checkKemiDocument( FromFsPath( entry.path() ), KEMI_FORMAT::DOC::FOOTPRINT );
            ++footprintCount;
        }
    }

    BOOST_CHECK_GT( footprintCount, 0u );
    BOOST_CHECK( wxFileExists( wxFileName( m_outputDirectory, wxS( "fp-lib-table" ) ).GetFullPath() ) );
    BOOST_CHECK( snapshotDirectory( m_sourceDirectory ) == sourceBefore );
    checkNoOfficialFiles( m_sourceDirectory );
    checkNoOfficialFiles( m_outputDirectory );
}

BOOST_AUTO_TEST_SUITE_END()
