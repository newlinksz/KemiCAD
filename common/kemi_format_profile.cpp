/*
 * This program source code file is part of KemiCAD.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 */

#include <kemi_format_profile.h>
#include <brand.h>

#include <json_common.h>
#include <ki_exception.h>
#include <dsnlexer.h>
#include <richio.h>
#include <wx_filename.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include <wx/filename.h>
#include <wx/ffile.h>
#include <wx/intl.h>

namespace KEMI_FORMAT
{

const std::vector<DOC_PROFILE>& AllProfiles()
{
    // clang-format off
    static const std::vector<DOC_PROFILE> profiles = {
        //  doc                 kemiExt         kicadExt         kemiRoot           kicadRoot           jsonType
        { DOC::PROJECT,       "kemi_pro",    "kicad_pro",    nullptr,           nullptr,            "project" },
        { DOC::PROJECT_LOCAL, "kemi_prl",    "kicad_prl",    nullptr,           nullptr,            "project_local" },
        { DOC::JOBSET,        "kemi_jobset", "kicad_jobset", nullptr,           nullptr,            "jobset" },
        { DOC::SCHEMATIC,     "kemi_sch",    "kicad_sch",    "kemi_sch",        "kicad_sch",        nullptr },
        { DOC::PCB,           "kemi_pcb",    "kicad_pcb",    "kemi_pcb",        "kicad_pcb",        nullptr },
        { DOC::SYMBOL_LIB,    "kemi_sym",    "kicad_sym",    "kemi_symbol_lib", "kicad_symbol_lib", nullptr },
        { DOC::FOOTPRINT,     "kemi_mod",    "kicad_mod",    "kemi_footprint",  "footprint",        nullptr },
        { DOC::DRAWING_SHEET, "kemi_wks",    "kicad_wks",    "kemi_wks",        "kicad_wks",        nullptr },
        { DOC::DESIGN_RULES,  "kemi_dru",    "kicad_dru",    "kemi_dru",        nullptr,            nullptr },
    };
    // clang-format on

    return profiles;
}


const DOC_PROFILE& Profile( DOC aDoc )
{
    for( const DOC_PROFILE& p : AllProfiles() )
    {
        if( p.doc == aDoc )
            return p;
    }

    wxFAIL_MSG( wxT( "KEMI_FORMAT::Profile: unknown document type" ) );
    return AllProfiles().front();
}


std::string Ext( DOC aDoc, FLAVOR aFlavor )
{
    const DOC_PROFILE& p = Profile( aDoc );
    return aFlavor == FLAVOR::KEMI ? p.kemiExt : p.kicadExt;
}


std::string RootToken( DOC aDoc, FLAVOR aFlavor )
{
    const DOC_PROFILE& p = Profile( aDoc );
    const char*        root = aFlavor == FLAVOR::KEMI ? p.kemiRoot : p.kicadRoot;

    return root ? root : "";
}


std::vector<std::string> ReadableExts( DOC aDoc )
{
    const DOC_PROFILE& p = Profile( aDoc );
    return { p.kicadExt, p.kemiExt };
}


std::vector<std::string> WildcardExts( DOC aDoc )
{
    return ReadableExts( aDoc );
}


bool IsKemiExt( DOC aDoc, const wxString& aExt )
{
    return aExt.IsSameAs( wxString( Profile( aDoc ).kemiExt ), false );
}


bool IsLegacyExt( DOC aDoc, const wxString& aExt )
{
    return aExt.IsSameAs( wxString( Profile( aDoc ).kicadExt ), false );
}


bool IsReadableExt( DOC aDoc, const wxString& aExt )
{
    return IsKemiExt( aDoc, aExt ) || IsLegacyExt( aDoc, aExt );
}


bool HasKemiExt( const wxString& aPath )
{
    wxString ext = wxFileName( aPath ).GetExt();

    for( const DOC_PROFILE& p : AllProfiles() )
    {
        if( ext.IsSameAs( wxString( p.kemiExt ), false ) )
            return true;
    }

    return false;
}


bool IsRestrictedOutput( const wxString& aDocumentPath, const wxString& aProjectPath )
{
    const wxString documentExt = wxFileName( aDocumentPath ).GetExt();
    const wxString projectExt = wxFileName( aProjectPath ).GetExt();

    for( const DOC_PROFILE& profile : AllProfiles() )
    {
        if( IsReadableExt( profile.doc, documentExt ) || IsReadableExt( profile.doc, projectExt ) )
        {
            return true;
        }
    }

    return false;
}


wxString ResolveNativeSaveTarget( DOC aDoc, const wxString& aSourcePath )
{
    wxFileName target( aSourcePath );
    target.SetExt( Ext( aDoc, FLAVOR::KEMI ) );
    return ResolveNativeSaveTarget( aDoc, aSourcePath, target.GetFullPath() );
}


wxString ResolveNativeSaveTarget( DOC aDoc, const wxString& aSourcePath,
                                 const wxString& aRequestedPath )
{
    const wxFileName target( PersistentOutputPath( aDoc, aRequestedPath, true ) );
    const bool native = IsKemiExt( aDoc, wxFileName( aSourcePath ).GetExt() );

    if( !native && target.Exists() )
    {
        THROW_IO_ERROR( wxString::Format(
                _( "Native save target '%s' already exists. Use Save As to choose another name." ),
                target.GetFullPath() ) );
    }

    return target.GetFullPath();
}


wxString PersistentOutputPath( DOC aDoc, const wxString& aRequestedPath, bool )
{
    // This is an explicit destination, unlike ResolveNativeSaveTarget's source path.
    if( wxFileName( aRequestedPath ).GetFullName().Lower().Contains( wxS( ".kicad_" ) ) )
        THROW_IO_ERROR( _( "Official KiCad formats are import-only. Choose a .kemi_* file." ) );

    wxFileName path( aRequestedPath );
    KEMI_ONLY_OUTPUT_POLICY policy( path.GetPath().IsEmpty() ? wxString( wxS( "." ) ) : path.GetPath() );

    if( HasKemiExt( aRequestedPath ) && !IsKemiExt( aDoc, path.GetExt() ) )
        THROW_IO_ERROR( _( "The native file extension belongs to a different document type." ) );

    path.SetExt( Ext( aDoc, FLAVOR::KEMI ) );
    policy.CheckWritePath( aDoc, path.GetFullPath() );
    return path.GetFullPath();
}


void CheckPersistentOutput( DOC aDoc, const wxString& aPath, FLAVOR aFlavor, bool aRestricted )
{
    if( aPath.IsEmpty() && !aRestricted )
        return;

    if( aFlavor != FLAVOR::KEMI )
        THROW_IO_ERROR( _( "Official KiCad formats are import-only. Choose a .kemi_* file." ) );

    // In-memory formatters have no physical path, but still must not emit legacy content.
    if( !aPath.IsEmpty() )
    {
        wxFileName path( aPath );
        KEMI_ONLY_OUTPUT_POLICY policy( path.GetPath().IsEmpty() ? wxString( wxS( "." ) ) : path.GetPath() );
        policy.CheckWritePath( aDoc, aPath );
    }
}


void RemapSchematicSheetPaths( std::string& aContent, const wxString& aSourcePath,
        const wxString& aTargetPath, const std::map<wxString, wxString>& aTargets,
        std::vector<wxString>* aReferencedPaths )
{
    struct LIST_STATE
    {
        std::string head;
        size_t atoms = 0;
        bool sheetFile = false;
    };

    std::vector<size_t> lines{ 0 };

    for( size_t i = 0; i < aContent.size(); ++i )
    {
        if( aContent[i] == '\n' )
            lines.push_back( i + 1 );
    }

    STRING_LINE_READER reader( aContent, aSourcePath );
    DSNLEXER lexer( nullptr, 0, nullptr, &reader );
    STRING_FORMATTER quoting;
    std::vector<LIST_STATE> lists;
    std::map<size_t, std::pair<size_t, std::string>> edits;

    for( int token = lexer.NextTok(); token != DSN_EOF; token = lexer.NextTok() )
    {
        if( token == DSN_LEFT )
        {
            lists.emplace_back();
            continue;
        }

        if( token == DSN_RIGHT )
        {
            if( lists.empty() )
                THROW_IO_ERROR( _( "Unbalanced schematic while remapping sheet files." ) );

            lists.pop_back();
            continue;
        }

        if( lists.empty() )
            continue;

        LIST_STATE& state = lists.back();

        if( state.atoms == 0 )
            state.head = lexer.CurStr();
        else if( state.head == "property" && lists.size() > 1
                 && lists[lists.size() - 2].head == "sheet" )
        {
            if( state.atoms == 1 )
                state.sheetFile = lexer.CurStr() == "Sheetfile";
            else if( state.atoms == 2 && state.sheetFile && token == DSN_STRING )
            {
                wxFileName reference( wxString::FromUTF8( lexer.CurStr() ) );

                // Unresolved path variables must retain their original spelling.
                if( !reference.GetFullPath().Contains( wxS( "$" ) ) )
                {
                    reference.MakeAbsolute( wxFileName( aSourcePath ).GetPath() );
                    if( aReferencedPaths )
                        aReferencedPaths->push_back( reference.GetFullPath() );

                    wxFileName destination( reference );

                    for( const auto& [source, target] : aTargets )
                    {
                        if( reference.SameAs( wxFileName( source ) ) )
                        {
                            destination.Assign( target );
                            break;
                        }
                    }

                    destination.MakeRelativeTo( wxFileName( aTargetPath ).GetPath() );
                    const std::string replacement = quoting.Quotew( destination.GetFullPath() );
                    // DSNLEXER reports the first character inside a quoted token.
                    const size_t begin = lines.at( lexer.CurLineNumber() - 1 ) + lexer.CurOffset() - 2;
                    size_t end = begin + 1;

                    if( aContent.at( begin ) != '"' )
                        THROW_IO_ERROR( _( "Invalid schematic sheet filename token." ) );

                    while( end < aContent.size() && aContent[end] != '"' )
                    {
                        if( aContent[end] == '\\' )
                            ++end;

                        ++end;
                    }

                    if( end == aContent.size() )
                        THROW_IO_ERROR( _( "Unterminated schematic sheet filename." ) );

                    edits.emplace( begin, std::make_pair( end - begin + 1, replacement ) );
                }
            }
        }

        ++state.atoms;
    }

    if( !lists.empty() )
        THROW_IO_ERROR( _( "Unbalanced schematic while remapping sheet files." ) );

    for( auto it = edits.rbegin(); it != edits.rend(); ++it )
        aContent.replace( it->first, it->second.first, it->second.second );
}


void CheckLegacyExportSource( const wxString& aPath )
{
    if( IsRestrictedOutput( aPath ) )
        CheckPersistentOutput( DOC::PROJECT, aPath, FLAVOR::LEGACY, true );

    const wxString extension = wxFileName( aPath ).GetExt();

    for( const DOC_PROFILE& profile : AllProfiles() )
    {
        if( !IsLegacyExt( profile.doc, extension ) )
            continue;

        if( profile.jsonType )
        {
            wxFFile input( aPath, wxS( "rb" ) );
            wxString text;

            if( !input.IsOpened() || !input.ReadAll( &text, wxConvUTF8 ) )
                THROW_IO_ERROR( wxString::Format( _( "Could not inspect export source '%s'." ), aPath ) );

            const auto document = nlohmann::json::parse( text.ToStdString( wxConvUTF8 ), nullptr, false );

            if( document.is_discarded() )
                THROW_IO_ERROR( wxString::Format( _( "Invalid JSON export source '%s'." ), aPath ) );

            CheckPersistentOutput( profile.doc, aPath, FLAVOR::LEGACY,
                                   document.contains( JSON_MARKER_KEY ) );
        }
        else
        {
            FILE_LINE_READER input( aPath );
            DSNLEXER lexer( nullptr, 0, nullptr, &input );

            if( lexer.NextTok() == DSN_LEFT && lexer.NextTok() == DSN_SYMBOL )
            {
                for( const DOC_PROFILE& rootProfile : AllProfiles() )
                {
                    if( rootProfile.kemiRoot && lexer.CurStr() == rootProfile.kemiRoot )
                        CheckPersistentOutput( profile.doc, aPath, FLAVOR::LEGACY, true );
                }
            }
        }

        return;
    }
}


FLAVOR FlavorForPath( DOC aDoc, const wxString& aPath )
{
    if( aPath.IsEmpty() )
        return FLAVOR::LEGACY;

    wxString ext = wxFileName( aPath ).GetExt();

    if( IsLegacyExt( aDoc, ext ) )
        return FLAVOR::LEGACY;

    if( IsKemiExt( aDoc, ext ) )
        return FLAVOR::KEMI;

    // Readers use this helper for related paths too. Writers must select native content
    // explicitly for staging paths; pathless fragments keep the upstream syntax.
    return FLAVOR::LEGACY;
}


FLAVOR FlavorForPath( DOC aDoc, const wxString& aPath,
                      const KEMI_ONLY_OUTPUT_POLICY& aPolicy )
{
    aPolicy.CheckWritePath( aDoc, aPath );
    return FLAVOR::KEMI;
}


KEMI_ONLY_OUTPUT_POLICY::KEMI_ONLY_OUTPUT_POLICY( const wxString& aFinalDirectory )
{
    wxFileName directory = wxFileName::DirName( aFinalDirectory );

    if( aFinalDirectory.IsEmpty() || !directory.IsOk()
            || !directory.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE ) )
    {
        THROW_IO_ERROR( _( "A KemiCAD output directory is required." ) );
    }

