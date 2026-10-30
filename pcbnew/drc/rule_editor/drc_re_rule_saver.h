/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 KiCad Developers, see AUTHORS.txt for contributors.
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

#ifndef DRC_RE_RULE_SAVER_H_
#define DRC_RE_RULE_SAVER_H_

#include <map>
#include <vector>

#include <wx/string.h>
#include <kemi_format_profile.h>

#include "drc_re_loaded_rule.h"

class BOARD;


/**
 * Saves DRC panel entries back to design-rule files using their persistent format profile.
 *
 * This class handles the conversion of graphical panel entries back to text-based
 * DRC rules. It supports round-trip preservation, meaning unedited rules will be
 * saved with their original text intact.
 */
class DRC_RULE_SAVER
{
public:
    DRC_RULE_SAVER();

    /**
     * Save all panel entries to a file.
     *
     * @param aPath Path to the output file.
     * @param aEntries Vector of panel entries to save.
     * @param aBoard Optional board for layer name resolution.
     * @param aTrivia Text outside the rules of the loaded file, reproduced around them.
     * @return True if the file was saved successfully.
     */
    bool SaveFile( const wxString&                               aPath,
                   const std::vector<DRC_RE_LOADED_PANEL_ENTRY>& aEntries,
                   const BOARD*                                  aBoard = nullptr,
                   const DRC_RE_FILE_TRIVIA&                     aTrivia = {} );

    /// Save the text editor's exact content, adding a native container only when absent.
    bool SaveTextFile( const wxString& aPath, const wxString& aText,
                       const BOARD* aBoard = nullptr, const DRC_RE_FILE_TRIVIA& aTrivia = {},
                       wxString* aError = nullptr );

    /// Detect the actual leading expression, ignoring comments and quoted rule text.
    static bool IsKemiText( const wxString& aText );

    /**
     * Generate rule text from panel entries.
     *
     * @param aEntries Vector of panel entries to convert.
     * @param aBoard Optional board for layer name resolution.
     * @param aTrivia Text outside the rules of the loaded file, reproduced around them.
     * @param aFlavor Output profile; native data cannot be emitted as legacy rules.
     * @return String containing all rules in DRC file format.
     */
    wxString GenerateRulesText( const std::vector<DRC_RE_LOADED_PANEL_ENTRY>& aEntries,
                                const BOARD*                                  aBoard = nullptr,
                                const DRC_RE_FILE_TRIVIA&                     aTrivia = {},
                                KEMI_FORMAT::FLAVOR aFlavor = KEMI_FORMAT::FLAVOR::LEGACY );

private:
    /**
     * Generate the rule text for a single panel entry.
     *
     * If the entry has not been edited and has original text, the original
     * text is returned for round-trip fidelity. Otherwise, the rule is
     * regenerated from the panel data.
     *
     * @param aEntry The panel entry to convert.
     * @param aBoard Optional board for layer name resolution.
     * @return Rule text in DRC file format.
     */
    wxString generateRuleText( const DRC_RE_LOADED_PANEL_ENTRY& aEntry,
                               const BOARD*                     aBoard );

    /**
     * Generate a layer clause from an LSET.
     *
     * @param aLayers The layer set.
     * @return Layer clause string like "(layer \"F.Cu\")", or empty for every layer.
     */
    wxString generateLayerClause( const LSET& aLayers );

    /**
     * Generate a severity clause.
     *
     * @param aSeverity The severity level.
     * @return Severity clause string like "(severity warning)".
     */
    wxString generateSeverityClause( SEVERITY aSeverity );

    /**
     * Generate a merged rule text from multiple panel entries with the same name/condition.
     *
     * Combines constraint clauses from multiple entries into a single rule.
     *
     * @param aEntries Vector of entries to merge (must have same name and condition).
     * @param aBoard Optional board for layer name resolution.
     * @return Merged rule text.
     */
    wxString generateMergedRuleText( const std::vector<const DRC_RE_LOADED_PANEL_ENTRY*>& aEntries,
                                      const BOARD*                                         aBoard );
};


#endif // DRC_RE_RULE_SAVER_H_
