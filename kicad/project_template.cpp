/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2012 Brian Sidebotham <brian.sidebotham@gmail.com>
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


#include <wx/bitmap.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/txtstrm.h>
#include <wx/wfstream.h>
#include <wx/log.h>
#include <wx/textfile.h>
#include <unordered_map>
#include <map>
#include <set>
#include <functional>
#include <exception>
#include <memory>
#include <utility>
#include <json_common.h>
#include <kemi_format_profile.h>
#include <kiid.h>
#include <kiway.h>
#include <pgm_base.h>
#include <richio.h>
#include <sexpr/sexpr.h>
#include <sexpr/sexpr_parser.h>

#include <kiplatform/io.h>
#include <wildcards_and_files_ext.h>
#include <wx_filename.h>
#include "project_template.h"


#define SEP wxFileName::GetPathSeparator()


PROJECT_TEMPLATE::PROJECT_TEMPLATE( const wxString& aPath, DOCUMENT_CONVERTER aConverter ) :
        m_converter( std::move( aConverter ) )
{
    m_basePath = wxFileName::DirName( aPath );
    m_metaPath = wxFileName::DirName( aPath + SEP + METADIR );
    m_metaHtmlFile = wxFileName::FileName( aPath + SEP + METADIR + SEP + METAFILE_INFO_HTML );
    m_metaIconFile = wxFileName::FileName( aPath + SEP + METADIR + SEP + METAFILE_ICON );

    m_title = wxEmptyString;

    // Test the project template requirements to make sure aPath is a valid template structure.
    if( !wxFileName::DirExists( m_basePath.GetPath() ) )
    {
        // Error, the path doesn't exist!
        m_error.Printf( _( "Could not open the template path '%s'" ), aPath );
    }
    else if( !wxFileName::DirExists( m_metaPath.GetPath() ) )
    {
        // Error, the meta information directory doesn't exist!
        m_error.Printf( _( "Could not find the expected 'meta' directory at '%s'" ), m_metaPath.GetPath() );
    }
    else if( !wxFileName::FileExists( m_metaHtmlFile.GetFullPath() ) )
    {
        // Error, the meta information directory doesn't contain the informational html file!
        m_error.Printf( _( "Could not find the expected meta HTML file at '%s'" ), m_metaHtmlFile.GetFullPath() );
    }

    if( m_title.IsEmpty() )
        m_title = GetPrjDirName();

    // Try to load an icon
    if( !wxFileName::FileExists( m_metaIconFile.GetFullPath() ) )
        m_metaIcon = &wxNullBitmap;
    else
        m_metaIcon = new wxBitmap( m_metaIconFile.GetFullPath(), wxBITMAP_TYPE_PNG );
}


class FILE_TRAVERSER : public wxDirTraverser
{
public:
    FILE_TRAVERSER( std::vector<wxFileName>& files, const wxString& exclude ) :
            m_files( files ),
            m_exclude( exclude )
    {
    }

    virtual wxDirTraverseResult OnFile( const wxString& filename ) override
    {
        wxFileName fn( filename );
        wxString   path( fn.GetPathWithSep() );

        EnsureGitFiles( path );

        if( IsIgnored( path, fn.GetFullName(), false ) )
            return wxDIR_CONTINUE;

        bool exclude = fn.GetName().Contains( "fp-info-cache" )
                       || fn.GetName().StartsWith( FILEEXT::LockFilePrefix );

        if( !exclude )
            m_files.emplace_back( wxFileName( filename ) );

        return wxDIR_CONTINUE;
    }

