/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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

#ifndef IMPORT_PROJ_PROPERTIES_H
#define IMPORT_PROJ_PROPERTIES_H

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <core/utf8.h>
#include <lib_id.h>
#include <wx/arrstr.h>
#include <wx/string.h>

/**
 * Property keys and codec threaded through IMPORT_PROJ_HELPER::m_properties to coordinate a
 * non-KiCad project import across the schematic and PCB editors.  Values ride the MAIL_IMPORT_FILE
 * payload, whose framing forbids '\n' inside a value, so list values join with the unit-separator
 * control character (a library nickname cannot contain it).  The codec lives here next to the
 * contract so the manager (encode) and both editor frames (decode) cannot drift.
 */
namespace IMPORT_PROJ_PROPS
{
inline constexpr char FP_CACHE_NICKNAME[]  = "import_fp_cache_nickname";
inline constexpr char SOURCE_FP_LIBS[]     = "import_source_fp_libs";
inline constexpr char SYM_CACHE_NICKNAME[] = "import_sym_cache_nickname";
inline constexpr char SOURCE_SYM_LIBS[]    = "import_source_sym_libs";
inline constexpr char NET_NAME_MAP[]       = "import_net_name_map";

/// Source project file, present when the importer is handed a project's sheets rather than one file.
inline constexpr char PROJECT_FILE[]       = "project_file";

/// Separator joining a list value within a single property.
inline constexpr char LIST_SEPARATOR = '\x1f';

/// Library-table row option key marking a row as a generated import cache.
inline constexpr char MANAGED_CACHE_KEY[] = "kicad_import_cache";

/// Options string identifying a library-table row as a generated import cache.
inline wxString ManagedCacheOption()
{
    return wxString( MANAGED_CACHE_KEY ) + wxS( "=1" );
}

/// Encode library nicknames into a single list-property value.
inline wxString JoinList( const wxArrayString& aItems )
{
    return wxJoin( aItems, LIST_SEPARATOR, '\0' );
}

/// Decode a list-property value back into nicknames, dropping empties.
inline std::vector<wxString> SplitList( const wxString& aValue )
{
    std::vector<wxString> out;

    for( const wxString& item : wxSplit( aValue, LIST_SEPARATOR, '\0' ) )
    {
        if( !item.IsEmpty() )
            out.push_back( item );
    }

    return out;
}

/// Leading field marking a value as an encoded net-name map, so a design that maps no nets is
/// still distinguishable from an import that never ran.
inline constexpr char NET_NAME_MAP_TAG[] = "netmap";

/// Leading field marking a value as an encoded map from reference to footprint LIB_ID.
inline constexpr char FOOTPRINT_ID_MAP_TAG[] = "fpids";

/// Validated, project-bound footprint metadata for one successful paired import.
struct FOOTPRINT_LINK_DATA
{
    wxString projectFile;
    std::map<wxString, wxString> ids;
    std::map<wxString, std::set<wxString>> pads;
};


inline wxString JoinFootprintLinkData( const FOOTPRINT_LINK_DATA& aData )
{
    if( aData.projectFile.IsEmpty() || aData.ids.size() != aData.pads.size() )
        return wxString();

    wxArrayString fields;
    fields.Add( wxS( "import-footprints" ) );
    fields.Add( aData.projectFile );
    fields.Add( wxString::Format( wxS( "%zu" ), aData.ids.size() ) );

    for( const auto& [ref, id] : aData.ids )
    {
        const auto pads = aData.pads.find( ref );

        if( pads == aData.pads.end() )
            return wxString();

        fields.Add( ref );
        fields.Add( id );
        fields.Add( wxString::Format( wxS( "%zu" ), pads == aData.pads.end() ? 0 : pads->second.size() ) );

        if( pads != aData.pads.end() )
        {
            for( const wxString& pad : pads->second )
                fields.Add( pad );
        }
    }

    return JoinList( fields );
}


inline std::optional<FOOTPRINT_LINK_DATA> SplitFootprintLinkData( const wxString& aValue )
{
    const wxArrayString fields = wxSplit( aValue, LIST_SEPARATOR, '\0' );
    unsigned long count = 0;

    if( fields.size() < 3 || fields[0] != wxS( "import-footprints" ) || fields[1].IsEmpty()
        || !fields[2].ToULong( &count ) || count > fields.size() )
    {
        return std::nullopt;
    }

    FOOTPRINT_LINK_DATA data;
    data.projectFile = fields[1];
    size_t cursor = 3;

    for( unsigned long i = 0; i < count; ++i )
    {
        unsigned long padCount = 0;

        if( cursor + 3 > fields.size() || fields[cursor].IsEmpty()
            || !fields[cursor + 2].ToULong( &padCount ) || padCount > fields.size() - cursor - 3 )
        {
            return std::nullopt;
        }

        const wxString ref = fields[cursor++];
        const wxString id = fields[cursor++];
        cursor++;

        if( !data.ids.emplace( ref, id ).second )
            return std::nullopt;

        std::set<wxString>& pads = data.pads[ref];

        for( unsigned long pad = 0; pad < padCount; ++pad )
        {
            if( fields[cursor].IsEmpty() || !pads.insert( fields[cursor++] ).second )
                return std::nullopt;
        }
    }

    if( cursor != fields.size() )
        return std::nullopt;

    return data;
}

/// Encode a string map as a single property value led by a tag.
inline wxString JoinTaggedMap( const char* aTag, const std::map<wxString, wxString>& aPairs )
{
    wxArrayString fields;
    fields.Add( aTag );

    for( const auto& [key, value] : aPairs )
    {
        fields.Add( key );
        fields.Add( value );
    }

    return JoinList( fields );
}

/// Decode a tagged string map property value, or nothing when the value is not one.
inline std::optional<std::map<wxString, wxString>> SplitTaggedMap( const char* aTag, const wxString& aValue )
{
    wxArrayString fields = wxSplit( aValue, LIST_SEPARATOR, '\0' );

    if( fields.IsEmpty() || fields[0] != aTag || fields.GetCount() % 2 == 0 )
        return std::nullopt;

    std::map<wxString, wxString> pairs;

    for( size_t i = 1; i < fields.GetCount(); i += 2 )
        pairs.emplace( fields[i], fields[i + 1] );

    return pairs;
}

/// Encode source-to-imported net names as a single property value.
inline wxString JoinNetNameMap( const std::map<wxString, wxString>& aNames )
{
    return JoinTaggedMap( NET_NAME_MAP_TAG, aNames );
}

/// Decode a net-name map property value, or nothing when the value is not one.
inline std::optional<std::map<wxString, wxString>> SplitNetNameMap( const wxString& aValue )
{
    return SplitTaggedMap( NET_NAME_MAP_TAG, aValue );
}

/// Read the footprint-import coordination properties out of a properties map.
inline void ReadFootprintProps( const std::map<std::string, UTF8>* aProps, wxString& aCacheNickname,
                                std::vector<wxString>& aSourceFpLibs )
{
    if( !aProps )
        return;

    if( auto it = aProps->find( FP_CACHE_NICKNAME ); it != aProps->end() )
        aCacheNickname = it->second.wx_str();

    if( auto it = aProps->find( SOURCE_FP_LIBS ); it != aProps->end() )
        aSourceFpLibs = SplitList( it->second.wx_str() );
}

/// Read the symbol-import coordination properties out of a properties map.
inline void ReadSymbolProps( const std::map<std::string, UTF8>* aProps, wxString& aCacheNickname,
                             std::vector<wxString>& aSourceSymLibs )
{
    if( !aProps )
        return;

    if( auto it = aProps->find( SYM_CACHE_NICKNAME ); it != aProps->end() )
        aCacheNickname = it->second.wx_str();

    if( auto it = aProps->find( SOURCE_SYM_LIBS ); it != aProps->end() )
        aSourceSymLibs = SplitList( it->second.wx_str() );
}

/**
 * Strip a trailing OrCAD/Allegro export timestamp of the form -YYYYMMDD-HHMM from a
 * project or design stem.  Leaves other hyphens and version tokens (e.g. -V7) alone.
 */
inline wxString StripImportDateSuffix( const wxString& aStem )
{
    // Match: ...-20YYMMDD-HHMM at end only.
    if( aStem.length() < 16 )
        return aStem;

    const size_t n = aStem.length();

    // Expect "-HHMM" then "-YYYYMMDD" immediately before the end.
    if( aStem[n - 5] != '-' || aStem[n - 14] != '-' )
        return aStem;

    auto isDigits = [&]( size_t start, size_t len )
    {
        for( size_t i = 0; i < len; ++i )
        {
            if( aStem[start + i] < '0' || aStem[start + i] > '9' )
                return false;
        }

        return true;
    };

    if( !isDigits( n - 4, 4 ) || !isDigits( n - 13, 8 ) )
        return aStem;

    // Require a 20xx/21xx-style year so random -12345678-1234 tails are kept.
    if( aStem[n - 13] != '2' )
        return aStem;

    return aStem.Left( n - 14 );
}

/// Derive the generated footprint-cache nickname from a project or file stem.
inline wxString MakeCacheNickname( const wxString& aStem )
{
    return LIB_ID::FixIllegalChars( StripImportDateSuffix( aStem ), true ).wx_str();
}

/// Derive the generated symbol-cache nickname from a project or file stem.
inline wxString MakeSymbolCacheNickname( const wxString& aStem )
{
    return LIB_ID::FixIllegalChars( StripImportDateSuffix( aStem ), true ).wx_str();
}
}

#endif // IMPORT_PROJ_PROPERTIES_H
