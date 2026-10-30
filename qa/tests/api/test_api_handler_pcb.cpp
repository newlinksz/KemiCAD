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

#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <wx/filename.h>
#include <wx/ffile.h>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>
#include <pcbnew_utils/board_test_utils.h>

#include <api/api_handler_pcb.h>
#include <api/headless_pcb_context.h>
#include <api/board/board_commands.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/types/base_types.pb.h>
#include <jobs/job_export_pcb_odb.h>

#include <board.h>
#include <board_loader.h>
#include <connectivity/connectivity_data.h>
#include <drc/drc_rule.h>
#include <drc/drc_rule_parser.h>
#include <drc/rule_editor/drc_re_rule_saver.h>
#include <settings/settings_manager.h>
#include <zone.h>


namespace
{

/// issue5830 is a four-copper-zone board with stable, human-readable zone UUIDs.
const wxString F_CU_ZONE   = wxS( "00000000-0000-0000-0000-00005c07d704" );
const wxString B_CU_ZONE   = wxS( "00000000-0000-0000-0000-00005c07d701" );
const wxString IN1_CU_ZONE = wxS( "00000000-0000-0000-0000-00005c07d707" );
const wxString IN2_CU_ZONE = wxS( "00000000-0000-0000-0000-00005c07d70a" );


struct API_HANDLER_PCB_FIXTURE
{
    SETTINGS_MANAGER                      m_settingsManager;
    std::unique_ptr<BOARD>                m_board;
    std::shared_ptr<HEADLESS_PCB_CONTEXT> m_context;

    // The context takes ownership of the board; the returned raw pointer lets the test inspect
    // zone state after the handler runs.
    BOARD* loadBoard( const wxString& aRelPath )
    {
        KI_TEST::LoadBoard( m_settingsManager, aRelPath, m_board );

        BOARD* board = m_board.get();
        m_context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( m_board ),
                                                            &m_settingsManager.Prj(), nullptr );
        return board;
    }

    kiapi::common::ApiRequest makeRefillRequest( BOARD* aBoard, const std::vector<wxString>& aZoneIds ) const
    {
        kiapi::board::commands::RefillZones command;
        command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
        command.mutable_board()->set_board_filename(
                wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );

        for( const wxString& id : aZoneIds )
            command.add_zones()->set_value( id.ToStdString() );

        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

        return request;
    }

    ZONE* zoneByUuid( BOARD* aBoard, const wxString& aUuid ) const
    {
        for( ZONE* zone : aBoard->Zones() )
        {
            if( zone->m_Uuid.AsString() == aUuid )
                return zone;
        }

        return nullptr;
    }

    void unfillAll( BOARD* aBoard ) const
    {
        // Start from a clean slate so a positive IsFilled() result can only come from this fill
        for( ZONE* zone : aBoard->Zones() )
        {
            zone->UnFill();
            BOOST_REQUIRE( !zone->IsFilled() );
        }
    }
};


kiapi::common::ApiRequest makeBeginCommitRequest()
{
    // No header, so the pre-11.0 path assumes the PCB editor
    kiapi::common::commands::BeginCommit command;

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( command );

    return request;
}


kiapi::common::ApiRequest makeRevertRequest( BOARD* aBoard )
{
    kiapi::common::commands::RevertDocument command;
    command.mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_document()->set_board_filename( wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( command );

    return request;
}

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiHandlerPcb, API_HANDLER_PCB_FIXTURE )


BOOST_AUTO_TEST_CASE( OdbJobRequestMapsVariants )
{
    using PACKAGING = JOB_EXPORT_PCB_ODB::VARIANT_PACKAGING;
    kiapi::board::jobs::RunBoardJobExportODB request;
    request.add_variant_names( "var1" );
    request.add_variant_names( "var2" );
    request.set_variant( "legacy" );
    request.set_variant_packaging( kiapi::board::jobs::ODBVP_COMBINED );
    request.set_manufacturer_part_number_column( "MPN" );
    JOB_EXPORT_PCB_ODB job;
    ApplyOdbJobRequest( request, job );
    BOOST_REQUIRE_EQUAL( job.m_variantNames.size(), 2u );
    BOOST_CHECK_EQUAL( job.m_variantNames[0], wxS( "var1" ) );
    BOOST_CHECK_EQUAL( job.m_variantNames[1], wxS( "var2" ) );
    BOOST_CHECK( job.m_variantPackaging == PACKAGING::COMBINED );
    BOOST_CHECK_EQUAL( job.m_colMfgPn, wxString( wxS( "MPN" ) ) );

    kiapi::board::jobs::RunBoardJobExportODB legacyRequest;
    legacyRequest.set_variant( "legacy" );
    JOB_EXPORT_PCB_ODB legacyJob;
    ApplyOdbJobRequest( legacyRequest, legacyJob );
    BOOST_REQUIRE_EQUAL( legacyJob.m_variantNames.size(), 1u );
    BOOST_CHECK_EQUAL( legacyJob.m_variantNames[0], wxS( "legacy" ) );
    BOOST_CHECK( legacyJob.m_variantPackaging == PACKAGING::SEPARATE );
}


