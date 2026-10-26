/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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

#ifndef KICAD_BRAND_H
#define KICAD_BRAND_H

#define BRAND_NAME "KemiCAD"
#define BRAND_NAME_W wxS( "KemiCAD" )
#define BRAND_DEFAULT_LANGUAGE wxLANGUAGE_CHINESE_SIMPLIFIED
/*
 * KemiCAD 0.9.1 = based on KiCad <KICAD_VERSION_FULL>.
 * The internal KiCad version stays unchanged for file formats and compatibility.
 */
#define BRAND_VERSION "0.9.1"
#define BRAND_VERSION_W wxS( "0.9.1" )
#define BRAND_RC_VER 0,9,1,0
#define BRAND_RC_VER_STR "0.9.1"

#ifndef RC_INVOKED // rc.exe cannot parse UTF-8 literals
#define BRAND_BASED_ON "基于 KiCad 开源软件"
#define BRAND_COMPANY "深圳新智联软件有限公司"
#endif

#endif // KICAD_BRAND_H
