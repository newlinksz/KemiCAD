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
 */

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <jobs/jobset.h>
#include <json_common.h>
#include <kemi_format_profile.h>
#include <ki_exception.h>
#include <local_history.h>
#include <project.h>
#include <project/project_file.h>
#include <project/project_local_settings.h>
#include <settings/settings_manager.h>

#include <wx/ffile.h>
#include <wx/filename.h>

#include <array>
#include <cstdint>
#include <memory>

namespace
{

using KEMI_FORMAT::DOC;
using KEMI_FORMAT::FLAVOR;

const std::array<DOC, 3> jsonDocuments = { DOC::PROJECT, DOC::PROJECT_LOCAL, DOC::JOBSET };

void writeDocument( const wxString& aPath, const std::string& aBytes )
{
    wxFFile file( aPath, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE_EQUAL( file.Write( aBytes.data(), aBytes.size() ), aBytes.size() );
    BOOST_REQUIRE( file.Close() );
}


struct KEMI_JSON_FIXTURE
{
    KEMI_JSON_FIXTURE() : directory( wxS( "kemi-json" ) ) {}

    wxString path( DOC aDoc, FLAVOR aFlavor, const wxString& aName = wxS( "document" ) )
    {
        return directory.ChildPathStr( aName + wxS( "." ) + KEMI_FORMAT::Ext( aDoc, aFlavor ) );
    }

    std::unique_ptr<JSON_SETTINGS> settings( DOC aDoc, const wxString& aPath )
    {
        switch( aDoc )
        {
        case DOC::PROJECT:
        {
            auto result = std::make_unique<PROJECT_FILE>( aPath );
            result->SetProject( &project );
            return result;
        }

        case DOC::PROJECT_LOCAL:
            return std::make_unique<PROJECT_LOCAL_SETTINGS>( &project, aPath );

        case DOC::JOBSET:
            return std::make_unique<JOBSET>( aPath );

        default:
            BOOST_FAIL( "Expected a JSON document type" );
            return nullptr;
        }
    }

    std::string fixtureBytes( JSON_SETTINGS& aSettings, FLAVOR aFlavor )
    {
        nlohmann::json document = nlohmann::json::parse( aSettings.FormatAsString() );

        if( aFlavor == FLAVOR::LEGACY )
            KEMI_FORMAT::ClearJsonMarker( document );

        return document.dump( 2 ) + '\n';
    }

    void saveFixture( JSON_SETTINGS& aSettings, const wxString& aFilename, FLAVOR aFlavor )
    {
        if( aFlavor == FLAVOR::LEGACY )
            writeDocument( aFilename, fixtureBytes( aSettings, aFlavor ) );
        else
            BOOST_REQUIRE( aSettings.SaveToFile( wxEmptyString, true ) );
    }

    void setPayload( JSON_SETTINGS& aSettings )
    {
        if( auto* projectFile = dynamic_cast<PROJECT_FILE*>( &aSettings ) )
            projectFile->m_TextVars[wxS( "roundtrip" )] = wxS( "preserved" );
        else if( auto* local = dynamic_cast<PROJECT_LOCAL_SETTINGS*>( &aSettings ) )
            local->m_OpenJobSets.push_back( wxS( "roundtrip.kemi_jobset" ) );
        else if( auto* jobset = dynamic_cast<JOBSET*>( &aSettings ) )
            jobset->AddNewDestination( JOBSET_DESTINATION_T::FOLDER );
    }

    void checkPayload( JSON_SETTINGS& aSettings )
    {
        if( auto* projectFile = dynamic_cast<PROJECT_FILE*>( &aSettings ) )
            BOOST_CHECK_EQUAL( projectFile->m_TextVars[wxS( "roundtrip" )], wxS( "preserved" ) );
        else if( auto* local = dynamic_cast<PROJECT_LOCAL_SETTINGS*>( &aSettings ) )
        {
            BOOST_REQUIRE_EQUAL( local->m_OpenJobSets.size(), 1 );
            BOOST_CHECK_EQUAL( local->m_OpenJobSets.front(), wxS( "roundtrip.kemi_jobset" ) );
        }
        else if( auto* jobset = dynamic_cast<JOBSET*>( &aSettings ) )
            BOOST_CHECK_EQUAL( jobset->GetDestinations().size(), 1 );
    }

    KI_TEST::SCOPED_TEMP_DIR directory;
    PROJECT                  project;
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( KemiJson, KEMI_JSON_FIXTURE )


BOOST_AUTO_TEST_CASE( RealDocumentRoundTrips )
{
    for( DOC doc : jsonDocuments )
    {
        for( FLAVOR flavor : { FLAVOR::LEGACY, FLAVOR::KEMI } )
        {
            wxString filename = path( doc, flavor );
            auto source = settings( doc, filename );
            setPayload( *source );
            saveFixture( *source, filename, flavor );
            BOOST_REQUIRE( wxFileName::FileExists( filename ) );

            nlohmann::json written = nlohmann::json::parse( KI_TEST::LoadStringData( filename ) );
            BOOST_CHECK_EQUAL( written.contains( KEMI_FORMAT::JSON_MARKER_KEY ),
                               flavor == FLAVOR::KEMI );

            auto restored = settings( doc, filename );
            BOOST_REQUIRE( restored->LoadFromFile() );
            BOOST_CHECK_EQUAL( restored->GetFullFilename(), filename );
            checkPayload( *restored );
        }
    }
}


BOOST_AUTO_TEST_CASE( RestrictedLegacyJobsetSaveAsPreservesSourceAndAdoptsNativeTarget )
{
    const wxString source = path( DOC::JOBSET, FLAVOR::LEGACY );
    const wxString target = path( DOC::JOBSET, FLAVOR::KEMI );
    JOBSET original( source );
    original.AddNewDestination( JOBSET_DESTINATION_T::FOLDER );
    saveFixture( original, source, FLAVOR::LEGACY );
    const std::string before = KI_TEST::LoadStringData( source );

    JOBSET edited( source );
    BOOST_REQUIRE( edited.LoadFromFile() );
    edited.SetKemiOnly();
    edited.AddNewDestination( JOBSET_DESTINATION_T::FOLDER );
    BOOST_REQUIRE( edited.GetDirty() );
    BOOST_CHECK( !edited.SaveAs( source ) );
    BOOST_CHECK( edited.LastSaveFailed() );
    BOOST_CHECK( edited.GetDirty() );
    BOOST_CHECK_EQUAL( edited.GetFullFilename(), source );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );

    BOOST_REQUIRE( edited.SaveAs( target ) );
    BOOST_CHECK( !edited.GetDirty() );
    BOOST_CHECK_EQUAL( edited.GetFullFilename(), target );
    BOOST_CHECK_EQUAL( edited.GetFullName(), wxFileName( target ).GetFullName() );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
    JOBSET restored( target );
    BOOST_REQUIRE( restored.LoadFromFile() );
    BOOST_CHECK( restored.IsKemiOnly() );
    BOOST_REQUIRE_EQUAL( restored.GetDestinations().size(), 2U );
    edited.AddNewDestination( JOBSET_DESTINATION_T::FOLDER );
    BOOST_REQUIRE( edited.SaveToFile( wxEmptyString, true ) );
    BOOST_REQUIRE( restored.LoadFromFile() );
    BOOST_CHECK_EQUAL( restored.GetDestinations().size(), 3U );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
}


BOOST_AUTO_TEST_CASE( OfficialJobsetSaveAsRejectsExistingTargetsAndKeepsIdentityOnFailure )
{
    const wxString source = path( DOC::JOBSET, FLAVOR::LEGACY );
    JOBSET original( source );
    saveFixture( original, source, FLAVOR::LEGACY );
    const std::string before = KI_TEST::LoadStringData( source );
    JOBSET edited( source );
    BOOST_REQUIRE( edited.LoadFromFile() );
    edited.AddNewDestination( JOBSET_DESTINATION_T::FOLDER );

    for( const wxString& name : { wxS( "document" ), wxS( "another" ) } )
    {
        const wxString target = path( DOC::JOBSET, FLAVOR::KEMI, name );
        const std::string sentinel = "existing native target\n";
        writeDocument( target, sentinel );
        BOOST_CHECK( !edited.SaveAs( target ) );
        BOOST_CHECK( edited.GetDirty() );
        BOOST_CHECK_EQUAL( edited.GetFullFilename(), source );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( target ), sentinel );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
    }

    BOOST_CHECK( !edited.SaveAs( directory.ChildPathStr( wxS( "missing/copy.kemi_jobset" ) ) ) );
    BOOST_CHECK( edited.GetDirty() );
    BOOST_CHECK_EQUAL( edited.GetFullFilename(), source );
    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( source ), before );
}