BOOST_AUTO_TEST_CASE( OdbJobRequestMapsExportOptions )
{
    kiapi::board::jobs::RunBoardJobExportODB request;
    request.set_origin( kiapi::board::jobs::ODBO_GRID );
    request.set_product_name( "board product" );
    request.mutable_content()->set_data_set( kiapi::board::jobs::FDS_ASSEMBLY );
    request.mutable_content()->set_sections( "KP" );
    request.mutable_content()->set_net_names( kiapi::board::jobs::FNNP_ANONYMIZE );

    JOB_EXPORT_PCB_ODB job;
    ApplyOdbJobRequest( request, job );
    BOOST_CHECK( job.m_origin == JOB_EXPORT_PCB_ODB::ORIGIN::GRID );
    BOOST_CHECK_EQUAL( job.m_productName, wxString( wxS( "board product" ) ) );
    BOOST_CHECK( job.m_dataSet == JOB_EXPORT_PCB_FAB::DATA_SET::ASSEMBLY );
    BOOST_REQUIRE( job.m_sections );
    BOOST_CHECK_EQUAL( *job.m_sections, wxString( wxS( "KP" ) ) );
    BOOST_CHECK( job.m_netNames == JOB_EXPORT_PCB_FAB::NET_NAMES::ANONYMIZE );

    kiapi::board::jobs::RunBoardJobExportODB legacy;
    JOB_EXPORT_PCB_ODB                       defaultJob;
    ApplyOdbJobRequest( legacy, defaultJob );
    BOOST_CHECK( defaultJob.m_origin == JOB_EXPORT_PCB_ODB::ORIGIN::ABSOLUTE_COORDS );
    BOOST_CHECK( defaultJob.m_dataSet == JOB_EXPORT_PCB_FAB::DATA_SET::USERDEF );
    BOOST_CHECK( defaultJob.m_netNames == JOB_EXPORT_PCB_FAB::NET_NAMES::INCLUDE );
}


BOOST_AUTO_TEST_CASE( RefillZonesSubset )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, { F_CU_ZONE, IN1_CU_ZONE } );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    BOOST_CHECK_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    ZONE* fCu   = zoneByUuid( board, F_CU_ZONE );
    ZONE* bCu   = zoneByUuid( board, B_CU_ZONE );
    ZONE* in1Cu = zoneByUuid( board, IN1_CU_ZONE );
    ZONE* in2Cu = zoneByUuid( board, IN2_CU_ZONE );

    BOOST_REQUIRE( fCu && bCu && in1Cu && in2Cu );

    // Exactly the requested zones must be filled; the others must be untouched.
    BOOST_CHECK( fCu->IsFilled() );
    BOOST_CHECK( in1Cu->IsFilled() );
    BOOST_CHECK( !bCu->IsFilled() );
    BOOST_CHECK( !in2Cu->IsFilled() );
}


BOOST_AUTO_TEST_CASE( RefillZonesSubsetRebuildsConnectivity )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    // Baseline ratsnest with every zone empty; the GND planes are unfilled so their pads still
    // ratsnest together.
    board->BuildConnectivity();
    const unsigned baseline = board->GetConnectivity()->GetUnconnectedCount( false );
    BOOST_REQUIRE_MESSAGE( baseline > 0, "expected an unconnected baseline with zones empty" );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, { F_CU_ZONE, IN1_CU_ZONE } );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    const unsigned afterFill = board->GetConnectivity()->GetUnconnectedCount( false );

    // Filling the GND planes bridges GND pads that previously ratsnested, so the unconnected
    // count drops.  Push cleared the ratsnest, so if the handler skipped the connectivity
    // rebuild this would read zero instead of the reduced-but-nonzero count.
    BOOST_CHECK_MESSAGE( afterFill > 0, "connectivity was cleared, not rebuilt, after the fill" );
    BOOST_CHECK_MESSAGE( afterFill < baseline,
                         "filling the GND planes should reduce the unconnected count ("
                                 << afterFill << " vs baseline " << baseline << ")" );
}


