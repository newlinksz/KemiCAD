/*
 * This program source code file is part of KemiCAD.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 */

#ifndef KEMI_PROJECT_PATHS_H
#define KEMI_PROJECT_PATHS_H

#include <kemi_format_profile.h>
#include <wx/filename.h>

namespace KEMI_FORMAT
{

/** Preserve a project's native/official flavor when finding one of its related documents. */
inline wxString RelatedDocumentPath( DOC aSourceDoc, DOC aTargetDoc, const wxString& aSourcePath )
{
    if( aSourcePath.IsEmpty() )
        return wxEmptyString;

    wxFileName target( aSourcePath );
    target.SetExt( Ext( aTargetDoc, FlavorForPath( aSourceDoc, aSourcePath ) ) );
    return target.GetFullPath();
}

/** Keep project-tree navigation mail independent of the Windows locale. */
inline std::string NavigationPathToUtf8( const wxString& aPath )
{
    return aPath.ToStdString( wxConvUTF8 );
}


inline wxString NavigationPathFromUtf8( const std::string& aPath )
{
    return wxString::FromUTF8( aPath );
}

} // namespace KEMI_FORMAT

#endif // KEMI_PROJECT_PATHS_H
