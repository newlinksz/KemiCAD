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

#ifndef LINK_IMPORTED_FOOTPRINTS_H
#define LINK_IMPORTED_FOOTPRINTS_H

#include <map>
#include <set>
#include <wx/string.h>

class REPORTER;
class SCHEMATIC;

enum class IMPORTED_FOOTPRINT_LINK_POLICY
{
    CACHE_RENAMES,
    FILL_EMPTY_ONLY
};

/**
 * Point the Footprint field of each symbol at the footprint with the same reference on the imported board.
 *
 * An empty field is filled. A field that holds a value is only changed when it is in the same library as the board's
 * footprint, which is how the import cache renames a footprint, and a footprint with no name is never used.
 * Shared by the project manager's import and the `kicad-cli import` job.
 *
 * @param[in] aSchematic is the imported schematic whose symbols are updated.
 * @param[in] aFootprintsByRef maps a board reference to the full LIB_ID of its footprint.
 * @param[in] aReporter receives a count of the symbols linked.
 * @param[in] aPolicy distinguishes an explicit cache rename from automatic paired-import
 *                   linking, which preserves every nonempty user field.
 * @param[in] aPadNumbersByRef supplies the validated physical pad sets for a paired import.
 *                            A connected pin without its target pad rejects all field changes;
 *                            explicit no-connect and bare unconnected pins retain warnings.
 * @return the number of symbols linked.
 * @throws IO_ERROR when paired-import pad coverage cannot safely be established.
 */
int LinkImportedFootprints( SCHEMATIC& aSchematic, const std::map<wxString, wxString>& aFootprintsByRef,
                            REPORTER& aReporter,
                            IMPORTED_FOOTPRINT_LINK_POLICY aPolicy = IMPORTED_FOOTPRINT_LINK_POLICY::CACHE_RENAMES,
                            const std::map<wxString, std::set<wxString>>* aPadNumbersByRef = nullptr );

#endif // LINK_IMPORTED_FOOTPRINTS_H