    virtual wxDirTraverseResult OnDir( const wxString& dirname ) override
    {
        wxFileName dir( dirname );
        wxString   parent = dir.GetPathWithSep();

        EnsureGitFiles( parent );

        if( dir.GetFullName() == wxT( ".git" ) || IsIgnored( parent, dir.GetFullName(), true )
            || dirname.StartsWith( m_exclude ) || dirname.EndsWith( "-backups" ) )
        {
            return wxDIR_IGNORE;
        }

        m_files.emplace_back( wxFileName::DirName( dirname ) );
        EnsureGitFiles( dirname + wxFileName::GetPathSeparator() );
        return wxDIR_CONTINUE;
    }

private:
    void EnsureGitFiles( const wxString& path )
    {
        if( m_gitIgnores.find( path ) != m_gitIgnores.end() )
            return;

        wxString gitignore = path + wxT( ".gitignore" );

        if( wxFileExists( gitignore ) )
        {
            wxFileInputStream input( gitignore );
            wxTextInputStream text( input, wxT( "\x9" ), wxConvUTF8 );

            while( input.IsOk() && !input.Eof() )
            {
                wxString line = text.ReadLine();

                line.Trim().Trim( false );

                if( line.IsEmpty() || line.StartsWith( wxT( "#" ) ) )
                    continue;

                m_gitIgnores[path].push_back( line );
            }

            m_files.emplace_back( wxFileName( gitignore ) );
        }
        else
        {
            m_gitIgnores[path] = {};
        }

        wxString gitattributes = path + wxT( ".gitattributes" );

        if( wxFileExists( gitattributes ) )
            m_files.emplace_back( wxFileName( gitattributes ) );
    }

    bool IsIgnored( const wxString& path, const wxString& name, bool isDir )
    {
        auto it = m_gitIgnores.find( path );

        if( it == m_gitIgnores.end() )
            return false;

        for( const wxString& pattern : it->second )
        {
            bool     dirOnly = pattern.EndsWith( wxT( "/" ) );
            wxString pat = dirOnly ? pattern.substr( 0, pattern.length() - 1 ) : pattern;

            if( dirOnly && !isDir )
                continue;

            if( wxMatchWild( pat, name ) )
                return true;
        }

        return false;
    }

    std::vector<wxFileName>&                            m_files;
    wxString                                            m_exclude;
    std::unordered_map<wxString, std::vector<wxString>> m_gitIgnores;
};


std::vector<wxFileName> PROJECT_TEMPLATE::GetFileList()
{
    std::vector<wxFileName> files;
    FILE_TRAVERSER          sink( files, m_metaPath.GetPath() );
    wxDir                   dir( m_basePath.GetPath() );

    dir.Traverse( sink, wxEmptyString, ( wxDIR_FILES | wxDIR_DIRS ) );
    return files;
}


wxString PROJECT_TEMPLATE::GetPrjDirName()
{
    return m_basePath.GetDirs()[m_basePath.GetDirCount() - 1];
}


PROJECT_TEMPLATE::~PROJECT_TEMPLATE()
{
}


wxFileName PROJECT_TEMPLATE::GetHtmlFile()
{
    return m_metaHtmlFile;
}


wxBitmap* PROJECT_TEMPLATE::GetIcon()
{
    return m_metaIcon;
}


namespace
{
using TEMPLATE_PATHS = std::map<wxString, wxString>;


/** Prepare every document before publishing, and roll back only this run's new files. */
class TEMPLATE_OUTPUTS
{
public:
    TEMPLATE_OUTPUTS( const wxString& aDirectory, size_t aFileCount, wxString* aError ) :
            m_error( aError )
    {
        m_prepared.reserve( aFileCount );
        m_published.reserve( aFileCount );

        if( !wxFileName::DirExists( aDirectory )
            && !wxFileName::Mkdir( aDirectory, 0777, wxPATH_MKDIR_FULL ) )
        {
            THROW_IO_ERRORF( _( "Cannot create folder '%s'." ), aDirectory );
        }

        m_directory = wxFileName( aDirectory, wxS( ".kemi-template-" ) + KIID().AsString() ).GetFullPath();

        if( !wxMkdir( m_directory ) )
            THROW_IO_ERRORF( _( "Cannot prepare template outputs in '%s'." ), aDirectory );
    }

    TEMPLATE_OUTPUTS( const TEMPLATE_OUTPUTS& ) = delete;
    TEMPLATE_OUTPUTS& operator=( const TEMPLATE_OUTPUTS& ) = delete;

    ~TEMPLATE_OUTPUTS()
    {
        if( !m_committed )
        {
            for( auto it = m_published.rbegin(); it != m_published.rend(); ++it )
                removeFile( *it );
        }

        for( const wxString& file : m_prepared )
            removeFile( file );

        // This directory is unique to this run. Never recursively remove destination folders.
        if( !wxRmdir( m_directory ) )
            reportCleanupError( m_directory );
    }