BOOST_AUTO_TEST_CASE( DottedBasenamesAndEmptyLegacyDocumentsKeepCompatibility )
{
    for( DOC doc : jsonDocuments )
    {
        wxString basename = directory.ChildPathStr( wxS( "project.rev1" ) );
        auto source = settings( doc, basename );
        wxString filename = basename + wxS( "." ) + KEMI_FORMAT::Ext( doc, FLAVOR::KEMI );
        BOOST_CHECK_EQUAL( source->GetFullFilename(), filename );
        BOOST_REQUIRE( source->SaveToFile( wxEmptyString, true ) );
        BOOST_CHECK( wxFileName::FileExists( filename ) );

        if( auto* projectFile = dynamic_cast<PROJECT_FILE*>( source.get() ) )
        {
            TOP_LEVEL_SHEET_INFO sheet;
            sheet.name = wxS( "project.rev1" );
            sheet.filename = wxS( "project.rev1.kicad_sch" );
            projectFile->GetTopLevelSheets().push_back( sheet );
            BOOST_REQUIRE( projectFile->SaveAs( directory.PathStr(), wxS( "renamed.rev2.kemi_pro" ) ) );
            BOOST_CHECK_EQUAL( projectFile->GetTopLevelSheets().front().filename,
                               wxS( "renamed.rev2.kicad_sch" ) );
        }

        filename = path( doc, FLAVOR::LEGACY, wxS( "empty" ) );
        writeDocument( filename, "{}" );
        auto legacy = settings( doc, filename );
        // Missing KiCad schema metadata historically returns false but allows defaults
        // to be saved.  It is not a malformed Kemi container or a JSON syntax error.
        BOOST_CHECK( !legacy->LoadFromFile() );
        BOOST_CHECK( legacy->GetLoadError().IsEmpty() );
        BOOST_CHECK( !legacy->IsReadOnly() );
        if( doc == DOC::JOBSET )
            BOOST_REQUIRE( legacy->SaveToFile( wxEmptyString, true ) );
        else
            BOOST_CHECK( !legacy->SaveToFile( wxEmptyString, true ) );

        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( filename ), "{}" );

        if( doc == DOC::PROJECT )
        {
            writeDocument( filename, "{}" );
            SETTINGS_MANAGER manager;
            BOOST_CHECK( !manager.LoadProject( filename, false ) );
            PROJECT* loaded = manager.GetProject( filename );
            BOOST_REQUIRE( loaded );
            BOOST_CHECK( loaded->GetProjectFile().GetLoadError().IsEmpty() );
            BOOST_CHECK( !loaded->IsReadOnly() );
            BOOST_CHECK( !loaded->GetProjectFile().IsReadOnly() );
            BOOST_CHECK( !manager.SaveProject( filename, loaded ) );
            BOOST_REQUIRE( manager.UnloadProject( loaded, false ) );

            BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( filename ), "{}" );
        }
    }
}