    m_finalDirectory = directory.GetPath();
}


void KEMI_ONLY_OUTPUT_POLICY::CheckWritePath( DOC aDoc, const wxString& aPath ) const
{
    wxFileName path( aPath );

    if( !path.IsOk() || !path.HasName() )
        THROW_IO_ERROR( _( "A KemiCAD output file name is required." ) );

    // Also reject legacy-looking staging names such as library.kicad_sym.tmp.
    if( path.GetFullName().Lower().Contains( wxS( ".kicad_" ) ) )
    {
        THROW_IO_ERROR( wxString::Format(
                _( "KemiCAD-only output cannot write the official KiCad target '%s'." ),
                aPath ) );
    }

    // Atomic writers follow symlinks. A native-looking link must not overwrite an official source.
    WX_FILENAME::ResolvePossibleSymlinks( path );

    if( path.GetFullName().Lower().Contains( wxS( ".kicad_" ) ) )
        THROW_IO_ERROR( _( "Official KiCad import sources cannot be overwritten through a symbolic link." ) );

    const wxString ext = path.GetExt().Lower();

    if( ext.StartsWith( wxS( "kemi_" ) ) && !IsKemiExt( aDoc, ext ) )
    {
        THROW_IO_ERROR( wxString::Format(
                _( "KemiCAD output target '%s' has an extension for a different document type." ),
                aPath ) );
    }
}