    wxFileName Prepare( const wxFileName& aTarget )
    {
        wxFileName staged( m_directory, wxString::Format( wxS( "%zu" ), m_prepared.size() ),
                           aTarget.GetExt() );
        m_prepared.push_back( staged.GetFullPath() );
        return staged;
    }

    void Publish( const wxFileName& aPrepared, const wxFileName& aTarget )
    {
        wxString targetPath = aTarget.GetFullPath();

        if( !aTarget.DirExists()
            && !wxFileName::Mkdir( aTarget.GetPath(), 0777, wxPATH_MKDIR_FULL ) )
        {
            THROW_IO_ERRORF( _( "Cannot create folder '%s'." ), aTarget.GetPath() );
        }

        // The no-overwrite rename also protects files created after the initial preflight.
        if( !wxRenameFile( aPrepared.GetFullPath(), targetPath, false ) )
            THROW_IO_ERRORF( _( "Cannot publish template file '%s'." ), targetPath );

        m_published.push_back( std::move( targetPath ) );
    }

    void Commit() { m_committed = true; }

private:
    void removeFile( const wxString& aPath )
    {
        if( wxFileExists( aPath ) && !wxRemoveFile( aPath ) )
            reportCleanupError( aPath );
    }

    void reportCleanupError( const wxString& aPath )
    {
        const wxString message = wxString::Format( _( "Cannot remove incomplete template output '%s'." ), aPath );

        if( m_error )
            *m_error += wxS( "\n" ) + message + wxS( "\n" );

        wxLogWarning( wxS( "%s" ), message );
    }

    wxString              m_directory;
    std::vector<wxString> m_prepared;
    std::vector<wxString> m_published;
    wxString*             m_error;
    bool                  m_committed = false;
};


wxString templatePathKey( wxFileName aFile )
{
    if( !aFile.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE ) )
        THROW_IO_ERROR( _( "Could not resolve a project template path." ) );

    wxString path = aFile.GetFullPath();

    // Directory entries and library-table URIs may spell the same path differently.
    while( path.length() > 1 && ( path.EndsWith( wxS( "/" ) ) || path.EndsWith( wxS( "\\" ) ) ) )
    {
        if( path.length() == 3 && path[1] == ':' )
            break;

        path.RemoveLast();
    }

    if( !wxFileName::IsCaseSensitive() )
        path.MakeLower();

    return path;
}


wxString templateBasename( const std::vector<wxFileName>& aFiles, const wxString& aDirectoryName )
{
    wxString basename;

    for( const wxFileName& file : aFiles )
    {
        if( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::PROJECT, file.GetExt() )
            || file.GetExt() == FILEEXT::LegacyProjectFileExtension )
        {
            if( !basename.IsEmpty() && basename != file.GetName() )
                return aDirectoryName;

            basename = file.GetName();
        }
    }

    return basename.IsEmpty() ? aDirectoryName : basename;
}


wxFileName templateDestination( const wxFileName& aSource, const wxFileName& aBasePath,
                                const wxFileName& aProject, const wxString& aBasename )
{
    wxFileName target( aSource );
    wxString name = target.GetName();
    const bool rescue = name.EndsWith( wxS( "-cache" ) ) || name.EndsWith( wxS( "-rescue" ) );
    const bool library = KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::SYMBOL_LIB, target.GetExt() )
            || target.GetExt() == FILEEXT::LegacySymbolDocumentFileExtension
            || target.GetExt() == FILEEXT::LegacySymbolLibFileExtension
            || target.GetPath().EndsWith( wxS( "." ) + wxString::FromUTF8( FILEEXT::KiCadFootprintLibPathExtension ) );

    if( !KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::DRAWING_SHEET, target.GetExt() )
        && ( rescue || !library ) )
    {
        name.Replace( aBasename, aProject.GetName() );
        target.SetName( name );
    }

    wxString path = target.GetPathWithSep();
    path.Replace( aBasePath.GetPathWithSep(), aProject.GetPathWithSep(), false );
    path.Replace( SEP + aBasename + SEP, SEP + aProject.GetName() + SEP );
    path.Replace( SEP + aBasename + wxS( "-" ), SEP + aProject.GetName() + wxS( "-" ) );
    target.SetPath( path );

    for( const auto& profile : KEMI_FORMAT::AllProfiles() )
    {
        if( KEMI_FORMAT::IsReadableExt( profile.doc, aSource.GetExt() ) )
        {
            target.SetExt( wxString::FromUTF8( profile.kemiExt ) );
            break;
        }
    }

    return target;
}


