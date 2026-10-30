/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef KEMI_BRAND_PATHS_H
#define KEMI_BRAND_PATHS_H

#include <brand.h>
#include <string_view>

// The brand version is independent of the upstream KiCad format and PCM versions.
#define KEMI_CONFIG_VERSION BRAND_CONFIG_VERSION

static_assert(
        std::string_view( BRAND_VERSION ).find( '.' ) != std::string_view::npos
                && std::string_view( BRAND_VERSION ).substr(
                           0, std::string_view( BRAND_VERSION ).find(
                                      '.', std::string_view( BRAND_VERSION ).find( '.' ) + 1 ) )
                           == KEMI_CONFIG_VERSION,
        "BRAND_CONFIG_VERSION must be the major.minor prefix of BRAND_VERSION" );

/** Brand path and environment identifiers; runtime adoption is a separate change. */
namespace KEMI_BRAND_PATHS
{

// Windows user directories. HOME overrides retain the upstream append semantics:
// config appends only the version; documents/cache append the brand and version.
inline constexpr char CONFIG_DIR_NAME[] = "kemicad";
inline constexpr char DOCUMENTS_DIR_NAME[] = "KemiCAD";
inline constexpr char CACHE_DIR_NAME[] = "KemiCAD";

inline constexpr char ENV_CONFIG_HOME[] = "KEMICAD_CONFIG_HOME";
inline constexpr char ENV_CONFIG_HOME_IS_QA[] = "KEMICAD_CONFIG_HOME_IS_QA";
inline constexpr char ENV_DOCUMENTS_HOME[] = "KEMICAD_DOCUMENTS_HOME";
inline constexpr char ENV_CACHE_HOME[] = "KEMICAD_CACHE_HOME";
inline constexpr char ENV_STOCK_DATA_HOME[] = "KEMICAD_STOCK_DATA_HOME";
inline constexpr char ENV_RUN_FROM_BUILD_DIR[] = "KEMICAD_RUN_FROM_BUILD_DIR";
inline constexpr char ENV_KEMICAD[] = "KEMICAD";

// Library paths deliberately have no version number.
inline constexpr char ENV_SYMBOL_DIR[] = "KEMICAD_SYMBOL_DIR";
inline constexpr char ENV_FOOTPRINT_DIR[] = "KEMICAD_FOOTPRINT_DIR";
inline constexpr char ENV_3DMODEL_DIR[] = "KEMICAD_3DMODEL_DIR";
inline constexpr char ENV_TEMPLATE_DIR[] = "KEMICAD_TEMPLATE_DIR";
inline constexpr char ENV_3RD_PARTY[] = "KEMICAD_3RD_PARTY";
inline constexpr char ENV_USER_TEMPLATE_DIR[] = "KEMICAD_USER_TEMPLATE_DIR";
inline constexpr char ENV_DESIGN_BLOCK_DIR[] = "KEMICAD_DESIGN_BLOCK_DIR";
inline constexpr char ENV_PTEMPLATES[] = "KEMICAD_PTEMPLATES";

inline constexpr char ENV_ENABLE_WXTRACE[] = "KEMICAD_ENABLE_WXTRACE";
inline constexpr char ENV_ALLOC_CONSOLE[] = "KEMICAD_ALLOC_CONSOLE";
inline constexpr char ENV_FORCE_CONSOLE_TRACE[] = "KEMICAD_FORCE_CONSOLE_TRACE";
inline constexpr char ENV_SOFTWARE_RENDERING[] = "KEMICAD_SOFTWARE_RENDERING";
inline constexpr char ENV_CURL_VERBOSE[] = "KEMICAD_CURL_VERBOSE";
inline constexpr char ENV_INHIBIT_SETTINGS_WRITES[] = "KEMICAD_INHIBIT_SETTINGS_WRITES";
inline constexpr char ENV_EDIT_ADVANCED_CFG[] = "KEMICAD_EDIT_ADVANCED_CFG";
inline constexpr char ENV_MERGETOOL_AUTO[] = "KEMICAD_MERGETOOL_AUTO";
inline constexpr char ENV_TEST_NOTI[] = "KEMICAD_TEST_NOTI";
inline constexpr char ENV_DEBUG_SYN_STYLE[] = "KEMICAD_DEBUG_SYN_STYLE";
inline constexpr char ENV_SHOW_GTK_MESSAGES[] = "KEMICAD_SHOW_GTK_MESSAGES";
inline constexpr char ENV_FORCE_SYN_ITALIC[] = "KEMICAD_FORCE_SYN_ITALIC";
inline constexpr char ENV_SYN_ITALIC_TILT[] = "KEMICAD_SYN_ITALIC_TILT";

// Private automation entry points share the same environment contract.
inline constexpr char ENV_CLI[] = "KEMICAD_CLI";
inline constexpr char ENV_WORK_ROOT[] = "KEMICAD_WORK_ROOT";

// KICAD_API_SOCKET/KICAD_API_TOKEN, KIPRJMOD and the five QA-only names
// retain their existing spelling; no replacement aliases are declared.

} // namespace KEMI_BRAND_PATHS

#endif // KEMI_BRAND_PATHS_H
