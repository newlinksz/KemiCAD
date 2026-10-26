#
# This program source code file is part of KiCad, a free EDA CAD application.
#
# Copyright (C) 2023 Mark Roszko <mark.roszko@gmail.com>
# Copyright (C) 2023 KiCad Developers
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <https://www.gnu.org/licenses/>.
#

"""Parse internal KiCad versions without GUI or image-test dependencies."""

import re
from typing import Optional


def _numeric_kicad_major(output: Optional[str]) -> Optional[int]:
    """Accept only a leading numeric KiCad version, never a brand display string."""
    match = re.match(r"([0-9]+)(?:\.|$)", (output or "").strip())
    return int(match.group(1)) if match else None


def _parse_kicad_major(internal_out: Optional[str], plain_out: Optional[str]) -> int:
    """Prefer the internal major, accept legacy numeric plain, otherwise use 10."""
    for output in (internal_out, plain_out):
        major = _numeric_kicad_major(output)
        if major is not None:
            return major

    return 10
