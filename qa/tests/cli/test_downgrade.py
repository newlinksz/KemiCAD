"""Import-only save policy. Source checks run without a build.

Set KICAD_CLI to a freshly built kemicad-cli to enable executable checks.
"""

# This program source code file is part of KiCad, a free EDA CAD application.
# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
# SPDX-License-Identifier: GPL-3.0-or-later

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "qa/data/cli/basic_test"
CLI = os.environ.get("KICAD_CLI")


class TestSavePolicySources(unittest.TestCase):
    def test_downgrade_is_not_registered_in_cli(self):
        source = (ROOT / "kicad/kicad_cli.cpp").read_text(encoding="utf-8")
        for name in ("pcbDowngradeCmd", "schDowngradeCmd", "projectDowngradeCmd"):
            with self.subTest(command=name):
                self.assertNotIn(name, source)

    def test_older_version_export_has_no_gui_action(self):
        for name in ("kicad/menubar.cpp", "kicad/tools/kicad_manager_actions.cpp",
                     "kicad/tools/kicad_manager_actions.h",
                     "kicad/tools/kicad_manager_control.cpp",
                     "kicad/tools/kicad_manager_control.h"):
            with self.subTest(source=name):
                self.assertNotIn("exportForOlderVersion",
                                 (ROOT / name).read_text(encoding="utf-8"))


@unittest.skipUnless(CLI, "Set KICAD_CLI to the newly built CLI")
class TestNativeSaveLifecycle(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="kemi_save_policy_")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.env = os.environ.copy()
        self.env["KICAD_CONFIG_HOME"] = str(self.root / "config")
        self.env["KEMICAD_CONFIG_HOME"] = str(self.root / "config")
        self.env["LC_ALL"] = "C"

    def cli(self, *args):
        return subprocess.run([CLI, *map(str, args)], env=self.env,
                              capture_output=True, text=True, errors="replace", timeout=60)

    def test_downgrade_is_unavailable_even_with_help_or_force(self):
        for kind in ("sch", "pcb", "project"):
            for option in ("--help", "--force"):
                with self.subTest(kind=kind, option=option):
                    result = self.cli(kind, "downgrade", option)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            result = self.cli(kind, "--help")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("downgrade", result.stdout.lower())

    def test_upgrade_writes_native_sibling_and_never_overwrites_it(self):
        for kind, suffix, root in (("sch", "sch", b"(kemi_sch"),
                                   ("pcb", "pcb", b"(kemi_pcb")):
            with self.subTest(kind=kind):
                directory = self.root / kind
                directory.mkdir()
                source = directory / f"source.kicad_{suffix}"
                shutil.copyfile(DATA / f"basic_test.kicad_{suffix}", source)
                before = source.read_bytes()
                timestamp = source.stat().st_mtime_ns
                target = source.with_suffix(f".kemi_{suffix}")
                result = self.cli(kind, "upgrade", "--force", source)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue(target.read_bytes().startswith(root))
                self.assertEqual(source.read_bytes(), before)
                self.assertEqual(source.stat().st_mtime_ns, timestamp)

                sentinel = target.read_bytes()
                result = self.cli(kind, "upgrade", "--force", source)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(target.read_bytes(), sentinel)
                self.assertEqual(source.read_bytes(), before)
                self.assertEqual(source.stat().st_mtime_ns, timestamp)


if __name__ == "__main__":
    unittest.main()
