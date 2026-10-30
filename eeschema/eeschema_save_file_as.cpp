/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 */

#include <eeschema_save_file_as.h>

#include <gestfich.h>
#include <kemi_format_profile.h>
#include <kemi_project_save_as.h>
#include <libraries/library_table.h>
#include <macros.h>
#include <wildcards_and_files_ext.h>
#include <wx/filename.h>

void EESCHEMA_SaveFileAs( const wxString& aProjectBasePath, const wxString& aProjectName,
                        const wxString& aNewProjectBasePath, const wxString& aNewProjectName,
                        const wxString& aSrcFilePath, wxString& aErrors )
{
    wxFileName destFile( aSrcFilePath );
    wxFileName projectBase( aProjectBasePath );
    wxFileName newProjectBase( aNewProjectBasePath );
    wxUniChar  pathSep = wxFileName::GetPathSeparator();
    wxString   ext = destFile.GetExt().Lower();
    const bool renameSymbols = KEMI_FORMAT::IsManagedProjectImportCache(
            aProjectBasePath, aProjectName, KEMI_FORMAT::DOC::SYMBOL_LIB );
    const bool renameFootprints = KEMI_FORMAT::IsManagedProjectImportCache(
            aProjectBasePath, aProjectName, KEMI_FORMAT::DOC::FOOTPRINT );
    const bool nativeSchematic = KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::SCHEMATIC, ext );
    const bool nativeProject = wxFileName( aProjectBasePath, aProjectName,
            KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::KEMI ) ).FileExists();
    const wxString cacheError = KEMI_FORMAT::ProjectImportCacheRenameError(
            aProjectBasePath, aProjectName, aNewProjectName );

    if( !cacheError.empty() )
    {
        aErrors += cacheError;
        return;
    }

    // Normalize separators/absolute form. Do not use wxPATH_NORM_CASE: it lowercases
    // the filename on Windows and then case-sensitive GetName() matches miss the rename.
    destFile.Normalize( wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS );
    projectBase.Normalize( wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS );
    newProjectBase.Normalize( wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS );

    wxString destPath = destFile.GetPathWithSep();
    const wxString oldPrefix = projectBase.GetFullPath();
    const wxString oldPrefixSep = oldPrefix.Last() == pathSep ? oldPrefix : oldPrefix + pathSep;
    const wxString newPrefix = newProjectBase.GetFullPath();
    const wxString newPrefixSep = newPrefix.Last() == pathSep ? newPrefix : newPrefix + pathSep;

    if( destPath.Lower().StartsWith( oldPrefixSep.Lower() ) )
        destPath = newPrefixSep + destPath.Mid( oldPrefixSep.Length() );

    destFile.SetPath( destPath );

    if( ext == FILEEXT::LegacySchematicFileExtension
        || ext == FILEEXT::LegacySchematicFileExtension + FILEEXT::BackupFileSuffix
        || ext == FILEEXT::KiCadSchematicFileExtension
        || KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::SCHEMATIC, ext )
        || ext == wxString( wxS( "kemi_sch" ) ) + FILEEXT::BackupFileSuffix
        || ext == FILEEXT::KiCadSchematicFileExtension + FILEEXT::BackupFileSuffix )
    {
        if( destFile.GetName().IsSameAs( aProjectName, wxFileName::IsCaseSensitive() ) )
        {
            destFile.SetName( aNewProjectName  );
        }
        else if( destFile.GetName().IsSameAs( aNewProjectName, wxFileName::IsCaseSensitive() ) )
        {
            wxString msg;

            if( !aErrors.empty() )
                aErrors += wxS( "\n" );

            msg.Printf( _( "Cannot copy file '%s' as it will be overwritten by the new root "
                           "sheet file." ), destFile.GetFullPath() );
            aErrors += msg;
            return;
        }

        CopySexprFile( aSrcFilePath, destFile.GetFullPath(),
                [&]( const std::string& token, wxString& value ) -> bool
                {
                    if( token == "project" && value == aProjectName )
                    {
                        value = aNewProjectName;
                        return true;
                    }

                    if( nativeSchematic && renameSymbols
                        && ( token == "symbol" || token == "lib_id" )
                        && value.StartsWith( aProjectName + wxS( ":" ) ) )
                    {
                        value = aNewProjectName + value.Mid( aProjectName.Length() );
                        return true;
                    }

                    return false;
                },
                aErrors,
                [&]( const std::string& token, const wxString& name, wxString& value ) -> bool
                {
                    if( nativeSchematic && renameFootprints && token == "property"
                        && name == wxS( "Footprint" )
                        && value.StartsWith( aProjectName + wxS( ":" ) ) )
                    {
                        value = aNewProjectName + value.Mid( aProjectName.Length() );
                        return true;
                    }

                    return false;
                } );
    }
    else if( ext == FILEEXT::SchematicSymbolFileExtension )
    {
        // Symbols are not project-specific.  Keep their source names.
        KiCopyFile( aSrcFilePath, destFile.GetFullPath(), aErrors );
    }
    else if( ext == FILEEXT::LegacySymbolLibFileExtension
             || ext == FILEEXT::LegacySymbolDocumentFileExtension
             || KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::SYMBOL_LIB, ext )
             || ext == FILEEXT::KiCadSymbolLibFileExtension )
    {
        if( renameSymbols && KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::SYMBOL_LIB, ext )
            && wxFileName( aSrcFilePath ).SameAs( wxFileName(
                    aProjectBasePath, aProjectName,
                    KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::SYMBOL_LIB, KEMI_FORMAT::FLAVOR::KEMI ) ) ) )
        {
            destFile.SetName( aNewProjectName );
        }

        if( destFile.GetName().IsSameAs( aProjectName + wxS( "-cache" ), wxFileName::IsCaseSensitive() ) )
            destFile.SetName( aNewProjectName + wxS( "-cache" ) );

        if( destFile.GetName().IsSameAs( aProjectName + wxS( "-rescue" ), wxFileName::IsCaseSensitive() ) )
            destFile.SetName( aNewProjectName + wxS( "-rescue" ) );

        KiCopyFile( aSrcFilePath, destFile.GetFullPath(), aErrors );
    }
    else if( ext == FILEEXT::NetlistFileExtension )
    {
        if( destFile.GetName().IsSameAs( aProjectName, wxFileName::IsCaseSensitive() ) )
            destFile.SetName( aNewProjectName );

        CopySexprFile( aSrcFilePath, destFile.GetFullPath(),
                [&]( const std::string& token, wxString& value ) -> bool
                {
                    if( token == "source" )
                    {
                        for( const wxString& extension : { wxString( wxT( ".sch" ) ), wxString( wxT( ".kicad_sch" ) ) } )
                        {
                            if( value == aProjectName + extension )
                            {
                                value = aNewProjectName + extension;
                                return true;
                            }
                            else if( value == aProjectBasePath + "/" + aProjectName + extension )
                            {
                                value = aNewProjectBasePath + "/" + aNewProjectName + extension;
                                return true;
                            }
                            else if( value.StartsWith( aProjectBasePath ) )
                            {
                                value.Replace( aProjectBasePath, aNewProjectBasePath, false );
                                return true;
                            }
                        }
                    }

                    return false;
                },
                aErrors );
    }
    else if( destFile.GetName() == FILEEXT::SymbolLibraryTableFileName )
    {
        const bool rootTable = wxFileName( aSrcFilePath ).SameAs(
                wxFileName( aProjectBasePath, FILEEXT::SymbolLibraryTableFileName ) );

        if( nativeProject && !rootTable )
        {
            KiCopyFile( aSrcFilePath, destFile.GetFullPath(), aErrors );
            return;
        }

        wxFileName    libTableFn( aSrcFilePath );
        LIBRARY_TABLE libTable( libTableFn, LIBRARY_TABLE_SCOPE::PROJECT );
        libTable.SetPath( destFile.GetFullPath() );
        libTable.SetType( LIBRARY_TABLE_TYPE::SYMBOL );

        for( LIBRARY_TABLE_ROW& row : libTable.Rows() )
        {
            wxString uri = row.URI();

            if( rootTable && renameSymbols
                && KEMI_FORMAT::IsProjectImportCachePath(
                        uri, aProjectBasePath, aProjectName, KEMI_FORMAT::DOC::SYMBOL_LIB ) )
            {
                if( row.Nickname() == aProjectName )
                    row.SetNickname( aNewProjectName );

                // Aliases keep their nickname while following the renamed local library.
                row.SetURI( wxS( "${KIPRJMOD}/" ) + aNewProjectName + wxS( ".kemi_sym" ) );
                continue;
            }

            if( nativeProject )
                continue;

            uri.Replace( wxS( "/" ) + aProjectName + wxS( "-cache.lib" ),
                         wxS( "/" ) + aNewProjectName + wxS( "-cache.lib" ) );
            uri.Replace( wxS( "/" ) + aProjectName + wxS( "-rescue.lib" ),
                         wxS( "/" ) + aNewProjectName +  wxS( "-rescue.lib" ) );
            uri.Replace( wxS( "/" ) + aProjectName + wxS( ".lib" ),
                         wxS( "/" ) + aNewProjectName + wxS( ".lib" ) );

            row.SetURI( uri );
        }

        libTable.Save().map_error(
                [&]( const LIBRARY_ERROR& aError )
                {
                        wxString msg;

                        if( !aErrors.empty() )
                            aErrors += wxT( "\n" );

                        msg.Printf( _( "Cannot copy file '%s'." ), destFile.GetFullPath() );
                        aErrors += msg;
                } );
    }
    else
    {
        aErrors += wxString::Format( _( "Cannot copy file '%s': unsupported file type.\n" ),
                                     aSrcFilePath );
        wxFAIL_MSG( wxS( "Unexpected filetype for Eeschema::SaveFileAs()" ) );
    }
}
