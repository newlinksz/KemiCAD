/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Licensed under the GNU General Public License, version 3 or later.
 */

#ifndef FOOTPRINT_PAD_QUERY_H
#define FOOTPRINT_PAD_QUERY_H

#include <set>
#include <wx/intl.h>
#include <kiface_ids.h>
#include <kiway.h>

struct FOOTPRINT_PAD_QUERY
{
    bool loaded = false;
    std::set<wxString> numbers;
    wxString error;
};

/** Preserve a successfully loaded empty footprint separately from an unavailable library. */
inline FOOTPRINT_PAD_QUERY QueryFootprintPadNumbers( KIFACE* aFace, const wxString& aFootprint,
                                                    PROJECT* aProject )
{
    FOOTPRINT_PAD_QUERY result;

    if( aFace )
    {
        using LEGACY_FN = void ( * )( const wxString&, PROJECT*, std::set<wxString>& );

        // Existing kifaces can reliably report numbered pads, but cannot distinguish a loaded
        // empty footprint from a failed lookup.  Keep that ambiguity unless the new API resolves it.
        if( auto legacy = reinterpret_cast<LEGACY_FN>( aFace->IfaceOrAddress( KIFACE_FOOTPRINT_PAD_NUMBERS ) ) )
            legacy( aFootprint, aProject, result.numbers );

        if( !result.numbers.empty() )
        {
            result.loaded = true;
            return result;
        }

        using QUERY_FN = bool ( * )( const wxString&, PROJECT*, std::set<wxString>&, wxString& );

        if( auto query = reinterpret_cast<QUERY_FN>( aFace->IfaceOrAddress( KIFACE_FOOTPRINT_PAD_QUERY ) ) )
            result.loaded = query( aFootprint, aProject, result.numbers, result.error );
    }

    if( !result.loaded )
    {
        result.numbers.clear();

        if( result.error.IsEmpty() )
            result.error = _( "Footprint pad information is unavailable." );
    }

    return result;
}

#endif
