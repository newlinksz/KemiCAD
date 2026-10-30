/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Licensed under the GNU General Public License, version 3 or later.
 */

#ifndef PIN_PAD_CONNECTION_H
#define PIN_PAD_CONNECTION_H

#include <map>
#include <set>
#include <utility>
#include <vector>
#include <advanced_config.h>
#include <connection_graph.h>
#include <connectivity/conn_facade.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_sheet_path.h>
#include <schematic.h>

enum class PIN_PAD_CONNECTION_KIND
{
    CONNECTED,
    EXPLICIT_NO_CONNECT,
    UNCONNECTED
};

using PIN_PAD_CONNECTIONS =
        std::map<std::pair<KIID, KIID_PATH>, PIN_PAD_CONNECTION_KIND>;

/**
 * Classify pins from the already rebuilt schematic connectivity, without changing the design.
 * A wire, label, power connection or another pin wins over a no-connect marker.  Only a bare
 * pin (possibly with a no-connect marker) is eligible for unconnected physical-pad resolution.
 */
inline PIN_PAD_CONNECTIONS CollectPinPadConnections( SCHEMATIC& aSchematic )
{
    PIN_PAD_CONNECTIONS result;

    struct GROUP
    {
        std::set<std::pair<KIID, KIID_PATH>> pins;
        bool hasConnection = false;
        bool hasNoConnect = false;

        void Add( SCH_ITEM* aItem, const KIID_PATH& aPath )
        {
            if( aItem->Type() == SCH_NO_CONNECT_T )
            {
                hasNoConnect = true;
            }
            else if( aItem->Type() == SCH_PIN_T )
            {
                auto* pin = static_cast<SCH_PIN*>( aItem );
                pins.emplace( pin->m_Uuid, aPath );
                hasNoConnect |= pin->GetType() == ELECTRICAL_PINTYPE::PT_NC;
                hasConnection |= pin->IsGlobalPower() || pin->IsLocalPower();
            }
            else
            {
                // Connectivity items include explicit labels, wires, junctions and sheet pins.
                // Unknown participating item kinds are conservatively treated as a connection.
                hasConnection = true;
            }
        }

        void Store( PIN_PAD_CONNECTIONS& aResult ) const
        {
            const PIN_PAD_CONNECTION_KIND kind =
                    hasConnection || pins.size() > 1 ? PIN_PAD_CONNECTION_KIND::CONNECTED :
                    hasNoConnect ? PIN_PAD_CONNECTION_KIND::EXPLICIT_NO_CONNECT :
                                   PIN_PAD_CONNECTION_KIND::UNCONNECTED;

            for( const auto& pin : pins )
                aResult[pin] = kind;
        }
    };

    if( ADVANCED_CFG::GetCfg().m_ConnectivityEngine )
    {
        for( const SCH_CONNECTIVITY::NET_GROUP& net : aSchematic.Connectivity().GetNetMap() )
        {
            GROUP group;

            for( const SCH_CONNECTIVITY::NET_VIEW& instance : net.instances )
            {
                if( !instance.IsNet() )
                    continue;

                for( SCH_ITEM* item : instance.Items() )
                    group.Add( item, instance.Instance() );
            }

            group.Store( result );
        }
    }
    else
    {
        for( const auto& [key, subgraphs] : aSchematic.ConnectionGraph()->GetNetMap() )
        {
            GROUP group;

            for( CONNECTION_SUBGRAPH* subgraph : subgraphs )
            {
                group.hasNoConnect |= subgraph->GetNoConnect()
                                      && subgraph->GetNoConnect()->Type() == SCH_NO_CONNECT_T;

                for( SCH_ITEM* item : subgraph->GetItems() )
                    group.Add( item, subgraph->GetSheet().Path() );
            }

            group.Store( result );
        }
    }

    // The connectivity engine isolates a PT_NC pin from its net, but a wire, label or another
    // pin physically touching it is still a real attachment.  Never let such a pin be treated
    // as a bare unconnected pin whose missing physical pad may be tolerated.
    for( const SCH_SHEET_PATH& path : aSchematic.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();

        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            for( SCH_PIN* pin : symbol->GetPins( &path ) )
            {
                auto it = result.find( { pin->m_Uuid, path.Path() } );

                if( it == result.end() || it->second == PIN_PAD_CONNECTION_KIND::CONNECTED )
                    continue;

                const VECTOR2I pos = pin->GetPosition();
                bool touched = false;

                for( SCH_ITEM* other : screen->Items().Overlapping( pos ) )
                {
                    if( touched )
                        break;

                    if( other->Type() == SCH_NO_CONNECT_T || !other->IsConnectable() )
                        continue;

                    if( other->Type() == SCH_SYMBOL_T )
                    {
                        for( SCH_PIN* otherPin : static_cast<SCH_SYMBOL*>( other )->GetPins( &path ) )
                            touched |= otherPin != pin && otherPin->GetPosition() == pos;
                    }
                    else
                    {
                        touched = other->IsConnected( pos );
                    }
                }

                if( touched )
                    it->second = PIN_PAD_CONNECTION_KIND::CONNECTED;
            }
        }
    }
    return result;
}

#endif