OUTPUT_TARGET KEMI_ONLY_OUTPUT_POLICY::Target( DOC aDoc, const wxString& aName ) const
{
    CheckWritePath( aDoc, aName );

    wxFileName target( aName );

    if( !target.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE, m_finalDirectory ) )
        THROW_IO_ERROR( _( "Could not resolve the KemiCAD output target." ) );

    wxFileName relative( target );

    if( !relative.MakeRelativeTo( m_finalDirectory ) || relative.IsAbsolute()
            || ( !relative.GetDirs().IsEmpty() && relative.GetDirs()[0] == wxS( ".." ) ) )
    {
        THROW_IO_ERROR( wxString::Format(
                _( "KemiCAD output target '%s' is outside the final output directory." ), aName ) );
    }

    target.SetExt( Ext( aDoc, FLAVOR::KEMI ) );
    return OUTPUT_TARGET( aDoc, target.GetFullPath() );
}


void KEMI_ONLY_OUTPUT_POLICY::Write( DOC aDoc, const wxString& aName, const WRITER& aWriter,
                                    const wxString& aStoragePath )
{
    const OUTPUT_TARGET target = Target( aDoc, aName );
    const wxString storagePath = aStoragePath.IsEmpty() ? target.FinalPath() : aStoragePath;
    CheckWritePath( aDoc, storagePath );

    if( !aWriter( storagePath, target ) )
    {
        THROW_IO_ERROR( wxString::Format(
                _( "Could not complete the KemiCAD output '%s'." ), target.FinalPath() ) );
    }

    // Target() already resolves absolute paths and dot components. Preserve the native
    // path comparison semantics without rescanning all earlier outputs for every footprint.
    wxFileName manifestPath( target.FinalPath() );
    manifestPath.Normalize( wxPATH_NORM_CASE | wxPATH_NORM_LONG );

    if( m_manifestPaths.insert( manifestPath.GetFullPath() ).second )
        m_manifest.push_back( target );
}


