/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 1992-2013 jp.charras at wanadoo.fr
 * Copyright (C) 2013 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
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

#include "netlist_exporter_kicad.h"

#include <algorithm>
#include <confirm.h>

#include <connection_graph.h>
#include <richio.h>
#include <xnode.h>


bool NETLIST_EXPORTER_KICAD::writeNetlist( const wxString& aOutFileName, unsigned aNetlistOptions,
                                           REPORTER& aReporter )
{
    try
    {
        m_exportVariant.clear();

        if( !ValidateFootprintPadMappings( GNL_ALL | GNL_OPT_KICAD, aReporter ) )
            return false;

        PRETTIFIED_FILE_OUTPUTFORMATTER formatter( aOutFileName );

        if( !Format( &formatter, GNL_ALL | GNL_OPT_KICAD, &aReporter ) )
            return false;
        formatter.Finish();
    }

    catch( const IO_ERROR& ioe )
    {
        aReporter.Report( ioe.What(), RPT_SEVERITY_ERROR );
        return false;
    }

    return true;
}


bool NETLIST_EXPORTER_KICAD::Format( OUTPUTFORMATTER* aOut, int aCtl, REPORTER* aReporter )
{
    // pcbnew resolves variants itself from the base design.
    m_exportVariant = ( aCtl & GNL_OPT_KICAD ) ? wxString() : m_schematic->GetCurrentVariant();

    CONNECTIVITY_SCOPE connectivity( *this );
    REPORTER& reporter = aReporter ? *aReporter : WXLOG_REPORTER::GetInstance();

    if( !ValidateFootprintPadMappings( aCtl, reporter ) )
        return false;

    std::unique_ptr<XNODE> xroot( makeRoot( aCtl ) );

    xroot->Format( aOut );
    return true;
}