BOOST_AUTO_TEST_CASE( RefillZonesAllHeadless )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, {} );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    BOOST_CHECK_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    // With no frame the empty-zones request must fill everything synchronously
    for( ZONE* zone : board->Zones() )
        BOOST_CHECK_MESSAGE( zone->IsFilled(), "zone " << zone->m_Uuid.AsStdString() << " not filled" );
}


BOOST_AUTO_TEST_CASE( RefillZonesUnknownIdRejected )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request =
            makeRefillRequest( board, { wxS( "deadbeef-0000-0000-0000-000000000000" ) } );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


// RevertDocument reloads the board, freeing every item an in-flight client commit points at.
// The handler must refuse with AS_BUSY while a commit is open, or the pointers dangle
// (use-after-free). Latent UAF found via #24803.
BOOST_AUTO_TEST_CASE( RevertDocumentRejectedWithOpenCommit )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    // Open a client transaction, as a client staging edits would
    kiapi::common::ApiRequest beginRequest = makeBeginCommitRequest();
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    kiapi::common::ApiRequest request = makeRevertRequest( board );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );
    BOOST_CHECK( result.error().error_message().find( "commit" ) != std::string::npos );
}


// With no open commit the guard passes; the reload then needs a running editor, so a headless
// handler reports AS_UNIMPLEMENTED. This confirms the guard does not reject the normal path.
BOOST_AUTO_TEST_CASE( RevertDocumentWithoutCommitPassesGuard )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRevertRequest( board );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( SaveCopyUsesNativeTargetsAndHonorsOverwrite )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );
    const wxString source = board->GetFileName();
    const std::string original = KI_TEST::LoadStringData( source );
    KI_TEST::SCOPED_TEMP_DIR output( wxS( "api-pcb-native-copy" ) );
    const wxString native = output.ChildPathStr( wxS( "copy.kemi_pcb" ) );
    const wxString official = output.ChildPathStr( wxS( "copy.kicad_pcb" ) );
    API_HANDLER_PCB handler( m_context );

    auto saveCopy = [&]( const wxString& aPath, bool aOverwrite )
    {
        kiapi::common::commands::SaveCopyOfDocument command;
        command.mutable_document()->set_type( kiapi::common::types::DOCTYPE_PCB );
        command.mutable_document()->set_board_filename( wxFileName( source ).GetFullName().ToStdString() );
        command.set_path( aPath.ToStdString() );
        command.mutable_options()->set_include_project( false );
        command.mutable_options()->set_overwrite( aOverwrite );
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return handler.Handle( request );
    };

    API_RESULT result = saveCopy( official, true );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::AS_BAD_REQUEST );
    BOOST_CHECK( !wxFileExists( official ) );
    result = saveCopy( source, true );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::AS_BAD_REQUEST );

    result = saveCopy( native, false );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE( wxFileExists( native ) );
    const std::string firstCopy = KI_TEST::LoadStringData( native );
    result = saveCopy( native, false );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::AS_BAD_REQUEST );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( native ), firstCopy );
    result = saveCopy( native, true );
    BOOST_REQUIRE( result.has_value() );

    auto reopened = BOARD_LOADER::Load( native, PCB_IO_MGR::KICAD_SEXP, &m_settingsManager.Prj() );
    BOOST_REQUIRE( reopened );
    BOOST_CHECK( reopened->IsKemiOnly() );
    BOOST_CHECK_EQUAL( board->GetFileName(), source );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), original );
}


BOOST_AUTO_TEST_SUITE_END()