BOOST_AUTO_TEST_CASE( InvalidMarkersRejectBeforeApplyingParametersAndPreserveBytes )
{
    const std::vector<nlohmann::json> invalidMarkers = {
        nlohmann::json::object(), nullptr, "invalid",
        { { "document_type", "jobset" }, { "format_version", "1" } },
        { { "document_type", "jobset" }, { "format_version", 1.0 } },
        { { "document_type", "jobset" }, { "format_version", true } },
        { { "document_type", "jobset" }, { "format_version", 0 } },
        { { "document_type", "jobset" }, { "format_version", 2 } },
        { { "document_type", "jobset" }, { "format_version", UINT64_MAX } },
        { { "document_type", 42 }, { "format_version", 1 } },
        { { "document_type", "wrong_document" }, { "format_version", 1 } }
    };

    for( DOC doc : jsonDocuments )
    {
        for( FLAVOR flavor : { FLAVOR::LEGACY, FLAVOR::KEMI } )
        {
            wxString filename = path( doc, flavor );
            auto source = settings( doc, filename );
            setPayload( *source );
            nlohmann::json document = nlohmann::json::parse( source->FormatAsString() );

            for( nlohmann::json marker : invalidMarkers )
            {
                // Keep the document type correct in version-specific cases.
                if( marker.is_object() && marker.value( "document_type", nlohmann::json() ) == "jobset" )
                    marker["document_type"] = KEMI_FORMAT::Profile( doc ).jsonType;

                document[KEMI_FORMAT::JSON_MARKER_KEY] = marker;
                const std::string bytes = document.dump( 2 ) + '\n';
                writeDocument( filename, bytes );

                for( bool raw : { false, true } )
                {
                    auto rejected = settings( doc, filename );
                    BOOST_CHECK( !( raw ? rejected->LoadFromRawFile( filename )
                                        : rejected->LoadFromFile() ) );
                    BOOST_CHECK( rejected->IsReadOnly() );
                    BOOST_CHECK( !rejected->GetLoadError().IsEmpty() );
                    BOOST_CHECK( !rejected->IsFileSynced() );

                    if( auto* jobset = dynamic_cast<JOBSET*>( rejected.get() ) )
                    {
                        BOOST_CHECK( jobset->GetJobs().empty() );
                        BOOST_CHECK( jobset->GetDestinations().empty() );
                    }

                    // SaveAs callers may clear the filesystem read-only flag; rejection persists.
                    rejected->SetReadOnly( false );
                    BOOST_CHECK( !rejected->SaveToFile( wxEmptyString, true ) );
                    BOOST_CHECK( rejected->LastSaveFailed() );
                    BOOST_CHECK_THROW( rejected->FormatAsString(), IO_ERROR );

                    if( auto* projectFile = dynamic_cast<PROJECT_FILE*>( rejected.get() ) )
                        BOOST_CHECK( projectFile->SerializeToString().empty() );

                    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( filename ), bytes );
                }
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( MissingMarkerIsOnlyAcceptedForLegacyExtension )
{
    for( DOC doc : jsonDocuments )
    {
        auto source = settings( doc, path( doc, FLAVOR::LEGACY ) );
        const std::string bytes = fixtureBytes( *source, FLAVOR::LEGACY );

        for( FLAVOR flavor : { FLAVOR::LEGACY, FLAVOR::KEMI } )
        {
            wxString filename = path( doc, flavor );
            writeDocument( filename, bytes );
            auto restored = settings( doc, filename );
            BOOST_CHECK_EQUAL( restored->LoadFromFile(), flavor == FLAVOR::LEGACY );
        }
    }
}


BOOST_AUTO_TEST_CASE( SyntaxErrorsAndRawSourceExtensionsRemainProtected )
{
    for( DOC doc : jsonDocuments )
    {
        wxString filename = path( doc, FLAVOR::KEMI );
        const std::string malformed = "{ invalid JSON";
        writeDocument( filename, malformed );
        auto rejected = settings( doc, filename );
        BOOST_CHECK( !rejected->LoadFromFile() );
        BOOST_CHECK( !rejected->LoadFromFile() );
        rejected->SetReadOnly( false );
        BOOST_CHECK( !rejected->SaveToFile( wxEmptyString, true ) );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( filename ), malformed );

        auto legacy = settings( doc, path( doc, FLAVOR::LEGACY ) );
        writeDocument( filename, fixtureBytes( *legacy, FLAVOR::LEGACY ) );
        BOOST_CHECK( !legacy->LoadFromRawFile( filename ) );
        BOOST_CHECK( legacy->IsReadOnly() );
    }
}


BOOST_AUTO_TEST_CASE( NumericOverflowRejectsBothLoadersAndRecoversAfterValidReload )
{
    for( DOC doc : jsonDocuments )
    {
        for( FLAVOR flavor : { FLAVOR::LEGACY, FLAVOR::KEMI } )
        {
            wxString filename = path( doc, flavor );
            auto source = settings( doc, filename );
            setPayload( *source );
            nlohmann::json document = nlohmann::json::parse( source->FormatAsString() );
            if( flavor == FLAVOR::LEGACY )
                KEMI_FORMAT::ClearJsonMarker( document );
            const std::string validBytes = document.dump( 2 ) + '\n';

            // A numeric overflow throws out_of_range while parsing, before marker validation.
            // Inject an independent field: valid legacy documents have no Kemi format_version.
            // Construct the invalid input as bytes because nlohmann::json cannot retain infinity.
            std::string overflowBytes = validBytes;
            BOOST_REQUIRE( !overflowBytes.empty() );
            BOOST_REQUIRE_EQUAL( overflowBytes.front(), '{' );
            overflowBytes.insert( 1, "\"numeric_overflow\": 1e999," );

            for( bool raw : { false, true } )
            {
                writeDocument( filename, validBytes );
                auto restored = settings( doc, filename );
                BOOST_REQUIRE( raw ? restored->LoadFromRawFile( filename )
                                   : restored->LoadFromFile() );
                checkPayload( *restored );
                writeDocument( filename, overflowBytes );

                for( int attempt = 0; attempt < 2; ++attempt )
                {
                    bool loaded = true;
                    BOOST_REQUIRE_NO_THROW( loaded = raw ? restored->LoadFromRawFile( filename )
                                                         : restored->LoadFromFile() );
                    BOOST_CHECK( !loaded );
                    BOOST_CHECK( restored->IsReadOnly() );
                    BOOST_CHECK( !restored->GetLoadError().IsEmpty() );
                    BOOST_CHECK( !restored->IsFileSynced() );

                    if( auto* jobset = dynamic_cast<JOBSET*>( restored.get() ) )
                    {
                        BOOST_CHECK( jobset->GetJobs().empty() );
                        BOOST_CHECK( jobset->GetDestinations().empty() );
                    }

                    restored->SetReadOnly( false );
                    BOOST_CHECK( !restored->SaveToFile( wxEmptyString, true ) );
                    BOOST_CHECK( restored->LastSaveFailed() );
                    BOOST_CHECK_THROW( restored->FormatAsString(), IO_ERROR );
                    BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( filename ), overflowBytes );
                }

                writeDocument( filename, validBytes );
                BOOST_REQUIRE( raw ? restored->LoadFromRawFile( filename )
                                   : restored->LoadFromFile() );
                BOOST_CHECK( restored->GetLoadError().IsEmpty() );
                BOOST_CHECK( !restored->IsReadOnly() );
                checkPayload( *restored );
                const wxString recovered = path( doc, FLAVOR::KEMI,
                        raw ? wxS( "recovered_raw" ) : wxS( "recovered_file" ) );
                BOOST_REQUIRE( restored->SaveCopyToFile( recovered ) );

                auto reloaded = settings( doc, recovered );
                BOOST_REQUIRE( reloaded->LoadFromFile() );
                checkPayload( *reloaded );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE( ExplicitKemiFilenameNeverSelectsLegacySibling )
{
    for( DOC doc : jsonDocuments )
    {
        auto legacy = settings( doc, path( doc, FLAVOR::LEGACY ) );
        saveFixture( *legacy, path( doc, FLAVOR::LEGACY ), FLAVOR::LEGACY );
        const std::string legacyBytes = KI_TEST::LoadStringData( path( doc, FLAVOR::LEGACY ) );

        auto kemi = settings( doc, path( doc, FLAVOR::KEMI ) );
        BOOST_CHECK( !kemi->LoadFromFile() );
        setPayload( *kemi );
        BOOST_REQUIRE( kemi->SaveToFile( wxEmptyString, true ) );

        auto restored = settings( doc, path( doc, FLAVOR::KEMI ) );
        BOOST_REQUIRE( restored->LoadFromFile() );
        checkPayload( *restored );
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( path( doc, FLAVOR::LEGACY ) ), legacyBytes );
    }
}


BOOST_AUTO_TEST_CASE( RenamedNativeDocumentCannotEraseMarkerOnSave )
{
    for( DOC doc : jsonDocuments )
    {
        wxString filename = path( doc, FLAVOR::LEGACY );
        auto source = settings( doc, filename );
        nlohmann::json document = nlohmann::json::parse( source->FormatAsString() );
        KEMI_FORMAT::StampJsonMarker( document, doc );
        document["unrecognized_extension"] = { { "retained", true } };
        writeDocument( filename, document.dump( 2 ) + '\n' );

        auto restored = settings( doc, filename );
        BOOST_REQUIRE( restored->LoadFromFile() );
        BOOST_CHECK( restored->IsKemiOnly() );
        if( doc == DOC::JOBSET )
            BOOST_CHECK( restored->SaveToFile() );
        else
        {
            BOOST_CHECK( !restored->SaveToFile() );
            BOOST_CHECK( restored->LastSaveFailed() );
        }

        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( filename ), document.dump( 2 ) + '\n' );
        BOOST_CHECK( nlohmann::json::parse( restored->FormatAsString() )
                             .contains( KEMI_FORMAT::JSON_MARKER_KEY ) );
    }
}


BOOST_AUTO_TEST_CASE( ProjectAndLocalSaveAsRetainNativeRestriction )
{
    for( DOC doc : { DOC::PROJECT, DOC::PROJECT_LOCAL } )
    {
        auto source = settings( doc, path( doc, FLAVOR::LEGACY ) );
        setPayload( *source );

        for( FLAVOR flavor : { FLAVOR::KEMI, FLAVOR::LEGACY } )
        {
            wxString filename = path( doc, flavor, wxS( "saved_as" ) );

            if( auto* projectFile = dynamic_cast<PROJECT_FILE*>( source.get() ) )
            {
                BOOST_REQUIRE_EQUAL( projectFile->SaveAs( directory.PathStr(), wxFileName( filename ).GetFullName() ),
                                     flavor == FLAVOR::KEMI );
                if( flavor == FLAVOR::LEGACY )
                {
                    BOOST_CHECK( !wxFileExists( filename ) );
                    continue;
                }

                BOOST_CHECK_EQUAL( nlohmann::json::parse( projectFile->SerializeToString() )
                                           .contains( KEMI_FORMAT::JSON_MARKER_KEY ),
                                   flavor == FLAVOR::KEMI );
            }
            else
            {
                auto* local = dynamic_cast<PROJECT_LOCAL_SETTINGS*>( source.get() );
                BOOST_REQUIRE_EQUAL( local->SaveAs( directory.PathStr(), wxFileName( filename ).GetFullName() ),
                                     flavor == FLAVOR::KEMI );
                if( flavor == FLAVOR::LEGACY )
                {
                    BOOST_CHECK( !wxFileExists( filename ) );
                    continue;
                }
            }

            auto restored = settings( doc, filename );
            BOOST_REQUIRE( restored->LoadFromFile() );
            checkPayload( *restored );
        }
    }
}


BOOST_AUTO_TEST_CASE( ManagerImportsBothJsonFlavorsAndOnlyCopiesNativeDocuments )
{
    for( FLAVOR flavor : { FLAVOR::LEGACY, FLAVOR::KEMI } )
    {
        const wxString projectPath = path( DOC::PROJECT, flavor );

        for( DOC doc : { DOC::PROJECT, DOC::PROJECT_LOCAL } )
        {
            auto source = settings( doc, path( doc, flavor ) );
            setPayload( *source );
            saveFixture( *source, path( doc, flavor ), flavor );
        }

        const std::string original = KI_TEST::LoadStringData( projectPath );
        SETTINGS_MANAGER manager;
        BOOST_REQUIRE( manager.LoadProject( projectPath, false ) );
        PROJECT* loaded = manager.GetProject( projectPath );
        BOOST_REQUIRE( loaded );
        checkPayload( loaded->GetProjectFile() );
        checkPayload( loaded->GetLocalSettings() );
        const wxString official = path( DOC::PROJECT, FLAVOR::LEGACY, wxS( "copy" ) );
        BOOST_CHECK( !manager.SaveProjectCopy( official, loaded ) );
        BOOST_CHECK_THROW( manager.SaveProjectAs( official, loaded ), IO_ERROR );
        BOOST_CHECK( !wxFileExists( official ) );
        BOOST_CHECK_EQUAL( loaded->GetProjectFullName(), projectPath );

        const wxString native = path( DOC::PROJECT, FLAVOR::KEMI, wxS( "copy" ) );
        if( flavor == FLAVOR::LEGACY )
        {
            BOOST_CHECK( !manager.SaveProjectCopy( native, loaded ) );
            BOOST_CHECK_THROW( manager.SaveProjectAs( native, loaded ), IO_ERROR );
            BOOST_CHECK( !wxFileExists( native ) );
        }
        else
        {
            BOOST_REQUIRE( manager.SaveProjectCopy( native, loaded ) );
            BOOST_CHECK( nlohmann::json::parse( KI_TEST::LoadStringData( native ) )
                                 .contains( KEMI_FORMAT::JSON_MARKER_KEY ) );
        }
        BOOST_CHECK_EQUAL( KI_TEST::LoadStringData( projectPath ), original );
        BOOST_CHECK( loaded->GetProjectFile().IsKemiOnly() );
        BOOST_CHECK( loaded->GetLocalSettings().IsKemiOnly() );
        BOOST_REQUIRE( manager.UnloadProject( loaded, false ) );
    }
}


BOOST_AUTO_TEST_CASE( ProjectHistoryUsesActualProjectAndLocalFilenames )
{
    wxString projectPath = path( DOC::PROJECT, FLAVOR::KEMI );
    auto source = settings( DOC::PROJECT, projectPath );
    setPayload( *source );
    BOOST_REQUIRE( source->SaveToFile( wxEmptyString, true ) );
    auto local = settings( DOC::PROJECT_LOCAL, path( DOC::PROJECT_LOCAL, FLAVOR::KEMI ) );
    setPayload( *local );
    BOOST_REQUIRE( local->SaveToFile( wxEmptyString, true ) );

    SETTINGS_MANAGER manager;
    BOOST_REQUIRE( manager.LoadProject( projectPath, false ) );
    PROJECT* loaded = manager.GetProject( projectPath );
    BOOST_REQUIRE( loaded );
    std::vector<HISTORY_FILE_DATA> snapshot;
    loaded->SaveToHistory( projectPath, snapshot );
    BOOST_REQUIRE_EQUAL( snapshot.size(), 2 );

    wxString restoreDirectory = directory.CreateChildDirStr( wxS( "restored" ) );

    for( const HISTORY_FILE_DATA& entry : snapshot )
    {
        wxString restoredPath = wxFileName( restoreDirectory, entry.relativePath ).GetFullPath();
        writeDocument( restoredPath, entry.content.empty() ? KI_TEST::LoadStringData( entry.sourcePath )
                                                          : entry.content );
        DOC doc = KEMI_FORMAT::IsKemiExt( DOC::PROJECT, wxFileName( restoredPath ).GetExt() )
                          ? DOC::PROJECT : DOC::PROJECT_LOCAL;
        BOOST_CHECK( KEMI_FORMAT::IsKemiExt( doc, wxFileName( restoredPath ).GetExt() ) );
        auto restored = settings( doc, restoredPath );
        BOOST_REQUIRE( restored->LoadFromFile() );
        checkPayload( *restored );
    }

    manager.UnloadProject( loaded, false );
}




BOOST_AUTO_TEST_CASE( FormatVersionPolicyAcceptsSupportedRange )
{
    using namespace KEMI_FORMAT;

    // Within [MIN, FORMAT_VERSION]: readable (backward compatible).
    BOOST_CHECK( CheckFormatVersion( DOC::PROJECT, MIN_FORMAT_VERSION ).IsEmpty() );
    BOOST_CHECK( CheckFormatVersion( DOC::SCHEMATIC, FORMAT_VERSION ).IsEmpty() );
    BOOST_CHECK( CheckFormatVersion( DOC::PCB, FORMAT_VERSION ).IsEmpty() );

    // Below lower bound: reject.
    const wxString below = CheckFormatVersion( DOC::PROJECT, MIN_FORMAT_VERSION - 1 );
    BOOST_CHECK( !below.IsEmpty() );
    BOOST_CHECK( below.Lower().Contains( wxS( "unsupported" ) )
                 || below.Lower().Contains( wxS( "invalid" ) ) );

    // Above software max: reject with upgrade guidance.
    const wxString above = CheckFormatVersion( DOC::SCHEMATIC, FORMAT_VERSION + 1 );
    BOOST_CHECK( !above.IsEmpty() );
    BOOST_CHECK( above.Lower().Contains( wxS( "upgrade" ) ) );
}


BOOST_AUTO_TEST_CASE( ValidateJsonMarkerRejectsMissingUnrecognizableAndFutureVersions )
{
    using namespace KEMI_FORMAT;

    const wxString kemiPath = path( DOC::PROJECT, FLAVOR::KEMI );
    const wxString legacyPath = path( DOC::PROJECT, FLAVOR::LEGACY );

    // Missing marker on .kemi_* : reject.
    BOOST_CHECK( !ValidateJsonMarker( nlohmann::json::object(), DOC::PROJECT, kemiPath ).IsEmpty() );

    // Missing marker on official .kicad_* : still readable (compat path).
    BOOST_CHECK( ValidateJsonMarker( nlohmann::json::object(), DOC::PROJECT, legacyPath ).IsEmpty() );

    // Unrecognizable version type.
    nlohmann::json badType = { { JSON_MARKER_KEY,
                                 { { "document_type", "project" }, { "format_version", "1" } } } };
    BOOST_CHECK( !ValidateJsonMarker( badType, DOC::PROJECT, kemiPath ).IsEmpty() );

    // Future version: reject + upgrade.
    nlohmann::json future = { { JSON_MARKER_KEY,
                                { { "document_type", "project" },
                                  { "format_version", FORMAT_VERSION + 1 } } } };
    const wxString futureErr = ValidateJsonMarker( future, DOC::PROJECT, kemiPath );
    BOOST_CHECK( !futureErr.IsEmpty() );
    BOOST_CHECK( futureErr.Lower().Contains( wxS( "upgrade" ) ) );

    // Current version: accept.
    nlohmann::json current = { { JSON_MARKER_KEY,
                                 { { "document_type", "project" },
                                   { "format_version", FORMAT_VERSION } } } };
    BOOST_CHECK( ValidateJsonMarker( current, DOC::PROJECT, kemiPath ).IsEmpty() );

    // Old supported version (MIN): accept when within range.
    nlohmann::json oldOk = { { JSON_MARKER_KEY,
                               { { "document_type", "project" },
                                 { "format_version", MIN_FORMAT_VERSION } } } };
    BOOST_CHECK( ValidateJsonMarker( oldOk, DOC::PROJECT, kemiPath ).IsEmpty() );
}

BOOST_AUTO_TEST_SUITE_END()