wxString CheckRootForSource( DOC aDoc, const wxString& aSource, bool aRootIsKemi )
{
    if( aRootIsKemi )
        return wxEmptyString;

    wxString ext = wxFileName( aSource ).GetExt();

    if( IsKemiExt( aDoc, ext ) )
    {
        return wxString::Format( _( "File '%s' has a KemiCAD extension (.%s) but contains "
                                    "legacy content (root '%s').\n"
                                    "The file was not loaded and will not be overwritten." ),
                                 aSource, Profile( aDoc ).kemiExt,
                                 Profile( aDoc ).kicadRoot ? Profile( aDoc ).kicadRoot
                                                           : "version" );
    }

    return wxEmptyString;
}


wxString CheckFormatVersion( DOC aDoc, int aVersion )
{
    if( aVersion < MIN_FORMAT_VERSION )
    {
        return wxString::Format( _( "Invalid or unsupported KemiCAD format_version %d "
                                    "(supported range %d-%d)." ),
                                 aVersion, MIN_FORMAT_VERSION, FORMAT_VERSION );
    }

    if( aVersion > FORMAT_VERSION )
    {
        return wxString::Format( _( "This file uses KemiCAD format_version %d; this version of "
                                    "KemiCAD only supports up to %d.  Please upgrade KemiCAD." ),
                                 aVersion, FORMAT_VERSION );
    }

    return wxEmptyString;
}