// Phase-6 P1: exercise the API writer with legacy, native and renamed native rule sources.
BOOST_AUTO_TEST_CASE( ApiCustomRulesPreservePersistentFormat )
{
    using namespace kiapi::board::commands;

    for( const wxString& scenario : { wxS( "legacy" ), wxS( "native-board" ),
                                     wxS( "native-rules" ), wxS( "renamed-rules" ) } )
    {
        BOOST_TEST_CONTEXT( scenario )
        {
            SETTINGS_MANAGER settings;
            KI_TEST::SCOPED_TEMP_PROJECT project( settings, wxS( "kemi-api-rules" ), wxS( "board" ) );
            auto board = std::make_unique<BOARD>();
            board->SetFileName( wxFileName( project.DirStr(), wxS( "board.kicad_pcb" ) ).GetFullPath() );

            if( scenario == wxS( "native-board" ) )
                board->SetKemiOnly();

            auto context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( board ),
                                                                  &project.Project(), nullptr );
            const wxString legacy = wxFileName( project.DirStr(), wxS( "board.kicad_dru" ) ).GetFullPath();
            const wxString native = wxFileName( project.DirStr(), wxS( "board.kemi_dru" ) ).GetFullPath();
            const bool nativeRules = scenario == wxS( "native-rules" ) || scenario == wxS( "renamed-rules" );
            DRC_RULE_SAVER saver;
            if( nativeRules )
            {
                BOOST_REQUIRE( saver.SaveTextFile( native, wxS( "(version 2)\n" ) ) );
            }
            else
            {
                wxFFile input( legacy, wxS( "wb" ) );
                BOOST_REQUIRE( input.IsOpened() );
                BOOST_REQUIRE( input.Write( wxS( "(version 2)\n" ), wxConvUTF8 ) );
                BOOST_REQUIRE( input.Close() );
            }

            if( scenario == wxS( "renamed-rules" ) )
                BOOST_REQUIRE( wxRenameFile( native, legacy ) );

            const bool hadLegacy = wxFileExists( legacy );
            const std::string original = hadLegacy ? KI_TEST::LoadStringData( legacy ) : std::string();
            SetCustomDesignRules command;
            command.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
            command.mutable_board()->set_board_filename( "board.kicad_pcb" );
            auto* rule = command.add_rules();
            rule->set_name( "API clearance" );
            auto* constraint = rule->add_constraints();
            constraint->set_type( kiapi::board::CRCT_CLEARANCE );
            constraint->mutable_numeric()->set_min( 200000 );
            kiapi::common::ApiRequest request;
            request.mutable_header()->set_client_name( "kicad.qa" );
            BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
            API_HANDLER_PCB handler( context );
            API_RESULT result = handler.Handle( request );
            BOOST_REQUIRE( result.has_value() );
            BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::AS_OK );
            CustomRulesResponse response;
            BOOST_REQUIRE( result->message().UnpackTo( &response ) );
            BOOST_REQUIRE_EQUAL( response.status(), CRS_VALID );
            BOOST_REQUIRE_EQUAL( response.rules_size(), 1 );
            BOOST_CHECK_EQUAL( response.rules( 0 ).name(), "API clearance" );

            const wxString content = wxString::FromUTF8( KI_TEST::LoadStringData( native ) );
            BOOST_CHECK( DRC_RULE_SAVER::IsKemiText( content ) );
            DRC_RULES_PARSER parser( content, native );
            std::vector<std::shared_ptr<DRC_RULE>> parsed;
            BOOST_CHECK_NO_THROW( parser.Parse( parsed, nullptr ) );
            BOOST_CHECK_EQUAL( parsed.size(), 1 );

            if( hadLegacy )
                BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( legacy ), original );
            else
                BOOST_CHECK( !wxFileExists( legacy ) );
        }
    }
}


BOOST_AUTO_TEST_CASE( PcbCopyRejectsOfficialProjectBeforeWritingBoard )
{
    KI_TEST::SCOPED_TEMP_DIR directory( wxS( "api-pcb-copy-project-failure" ) );
    const wxString source = directory.ChildPathStr( wxS( "source.kicad_pro" ) );
    {
        wxFFile file( source, wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( wxS( "{}" ), wxConvUTF8 ) );
    }

    SETTINGS_MANAGER settings;
    BOOST_REQUIRE( settings.LoadProject( source ) );
    PROJECT* project = settings.GetProject( source );
    BOOST_REQUIRE( project );
    auto board = std::make_unique<BOARD>();
    board->SetFileName( directory.ChildPathStr( wxS( "source.kicad_pcb" ) ) );
    auto context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( board ), project, nullptr );
    API_HANDLER_PCB handler( context );
    kiapi::common::commands::SaveCopyOfDocument command;
    command.mutable_document()->set_type( kiapi::common::types::DOCTYPE_PCB );
    command.mutable_document()->set_board_filename( "source.kicad_pcb" );
    command.set_path( directory.ChildPathStr( wxS( "copy.kemi_pcb" ) ).ToStdString() );
    command.mutable_options()->set_include_project( true );
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::AS_INTERNAL_ERROR );
    BOOST_CHECK( !wxFileExists( directory.ChildPathStr( wxS( "copy.kemi_pcb" ) ) ) );
    BOOST_CHECK( !wxFileExists( directory.ChildPathStr( wxS( "copy.kemi_pro" ) ) ) );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), "{}" );
}