void remapTemplatePath( wxString& aValue, const wxFileName& aSource, const wxFileName& aTarget,
                        const wxFileName& aSourceRoot, const wxFileName& aProject,
                        const TEMPLATE_PATHS& aPaths )
{
    wxString value = aValue;
    wxString projectPrefix;

    for( const wxString& prefix : { wxString( wxS( "${KIPRJMOD}/" ) ),
                                    wxString( wxS( "$(KIPRJMOD)/" ) ),
                                    wxString( wxS( "${KIPRJMOD}\\" ) ),
                                    wxString( wxS( "$(KIPRJMOD)\\" ) ) } )
    {
        if( value.StartsWith( prefix ) )
        {
            projectPrefix = prefix;
            projectPrefix.Replace( wxS( "\\" ), wxS( "/" ) );
            value = aSourceRoot.GetPathWithSep() + value.Mid( prefix.Length() );
            break;
        }
    }

    // Environment-variable migration belongs to project import, not template instantiation.
    if( value.IsEmpty() || value.Contains( wxS( "$" ) ) )
        return;

    wxFileName reference( value );
    const bool absolute = reference.IsAbsolute();

    if( !reference.MakeAbsolute( aSource.GetPath() ) )
        THROW_IO_ERROR( _( "Could not resolve a project template reference." ) );

    auto found = aPaths.find( templatePathKey( reference ) );

    if( found == aPaths.end() )
        return;

    wxFileName destination( found->second );

    if( !projectPrefix.IsEmpty() )
    {
        if( !destination.MakeRelativeTo( aProject.GetPath() ) )
            THROW_IO_ERROR( _( "Could not make a template library reference relative." ) );

        aValue = projectPrefix + destination.GetFullPath( wxPATH_UNIX );
    }
    else
    {
        if( !absolute && !destination.MakeRelativeTo( aTarget.GetPath() ) )
            THROW_IO_ERROR( _( "Could not make a template reference relative." ) );

        aValue = destination.GetFullPath( wxPATH_UNIX );
    }
}


