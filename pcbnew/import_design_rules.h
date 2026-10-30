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

#ifndef IMPORT_DESIGN_RULES_H
#define IMPORT_DESIGN_RULES_H

#include <wx/filename.h>
#include <wx/string.h>

class REPORTER;

/**
 * Save a template's official or native rules as a native document, preserving its text.
 *
 * Both the source and the written document are loaded by the DRC parser; a changed rule
 * count is an error. The caller owns the staging destination and publishes it afterwards.
 * Validation uses the source's same-stem board and project (native preferred) to resolve text
 * variables and cross-references as normal DRC does. Without a source board it uses an empty
 * board attached to the project. Source and destination text retain variables and comments.
 *
 * @throw IO_ERROR if reading, validation or writing fails, or the destination exists.
 */
void SaveNativeDesignRules( const wxString& aSource, const wxString& aTarget );

/**
 * Save the custom rules an importer derived beside the board it produced, as .kemi_dru.
 *
 * An existing rules file is the user's and is never replaced.
 *
 * Shared by the board editor import and the `kicad-cli import` job.
 *
 * @param[in] aRules is the rules text, nothing is written when it is empty.
 * @param[in] aBoardPath is the board the rules belong to, the rules take its name.
 * @param[in] aReporter receives a warning when the rules are not written.
 * @return true if the rules file was written.
 */
bool SaveImportedDesignRules( const wxString& aRules, const wxFileName& aBoardPath, REPORTER& aReporter );

#endif // IMPORT_DESIGN_RULES_H
