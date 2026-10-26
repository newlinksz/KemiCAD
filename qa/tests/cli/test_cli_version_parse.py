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

"""Version parsing regressions; also runnable without pytest or a KiCad build."""

import unittest

from cli_version import _parse_kicad_major


class TestCliVersionParse(unittest.TestCase):
    def test_internal_version(self):
        """M-P01: the internal KiCad version supplies the library major."""
        self.assertEqual(_parse_kicad_major("10.99.0", None), 10)

    def test_legacy_plain_version(self):
        """M-P02: old binaries still supply a numeric plain version."""
        self.assertEqual(_parse_kicad_major(None, "10.0.6"), 10)

    def test_internal_beats_brand_version(self):
        """M-P03: a brand major must not override an internal version."""
        self.assertEqual(_parse_kicad_major("10.99.0", "KEMI EDA 2.0.0"), 10)

    def test_brand_never_supplies_major(self):
        """M-P04: display versions never become KICAD<major> variables."""
        for brand in ("KEMI EDA 2.0.0", "KEMI EDA 1.1.0"):
            with self.subTest(brand=brand):
                self.assertEqual(_parse_kicad_major(None, brand), 10)

    def test_future_internal_version(self):
        """M-P05: a future internal major is not hard-coded to 10."""
        self.assertEqual(_parse_kicad_major("11.0.0", "KEMI EDA 2.0.0"), 11)

    def test_invalid_internal_uses_legacy_plain(self):
        """M-P06: invalid internal output falls back to old numeric plain."""
        self.assertEqual(_parse_kicad_major("Invalid format", "9.0.6"), 9)

    def test_missing_or_invalid_versions_default(self):
        """M-P07: empty or nonnumeric outputs use the compatibility default."""
        for internal, plain in ((None, None), ("", ""), ("error", "KEMI EDA 2.0.0"),
                                ("10beta", "+9.0.0"), ("-10.0.0", "KiCad 9.0.0")):
            with self.subTest(internal=internal, plain=plain):
                self.assertEqual(_parse_kicad_major(internal, plain), 10)

    def test_numeric_plain_is_not_hard_coded(self):
        """M-P08: legacy fallback can supply a different valid major."""
        self.assertEqual(_parse_kicad_major(None, "11.0.1"), 11)

    def test_version_whitespace(self):
        """M-P09: console line endings and whitespace are ignored."""
        self.assertEqual(_parse_kicad_major("  11.0.0\r\n", None), 11)


if __name__ == "__main__":
    unittest.main()
