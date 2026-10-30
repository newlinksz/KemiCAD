/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later version.
 */

#ifndef KEMI_PROJECT_SAVE_AS_H
#define KEMI_PROJECT_SAVE_AS_H

#include <import_proj_properties.h>
#include <kemi_format_profile.h>
#include <libraries/library_table.h>
#include <wildcards_and_files_ext.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/intl.h>

namespace KEMI_FORMAT
{

/** Match a URI to the exact project-root library path that Save As renames. */
inline bool IsProjectImportCachePath( const wxString& aUri, const wxString& aProjectDirectory,
                                     const wxString& aProjectName, DOC aLibraryType )
{
    const bool symbol = aLibraryType == DOC::SYMBOL_LIB;
    wxString uri = aUri;
    uri.Replace( wxS( "${KIPRJMOD}" ), aProjectDirectory );
    wxFileName actual( uri );
    const wxFileName expected( aProjectDirectory, aProjectName,
                               symbol ? Ext( DOC::SYMBOL_LIB, FLAVOR::KEMI ) : "pretty" );

    return actual.Normalize( wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS, aProjectDirectory )
           && actual.SameAs( expected )
           && ( symbol ? expected.FileExists() : wxDirExists( expected.GetFullPath() ) );
}


/** Only a marked, project-named local import cache follows a native project's Save As name. */
inline bool IsManagedProjectImportCache( const wxString& aProjectDirectory,
                                        const wxString& aProjectName, DOC aLibraryType )
{
    const wxFileName project( aProjectDirectory, aProjectName, Ext( DOC::PROJECT, FLAVOR::KEMI ) );

    if( !project.FileExists()
        || ( aLibraryType != DOC::SYMBOL_LIB && aLibraryType != DOC::FOOTPRINT ) )
    {
        return false;
    }

    const bool symbol = aLibraryType == DOC::SYMBOL_LIB;
    const wxFileName tablePath( aProjectDirectory,
                               symbol ? FILEEXT::SymbolLibraryTableFileName
                                      : FILEEXT::FootprintLibraryTableFileName );

    if( !tablePath.FileExists() )
        return false;

    LIBRARY_TABLE table( tablePath, LIBRARY_TABLE_SCOPE::PROJECT );
    LIBRARY_TABLE_ROW* row = table.Row( aProjectName ).value_or( nullptr );

    if( !table.IsOk() || !row || row->Type() != wxS( "KiCad" ) )
        return false;

    const auto options = row->GetOptionsMap();
    const auto managed = options.find( IMPORT_PROJ_PROPS::MANAGED_CACHE_KEY );

    if( managed == options.end() || managed->second.wx_str() != wxS( "1" ) )
        return false;

    return IsProjectImportCachePath( row->URI(), aProjectDirectory, aProjectName, aLibraryType );
}

/** Preflight both root tables before Save As can write a destination file or directory. */
inline wxString ProjectImportCacheRenameError( const wxString& aProjectDirectory,
                                               const wxString& aProjectName,
                                               const wxString& aNewProjectName )
{
    if( aProjectName == aNewProjectName )
        return wxString();

    for( DOC type : { DOC::SYMBOL_LIB, DOC::FOOTPRINT } )
    {
        if( !IsManagedProjectImportCache( aProjectDirectory, aProjectName, type ) )
            continue;

        const wxFileName tablePath( aProjectDirectory,
                                   type == DOC::SYMBOL_LIB ? FILEEXT::SymbolLibraryTableFileName
                                                          : FILEEXT::FootprintLibraryTableFileName );
        LIBRARY_TABLE table( tablePath, LIBRARY_TABLE_SCOPE::PROJECT );

        if( table.HasRow( aNewProjectName ) )
        {
            return wxString::Format(
                    _( "Cannot rename imported library '%s' to '%s': that nickname "
                       "already exists in '%s'.\n" ),
                    aProjectName, aNewProjectName, tablePath.GetFullPath() );
        }
    }

    return wxString();
}


} // namespace KEMI_FORMAT

#endif // KEMI_PROJECT_SAVE_AS_H