void writeTemplateDocument( const wxFileName& aSource, const wxFileName& aTarget,
                            const wxFileName& aOutput,
                            const wxFileName& aSourceRoot, const wxFileName& aProject,
                            const wxString& aBasename, const TEMPLATE_PATHS& aPaths,
                            const PROJECT_TEMPLATE::DOCUMENT_CONVERTER& aConverter )
{
    const KEMI_FORMAT::DOC_PROFILE* profile = nullptr;

    for( const auto& candidate : KEMI_FORMAT::AllProfiles() )
    {
        if( KEMI_FORMAT::IsReadableExt( candidate.doc, aSource.GetExt() ) )
        {
            profile = &candidate;
            break;
        }
    }

    const wxString name = aSource.GetFullName();
    const bool table = name == FILEEXT::SymbolLibraryTableFileName
            || name == FILEEXT::FootprintLibraryTableFileName
            || name == wxS( "design-block-lib-table" );

    if( !profile && !table )
    {
        const bool container = aSource.GetExt() == wxS( "kicad_dbl" )
                || aSource.GetExt() == wxS( "kicad_httplib" )
                || aSource.GetExt() == wxS( "kicad_block" )
                || aSource.GetExt() == wxS( "kicad_symdir" );

        if( aSource.GetExt() == FILEEXT::LegacyProjectFileExtension
            || aSource.GetExt() == FILEEXT::LegacySchematicFileExtension
            || aSource.GetExt() == FILEEXT::LegacyPcbFileExtension
            || ( !container && aTarget.GetFullName().Lower().Contains( wxS( ".kicad_" ) ) ) )
        {
            THROW_IO_ERROR( _( "Import legacy project templates before creating a native project." ) );
        }

        if( !wxCopyFile( aSource.GetFullPath(), aOutput.GetFullPath(), false ) )
            THROW_IO_ERRORF( _( "Cannot copy file '%s'." ), aTarget.GetFullPath() );

        KIPLATFORM::IO::MakeWriteable( aOutput.GetFullPath() );
        return;
    }

    if( profile )
    {
        KEMI_FORMAT::CheckPersistentOutput( profile->doc, aTarget.GetFullPath(),
                                            KEMI_FORMAT::FLAVOR::KEMI, true );
    }

    if( profile && profile->doc == KEMI_FORMAT::DOC::DESIGN_RULES )
    {
        bool saved = false;

        // Keep the original source path: rule validation needs its companion board and project.
        if( aConverter )
        {
            saved = aConverter( aSource.GetFullPath(), aOutput.GetFullPath() );
        }
        else
        {
            // A lazily loaded kiface retains its startup bus, so keep the bus alive.
            static KIWAY converterKiway( KFCTL_CPP_PROJECT_SUITE | ( Pgm().IsGUI() ? 0 : KFCTL_CLI ) );
            KIFACE* iface = converterKiway.KiFACE( KIWAY::FACE_PCB );
            saved = iface && iface->SaveNativeDocument( aSource.GetFullPath(), aOutput.GetFullPath() );
        }

        if( !saved )
            THROW_IO_ERRORF( _( "Cannot convert template rules '%s'." ), aSource.GetFullPath() );

        return;
    }

    wxFFile input( aSource.GetFullPath(), wxS( "rb" ) );
    wxString contents;

    if( !input.IsOpened() || !input.ReadAll( &contents, wxConvUTF8 ) )
        THROW_IO_ERRORF( _( "Cannot read template file '%s'." ), aSource.GetFullPath() );

    auto remap = [&]( wxString& aValue )
    {
        remapTemplatePath( aValue, aSource, aTarget, aSourceRoot, aProject, aPaths );
    };

    if( profile && profile->jsonType )
    {
        nlohmann::json document = nlohmann::json::parse( contents.ToStdString( wxConvUTF8 ) );
        const wxString error = KEMI_FORMAT::ValidateJsonMarker( document, profile->doc,
                                                               aSource.GetFullPath() );

        if( !error.IsEmpty() )
            THROW_IO_ERROR( error );

        auto remapJson = [&]( nlohmann::json& aValue )
        {
            if( aValue.is_string() )
            {
                wxString value = wxString::FromUTF8( aValue.get<std::string>() );
                remap( value );
                aValue = value.ToStdString( wxConvUTF8 );
            }
        };

        for( const char* path : { "/schematic/page_layout_descr_file", "/pcbnew/page_layout_descr_file" } )
        {
            const nlohmann::json::json_pointer pointer( path );

            if( document.contains( pointer ) )
                remapJson( document[pointer] );
        }

        const nlohmann::json::json_pointer sheets( "/schematic/top_level_sheets" );

        if( document.contains( sheets ) && document[sheets].is_array() )
        {
            for( auto& sheet : document[sheets] )
            {
                if( sheet.is_object() && sheet.contains( "filename" ) )
                    remapJson( sheet["filename"] );

                if( sheet.is_object() && sheet.value( "name", std::string() )
                        == aBasename.ToStdString( wxConvUTF8 ) )
                {
                    sheet["name"] = aProject.GetName().ToStdString( wxConvUTF8 );
                }
            }
        }

        for( const char* key : { "sheets", "boards" } )
        {
            if( document.contains( key ) && document[key].is_array() )
            {
                for( auto& entry : document[key] )
                {
                    if( entry.is_array() && entry.size() == 2 )
                        remapJson( entry[1] );
                }
            }
        }

        KEMI_FORMAT::StampJsonMarker( document, profile->doc );

        if( profile->doc == KEMI_FORMAT::DOC::PROJECT )
            document["meta"]["filename"] = aTarget.GetFullName().ToStdString( wxConvUTF8 );

        FILE_OUTPUTFORMATTER output( aOutput.GetFullPath() );
        output.Print( "%s\n", document.dump( 2 ).c_str() );
        output.Finish();
        return;
    }

    SEXPR::PARSER parser;
    std::unique_ptr<SEXPR::SEXPR> root = parser.Parse( contents.ToStdString( wxConvUTF8 ) );

    if( !root || !root->IsList() || !root->GetNumberOfChildren() || !root->GetChild( 0 )->IsSymbol() )
        THROW_IO_ERROR( _( "Invalid project template document root." ) );

    if( profile )
    {
        auto* token = static_cast<SEXPR::SEXPR_SYMBOL*>( root->GetChild( 0 ) );
        bool nativeRoot = token->m_value == profile->kemiRoot;

        if( !nativeRoot && ( !profile->kicadRoot || token->m_value != profile->kicadRoot ) )
        {
            THROW_IO_ERROR( _( "Unexpected project template document root." ) );
        }

        const wxString error = KEMI_FORMAT::CheckRootForSource( profile->doc, aSource.GetFullPath(), nativeRoot );

        if( !error.IsEmpty() )
            THROW_IO_ERROR( error );

        token->m_value = profile->kemiRoot;
        bool hasFormatVersion = false;

        for( size_t i = 1; i < root->GetNumberOfChildren(); ++i )
        {
            SEXPR::SEXPR* node = root->GetChild( i );

            if( !node->IsList() || !node->GetNumberOfChildren() || !node->GetChild( 0 )->IsSymbol() )
                continue;

            if( node->GetChild( 0 )->GetSymbol() == "format_version" )
            {
                if( hasFormatVersion || node->GetNumberOfChildren() != 2 || !node->GetChild( 1 )->IsInteger() )
                    THROW_IO_ERROR( _( "Invalid template format_version." ) );

                const auto version = node->GetChild( 1 )->GetLongInteger();

                if( version < KEMI_FORMAT::MIN_FORMAT_VERSION || version > KEMI_FORMAT::FORMAT_VERSION )
                    THROW_IO_ERROR( _( "Unsupported template format_version." ) );

                hasFormatVersion = true;
            }
            else if( node->GetChild( 0 )->GetSymbol() == "generator_version"
                     && node->GetNumberOfChildren() == 2 )
            {
                const std::string version = KEMI_FORMAT::GeneratorVersion().ToStdString( wxConvUTF8 );

                if( auto* value = dynamic_cast<SEXPR::SEXPR_STRING*>( node->GetChild( 1 ) ) )
                    value->m_value = version;
                else if( auto* value = dynamic_cast<SEXPR::SEXPR_SYMBOL*>( node->GetChild( 1 ) ) )
                    value->m_value = version;
            }
        }

        if( !hasFormatVersion )
        {
            if( nativeRoot )
                THROW_IO_ERROR( _( "Missing template format_version." ) );

            auto version = std::make_unique<SEXPR::SEXPR_LIST>();
            version->AddChild( new SEXPR::SEXPR_SYMBOL( "format_version" ) );
            version->AddChild( new SEXPR::SEXPR_INTEGER( KEMI_FORMAT::FORMAT_VERSION ) );
            auto& children = root->GetList()->m_children;
            const size_t position = profile->doc == KEMI_FORMAT::DOC::FOOTPRINT ? 2 : 1;

            if( children.size() < position )
                THROW_IO_ERROR( _( "Invalid template footprint name." ) );

            children.insert( children.begin() + position, version.release() );
        }
    }

    std::function<void( SEXPR::SEXPR*, const std::string& )> remapNode;
    remapNode = [&]( SEXPR::SEXPR* aNode, const std::string& aParent )
    {
        if( !aNode->IsList() || !aNode->GetNumberOfChildren() || !aNode->GetChild( 0 )->IsSymbol() )
            return;

        const std::string head = aNode->GetChild( 0 )->GetSymbol();
        size_t index = 0;

        if( table && head == "uri" )
            index = 1;
        else if( profile && profile->doc == KEMI_FORMAT::DOC::FOOTPRINT && head == "model" )
            index = 1;
        else if( profile && profile->doc == KEMI_FORMAT::DOC::PCB && head == "model" )
            index = 1;
        else if( profile && profile->doc == KEMI_FORMAT::DOC::SCHEMATIC && head == "property"
                 && aParent == "sheet" && aNode->GetNumberOfChildren() > 2
                 && aNode->GetChild( 1 )->IsString() && aNode->GetChild( 1 )->GetString() == "Sheetfile" )
        {
            index = 2;
        }

        if( index && aNode->GetNumberOfChildren() > index && aNode->GetChild( index )->IsString() )
        {
            auto* value = static_cast<SEXPR::SEXPR_STRING*>( aNode->GetChild( index ) );
            wxString path = wxString::FromUTF8( value->m_value );
            remap( path );
            value->m_value = path.ToStdString( wxConvUTF8 );
        }

        if( profile && profile->doc == KEMI_FORMAT::DOC::SCHEMATIC && head == "project"
            && aNode->GetNumberOfChildren() > 1 && aNode->GetChild( 1 )->IsString() )
        {
            auto* value = static_cast<SEXPR::SEXPR_STRING*>( aNode->GetChild( 1 ) );

            if( wxString::FromUTF8( value->m_value ) == aBasename )
                value->m_value = aProject.GetName().ToStdString( wxConvUTF8 );
        }

        for( size_t i = 1; i < aNode->GetNumberOfChildren(); ++i )
            remapNode( aNode->GetChild( i ), head );
    };

    remapNode( root.get(), std::string() );
    PRETTIFIED_FILE_OUTPUTFORMATTER output( aOutput.GetFullPath() );
    output.Print( "%s", root->AsString().c_str() );
    output.Finish();
}
} // namespace


