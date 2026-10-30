/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include <pcbnew_save_file_as.h>

#include <gestfich.h>
#include <kemi_format_profile.h>
#include <kemi_project_save_as.h>
#include <libraries/library_table.h>
#include <macros.h>
#include <wildcards_and_files_ext.h>
#include <wx/filename.h>

void PCBNEW_SaveFileAs( const wxString& aProjectBasePath, const wxString& aSrcProjectName,
                        const wxString& aNewProjectBasePath, const wxString& aNewProjectName,
                        const wxString& aSrcFilePath, wxString& aErrors )
{
    wxFileName destFile( aSrcFilePath );
    wxFileName projectBase( aProjectBasePath );
    wxFileName newProjectBase( aNewProjectBasePath );
    wxUniChar  pathSep = wxFileName::GetPathSeparator();
    wxString   ext = destFile.GetExt().Lower();
    const bool renameFootprints = KEMI_FORMAT::IsManagedProjectImportCache(
            aProjectBasePath, aSrcProjectName, KEMI_FORMAT::DOC::FOOTPRINT );
    const bool nativeBoard = KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::PCB, ext );
    const bool nativeProject = wxFileName( aProjectBasePath, aSrcProjectName,
            KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::KEMI ) ).FileExists();
    const wxString cacheError = KEMI_FORMAT::ProjectImportCacheRenameError(
            aProjectBasePath, aSrcProjectName, aNewProjectName );

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


    wxString srcProjectFootprintLib = pathSep + aSrcProjectName + wxT( ".pretty" ) + pathSep;
    wxString newProjectFootprintLib = pathSep + aNewProjectName + wxT( ".pretty" ) + pathSep;

    if( !nativeProject )
    {
        destPath.Replace( srcProjectFootprintLib, newProjectFootprintLib, true );
    }
    else if( renameFootprints )
    {
        const wxString cachePath = newPrefixSep + aSrcProjectName + wxS( ".pretty" ) + pathSep;

        if( destPath.StartsWith( cachePath ) )
        {
            destPath = newPrefixSep + aNewProjectName + wxS( ".pretty" ) + pathSep
                       + destPath.Mid( cachePath.Length() );
        }
    }

    destFile.SetPath( destPath );

    if( ext == FILEEXT::KiCadPcbFileExtension
        || ext == FILEEXT::KiCadPcbFileExtension + FILEEXT::BackupFileSuffix
        || KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::PCB, ext )
        || ext == wxString( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PCB, KEMI_FORMAT::FLAVOR::KEMI ) )
                       + FILEEXT::BackupFileSuffix )
    {
        if( destFile.GetName().IsSameAs( aSrcProjectName, wxFileName::IsCaseSensitive() ) )
            destFile.SetName( aNewProjectName  );

        CopySexprFile( aSrcFilePath, destFile.GetFullPath(),
                [&]( const std::string& token, wxString& value )
                {
                    if( nativeBoard && renameFootprints && token == "footprint"
                        && value.StartsWith( aSrcProjectName + wxS( ":" ) ) )
                    {
                        value = aNewProjectName + value.Mid( aSrcProjectName.Length() );
                        return true;
                    }

                    if( token == "sheetfile" )
                    {
                        for( const wxString extension : { wxT( ".sch" ), wxT( ".kicad_sch" ),
                                                          wxT( ".kemi_sch" ) } )
                        {
                            if( value == aSrcProjectName + extension )
                            {
                                value = aNewProjectName + extension;
                                return true;
                            }
                            else if( value == aProjectBasePath + "/" + aSrcProjectName + extension )
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
    else if( ext == FILEEXT::LegacyPcbFileExtension )
    {
        if( destFile.GetName().IsSameAs( aSrcProjectName, wxFileName::IsCaseSensitive() ) )
            destFile.SetName( aNewProjectName );

        KiCopyFile( aSrcFilePath, destFile.GetFullPath(), aErrors );
    }
    else if( ext == FILEEXT::LegacyFootprintLibPathExtension
             || ext == FILEEXT::KiCadFootprintFileExtension
             || KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::FOOTPRINT, ext ) )
    {
        // Footprints are not project-specific.  Keep their source names.
        KiCopyFile( aSrcFilePath, destFile.GetFullPath(), aErrors );
    }
    else if( ext == FILEEXT::FootprintAssignmentFileExtension )
    {
        // TODO
    }
    else if( ext == wxT( "rpt" ) )
    {
        // DRC must be the "gold standard".  Since we can't guarantee that there aren't
        // any non-deterministic cases in the save-as algorithm, we don't want to certify
        // the result with the source's DRC report.  Therefore copy it under the old
        // name.
        KiCopyFile( aSrcFilePath, destFile.GetFullPath(), aErrors );
    }
    else if( destFile.GetName() == FILEEXT::FootprintLibraryTableFileName )
    {
        const bool rootTable = wxFileName( aSrcFilePath ).SameAs(
                wxFileName( aProjectBasePath, FILEEXT::FootprintLibraryTableFileName ) );

        if( nativeProject && !rootTable )
        {
            KiCopyFile( aSrcFilePath, destFile.GetFullPath(), aErrors );
            return;
        }

        wxFileName    libTableFn( aSrcFilePath );
        LIBRARY_TABLE libTable( libTableFn, LIBRARY_TABLE_SCOPE::PROJECT );
        libTable.SetPath( destFile.GetFullPath() );
        libTable.SetType( LIBRARY_TABLE_TYPE::FOOTPRINT );

        for( LIBRARY_TABLE_ROW& row : libTable.Rows() )
        {
            wxString uri = row.URI();

            if( rootTable && renameFootprints
                && KEMI_FORMAT::IsProjectImportCachePath(
                        uri, aProjectBasePath, aSrcProjectName, KEMI_FORMAT::DOC::FOOTPRINT ) )
            {
                if( row.Nickname() == aSrcProjectName )
                    row.SetNickname( aNewProjectName );

                // Aliases keep their nickname while following the renamed local library.
                row.SetURI( wxS( "${KIPRJMOD}/" ) + aNewProjectName + wxS( ".pretty" ) );
                continue;
            }

            // Native projects preserve third-party paths that merely share a basename.
            if( nativeProject )
                continue;

            uri.Replace( wxT( "/" ) + aSrcProjectName + wxT( ".pretty" ),
                         wxT( "/" ) + aNewProjectName + wxT( ".pretty" ) );

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
        wxFAIL_MSG( wxT( "Unexpected filetype for Pcbnew::SaveFileAs()" ) );
    }
}