wxString GeneratorVersion()
{
    return BRAND_VERSION_W;
}


bool IsKemiGeneratorVersion( const wxString& aGeneratorVersion )
{
    // Strict: exactly MAJOR.MINOR.PATCH, ASCII digits only, no sign, no leading zeros, no
    // suffix and no surrounding text.  KemiCAD always writes its
    // three-part brand version; upstream KiCad writes two parts ("10.99", "9.0", "5.1"), so
    // those, old upstream low versions included, are never shown as a KemiCAD version.
    // Any major is accepted, so a future KemiCAD 5.0.0 / 12.0.0 still reads as KemiCAD.
    const wxString& v = aGeneratorVersion;
    int             parts = 0;
    size_t          i = 0;

    while( true )
    {
        const size_t start = i;

        while( i < v.length() && v[i] >= '0' && v[i] <= '9' )
            ++i;

        const size_t len = i - start;

        if( len == 0 || len > 9 || ( len > 1 && v[start] == '0' ) )
            return false;

        ++parts;

        if( i == v.length() )
            break;

        if( v[i] != '.' || parts == 3 )
            return false;

        ++i;
    }

    return parts == 3;
}


bool ShowKemiGeneratorVersion( const wxString& aGeneratorVersion, bool aKemiOrigin )
{
    return aKemiOrigin && IsKemiGeneratorVersion( aGeneratorVersion );
}


void StampJsonMarker( nlohmann::json& aDocument, DOC aDoc )
{
    const DOC_PROFILE& p = Profile( aDoc );

    if( !p.jsonType )
        return;

    aDocument[JSON_MARKER_KEY] = { { "format_version", FORMAT_VERSION },
                                  { "document_type", p.jsonType } };
}


void ClearJsonMarker( nlohmann::json& aDocument )
{
    aDocument.erase( JSON_MARKER_KEY );
}


wxString ValidateJsonMarker( const nlohmann::json& aDocument, DOC aDoc, const wxString& aPath )
{
    const DOC_PROFILE& p = Profile( aDoc );
    bool               kemiExt = IsKemiExt( aDoc, wxFileName( aPath ).GetExt() );

    if( aDocument.is_object() && !aDocument.contains( JSON_MARKER_KEY ) )
    {
        if( kemiExt )
        {
            return wxString::Format( _( "File '%s' has a KemiCAD extension but no KemiCAD "
                                        "marker.\nThe file was not loaded and will not be "
                                        "overwritten." ),
                                     aPath );
        }

        return wxEmptyString;   // legacy file read through the compatibility path
    }

    if( !aDocument.is_object() || !aDocument.contains( JSON_MARKER_KEY )
            || !aDocument[JSON_MARKER_KEY].is_object()
            || !aDocument[JSON_MARKER_KEY].contains( "format_version" )
            || !aDocument[JSON_MARKER_KEY]["format_version"].is_number_integer()
            || !aDocument[JSON_MARKER_KEY].contains( "document_type" )
            || !aDocument[JSON_MARKER_KEY]["document_type"].is_string()
            || aDocument[JSON_MARKER_KEY]["document_type"] != p.jsonType )
    {
        return wxString::Format( _( "File '%s' has an invalid KemiCAD marker (expected "
                                    "document type '%s').\nThe file was not loaded and will "
                                    "not be overwritten." ),
                                 aPath, p.jsonType );
    }

    const nlohmann::json& version = aDocument[JSON_MARKER_KEY]["format_version"];

    // Compare as JSON numbers before conversion, so oversized integers cannot wrap to 1.
    if( !version.is_number_integer() )
    {
        return wxString::Format( _( "File '%s' has an unrecognizable KemiCAD format_version %s." ),
                                 aPath, wxString::FromUTF8( version.dump() ) );
    }

    const auto raw = version.get<std::int64_t>();

    if( raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max() )
    {
        return wxString::Format( _( "File '%s' has an unsupported KemiCAD format_version %s." ),
                                 aPath, wxString::FromUTF8( version.dump() ) );
    }

    const wxString versionError = CheckFormatVersion( aDoc, static_cast<int>( raw ) );

    if( !versionError.IsEmpty() )
        return wxString::Format( _( "File '%s': %s" ), aPath, versionError );

    return wxEmptyString;
}

} // namespace KEMI_FORMAT