size_t PROJECT_TEMPLATE::GetDestinationFiles( const wxFileName& aNewProjectPath,
                                               std::vector<wxFileName>& aDestFiles )
{
    const std::vector<wxFileName> sources = GetFileList();
    const wxString basename = templateBasename( sources, GetPrjDirName() );
    std::set<wxString> destinations;

    for( const wxFileName& source : sources )
    {
        const wxFileName target = templateDestination( source, m_basePath, aNewProjectPath, basename );

        if( destinations.insert( templatePathKey( target ) ).second )
            aDestFiles.push_back( target );
    }

    return aDestFiles.size();
}


bool PROJECT_TEMPLATE::CreateProject( wxFileName& aNewProjectPath, wxString* aErrorMsg )
{
    try
    {
        aNewProjectPath.Assign( KEMI_FORMAT::PersistentOutputPath( KEMI_FORMAT::DOC::PROJECT,
                                                                 aNewProjectPath.GetFullPath(), true ) );
        const std::vector<wxFileName> sources = GetFileList();
        const wxString basename = templateBasename( sources, GetPrjDirName() );
        TEMPLATE_PATHS paths;

        for( const wxFileName& source : sources )
        {
            paths.emplace( templatePathKey( source ),
                    templateDestination( source, m_basePath, aNewProjectPath, basename ).GetFullPath() );
        }

        std::set<wxString> plannedSources;
        std::set<wxString> destinations;
        std::vector<std::pair<wxFileName, wxFileName>> files;
        std::set<wxString> directories;

        // Check every final destination before creating directories or opening any output.
        for( const wxFileName& source : sources )
        {
            const wxString sourceKey = templatePathKey( source );

            if( !plannedSources.insert( sourceKey ).second )
                continue;

            if( source.IsDir() )
            {
                directories.insert( paths.at( sourceKey ) );
                continue;
            }

            if( !source.FileExists() )
                continue;

            bool retainedOriginal = false;

            for( const auto& profile : KEMI_FORMAT::AllProfiles() )
            {
                if( KEMI_FORMAT::IsLegacyExt( profile.doc, source.GetExt() ) )
                {
                    wxFileName native( source );
                    native.SetExt( wxString::FromUTF8( profile.kemiExt ) );
                    retainedOriginal = paths.contains( templatePathKey( native ) );
                    break;
                }
            }

            if( retainedOriginal )
                continue;

            const wxFileName target( paths.at( sourceKey ) );

            if( target.Exists() || !destinations.insert( templatePathKey( target ) ).second )
                THROW_IO_ERRORF( _( "Template destination '%s' already exists or is ambiguous." ), target.GetFullPath() );

            files.emplace_back( source, target );
        }

        TEMPLATE_OUTPUTS outputs( aNewProjectPath.GetPath(), files.size(), aErrorMsg );
        std::vector<wxFileName> prepared;
        prepared.reserve( files.size() );

        for( const auto& [source, target] : files )
        {
            const wxFileName staged = outputs.Prepare( target );
            writeTemplateDocument( source, target, staged, m_basePath, aNewProjectPath, basename,
                                    paths, m_converter );
            prepared.push_back( staged );
        }

        for( const wxString& directory : directories )
        {
            if( !wxFileName::DirExists( directory )
                && !wxFileName::Mkdir( directory, 0777, wxPATH_MKDIR_FULL ) )
            {
                THROW_IO_ERRORF( _( "Cannot create folder '%s'." ), directory );
            }
        }

        for( size_t i = 0; i < files.size(); ++i )
            outputs.Publish( prepared[i], files[i].second );

        outputs.Commit();
        return true;
    }
    catch( const IO_ERROR& error )
    {
        if( aErrorMsg )
            *aErrorMsg += error.What();
    }
    catch( const std::exception& error )
    {
        if( aErrorMsg )
            *aErrorMsg += wxString::FromUTF8( error.what() );
    }

    return false;
}


