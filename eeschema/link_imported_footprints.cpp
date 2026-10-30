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

#include "link_imported_footprints.h"

#include <set>
#include <vector>

#include <lib_id.h>
#include <ki_exception.h>
#include <lib_symbol.h>
#include <pin_pad_connection.h>
#include <reporter.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <sch_pin.h>
#include <string_utils.h>


int LinkImportedFootprints( SCHEMATIC& aSchematic, const std::map<wxString, wxString>& aFootprintsByRef,
                            REPORTER& aReporter, IMPORTED_FOOTPRINT_LINK_POLICY aPolicy,
                            const std::map<wxString, std::set<wxString>>* aPadNumbersByRef )
{
    struct OCCURRENCE
    {
        SCH_SYMBOL* symbol;
        SCH_SHEET_PATH path;
        SCH_SCREEN* screen;
    };

    std::map<wxString, std::vector<OCCURRENCE>> components;

    for( const SCH_SHEET_PATH& path : aSchematic.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();

        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            const wxString ref = symbol->GetRef( &path );
            components[ref].push_back( { symbol, path, screen } );
        }
    }

    // A reused sheet shares its symbol fields. Every occurrence must name the same
    // physical footprint before any of those shared fields can be changed.
    std::map<SCH_SYMBOL*, std::set<wxString>> targets;
    std::map<SCH_SYMBOL*, SCH_SCREEN*> screens;

    for( const auto& [ref, occurrences] : components )
    {
        const auto footprint = aFootprintsByRef.find( ref );
        wxString target = footprint == aFootprintsByRef.end() ? wxString() : footprint->second;
        LIB_ID targetId;

        if( ref.IsEmpty() || ref.EndsWith( wxS( "?" ) ) )
            target.clear();

        if( !target.IsEmpty()
            && ( targetId.Parse( target ) >= 0 || targetId.GetLibNickname().empty()
                 || targetId.GetLibItemName().empty() ) )
        {
            aReporter.Report( wxString::Format( _( "IMPORT_LINK_INVALID_ID: skipping %s: '%s' is not a full footprint ID." ),
                                                ref, target ), RPT_SEVERITY_WARNING );
            target.clear();
        }

        if( !target.IsEmpty() && occurrences.size() > 1 )
        {
            std::set<int> units;
            const SCH_SYMBOL* first = occurrences.front().symbol;
            bool uniqueUnits = true;

            for( const OCCURRENCE& occurrence : occurrences )
            {
                const SCH_SYMBOL* symbol = occurrence.symbol;

                uniqueUnits &= symbol->GetUnitCount() > 1 && symbol->GetUnit() > 0
                               && symbol->GetUnit() <= symbol->GetUnitCount()
                               && symbol->GetLibId() == first->GetLibId()
                               && symbol->GetUnitCount() == first->GetUnitCount()
                               && symbol->GetLibSymbolRef() && first->GetLibSymbolRef()
                               && symbol->GetLibSymbolRef()->GetName() == first->GetLibSymbolRef()->GetName()
                               && units.insert( symbol->GetUnit() ).second;
            }

            if( !uniqueUnits )
            {
                aReporter.Report( wxString::Format( _( "IMPORT_LINK_DUPLICATE_REF: skipping ambiguous schematic reference %s." ),
                                                    ref ), RPT_SEVERITY_WARNING );
                target.clear();
            }
        }

        // A conflicting unit's user choice vetoes the entire physical component.
        // Normal paired imports never infer that a nonempty field is a cache rename.
        if( !target.IsEmpty() )
        {
            for( const OCCURRENCE& occurrence : occurrences )
            {
                const SCH_SYMBOL* symbol = occurrence.symbol;
                const wxString current = symbol->GetField( FIELD_T::FOOTPRINT )->GetText();
                LIB_ID currentId;

                const bool keep = !current.IsEmpty() && current != target
                                  && ( aPolicy == IMPORTED_FOOTPRINT_LINK_POLICY::FILL_EMPTY_ONLY
                                       || currentId.Parse( current ) >= 0
                                       || currentId.GetLibNickname() != targetId.GetLibNickname() );

                if( keep || symbol->GetExcludedFromBoard( &occurrence.path ) )
                {
                    aReporter.Report( wxString::Format( _( "IMPORT_LINK_PRESERVED: keeping existing fields for %s (user choice or board exclusion)." ),
                                                        ref ), RPT_SEVERITY_WARNING );
                    target.clear();
                    break;
                }
            }
        }

        for( const OCCURRENCE& occurrence : occurrences )
        {
            targets[occurrence.symbol].insert( target );
            screens[occurrence.symbol] = occurrence.screen;
        }
    }

    if( aPadNumbersByRef )
    {
        aSchematic.RebuildConnectivity();
        const PIN_PAD_CONNECTIONS connections = CollectPinPadConnections( aSchematic );

        // Validate every candidate before modifying the first field. Pin maps are
        // consumed as supplied by the source; this operation never invents a map.
        for( const auto& [ref, occurrences] : components )
        {
            const auto footprint = aFootprintsByRef.find( ref );

            if( footprint == aFootprintsByRef.end() )
                continue;

            for( const OCCURRENCE& occurrence : occurrences )
            {
                const auto& candidates = targets[occurrence.symbol];

                if( candidates.size() != 1 || candidates.begin()->IsEmpty() )
                    continue;

                const auto pads = aPadNumbersByRef->find( ref );

                if( pads == aPadNumbersByRef->end() )
                    THROW_IO_ERROR( _( "Imported footprint link metadata is missing its physical pad set." ) );

                LIB_ID targetId;
                targetId.Parse( footprint->second );

                for( const SCH_PIN* pin : occurrence.symbol->GetPins( &occurrence.path ) )
                {
                    SCH_PIN::PAD_RESOLUTION state;
                    const wxString resolved = pin->GetEffectivePadNumber(
                            occurrence.path, aSchematic.GetCurrentVariant(), targetId, &pads->second, &state );
                    bool valid = false;
                    const auto resolvedPads = ExpandStackedPinNotation( resolved, &valid );
                    bool covered = valid && !resolvedPads.empty();

                    for( const wxString& pad : resolvedPads )
                        covered &= pads->second.contains( pad );

                    if( covered && state != SCH_PIN::PAD_RESOLUTION::UNMAPPED )
                        continue;

                    const auto connection = connections.find( { pin->m_Uuid, occurrence.path.Path() } );
                    const PIN_PAD_CONNECTION_KIND kind = connection == connections.end()
                                                                 ? PIN_PAD_CONNECTION_KIND::CONNECTED
                                                                 : connection->second;

                    if( kind == PIN_PAD_CONNECTION_KIND::CONNECTED )
                    {
                        THROW_IO_ERROR( wxString::Format(
                                _( "IMPORT_LINK_CONNECTED_PIN_MISSING_PAD: %s pin %s has a connection but no matching physical pad in %s. No footprint fields were changed." ),
                                ref, pin->GetNumber(), footprint->second ) );
                    }

                    aReporter.Report( wxString::Format(
                            kind == PIN_PAD_CONNECTION_KIND::EXPLICIT_NO_CONNECT
                                    ? _( "IMPORT_LINK_NC_PIN: %s pin %s has an explicit no-connect and no physical pad in %s; the source pin and no-connect are preserved." )
                                    : _( "IMPORT_LINK_UNCONNECTED_PIN: %s pin %s is unconnected, without an explicit no-connect, and has no physical pad in %s; the source pin is preserved." ),
                            ref, pin->GetNumber(), footprint->second ), RPT_SEVERITY_WARNING );
                }
            }
        }
    }

    int linked = 0;

    for( const auto& [symbol, footprints] : targets )
    {
        if( footprints.size() != 1 || footprints.begin()->IsEmpty() )
            continue;

        if( symbol->GetField( FIELD_T::FOOTPRINT )->GetText() == *footprints.begin() )
            continue;

        symbol->SetFootprintFieldText( *footprints.begin() );
        screens[symbol]->SetContentModified();
        linked++;
    }

    if( linked > 0 )
    {
        aReporter.Report( wxString::Format( _( "Linked %d symbol(s) to the footprint with the same reference.\n" ),
                                            linked ),
                          RPT_SEVERITY_INFO );
    }

    return linked;
}