wxString* PROJECT_TEMPLATE::GetTitle()
{
    if( !GetHtmlFile().IsFileReadable() )
        return &m_title;

    if( m_title == wxEmptyString )
    {
        wxFFileInputStream input( GetHtmlFile().GetFullPath() );
        wxString           separator( wxT( "\x9" ) );
        wxTextInputStream  text( input, separator, wxConvUTF8 );

        int  start = 0;
        int  finish = 0;
        bool done = false;
        bool hasStart = false;

        while( input.IsOk() && !input.Eof() && !done )
        {
            wxString line = text.ReadLine();
            wxString upperline = line.Clone().Upper();

            start = upperline.Find( wxT( "<TITLE>" ) );
            finish = upperline.Find( wxT( "</TITLE>" ) );
            int length = finish - start - 7;

            // find the opening tag
            if( start != wxNOT_FOUND )
            {
                if( finish != wxNOT_FOUND )
                {
                    m_title = line( start + 7, length );
                    done = true;
                }
                else
                {
                    m_title = line.Mid( start + 7 );
                    hasStart = true;
                }
            }
            else
            {
                if( finish != wxNOT_FOUND )
                {
                    m_title += line.SubString( 0, finish - 1 );
                    done = true;
                }
                else if( hasStart )
                    m_title += line;
            }
        }

        // Remove line endings
        m_title.Replace( wxT( "\r" ), wxT( "" ) );
        m_title.Replace( wxT( "\n" ), wxT( "" ) );

        m_title.Trim( false ); // Trim from left
        m_title.Trim();        // Trim from right
    }

    return &m_title;
}


wxFileName EnsureDefaultProjectTemplate( const wxString& aBaseDir )
{
    if( aBaseDir.IsEmpty() )
        return wxFileName();

    wxFileName templatePath;
    templatePath.AssignDir( aBaseDir );
    templatePath.Normalize( FN_NORMALIZE_FLAGS | wxPATH_NORM_ENV_VARS );
    templatePath.AppendDir( wxT( "default" ) );

    if( !templatePath.DirExists() && !templatePath.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        return wxFileName();

    wxFileName metaDir = templatePath;
    metaDir.AppendDir( METADIR );

    if( !metaDir.DirExists() && !metaDir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        return wxFileName();

    wxFileName infoFile = metaDir;
    infoFile.SetFullName( METAFILE_INFO_HTML );

    if( !infoFile.FileExists() )
    {
        wxFFile info( infoFile.GetFullPath(), wxT( "w" ) );

        if( !info.IsOpened() )
            return wxFileName();

        info.Write( wxT( "<html><head><title>Default</title></head><body>"
                         "<h3>Default KemiCAD project template.</h3></body></html>" ) );
        info.Close();
    }

    wxFileName proFile = templatePath;
    proFile.SetFullName( wxS( "default.kemi_pro" ) );
    wxFileName official( proFile );
    official.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::LEGACY ) );

    // Existing official templates remain read-only sources for CreateProject().
    if( !proFile.FileExists() && official.FileExists() )
        return templatePath;

    if( !proFile.FileExists() )
    {
        try
        {
            nlohmann::json document = { { "meta", { { "version", 1 } } } };
            KEMI_FORMAT::StampJsonMarker( document, KEMI_FORMAT::DOC::PROJECT );
            KEMI_FORMAT::CheckPersistentOutput( KEMI_FORMAT::DOC::PROJECT, proFile.GetFullPath(),
                                                KEMI_FORMAT::FLAVOR::KEMI, true );
            FILE_OUTPUTFORMATTER output( proFile.GetFullPath() );
            output.Print( "%s\n", document.dump( 2 ).c_str() );
            output.Finish();
        }
        catch( const IO_ERROR& )
        {
            return wxFileName();
        }
    }

    if( infoFile.FileExists() && proFile.FileExists() )
        return templatePath;

    return wxFileName();
}
