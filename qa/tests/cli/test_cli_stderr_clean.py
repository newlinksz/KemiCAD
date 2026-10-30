#
# This program source code file is part of KiCad, a free EDA CAD application.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#

"""Regression: kicad-cli must not log a bogus mkdir of '' on exit.

The default (unnamed) project used to get an extension-only local settings name, so
SETTINGS_MANAGER::Save on exit tried to create directory '' and wx logged
"Directory '' couldn't be created" to stderr on every command.
"""

import utils
import pytest

from conftest import KiTestFixture


BAD_MESSAGES = ( "couldn't be created", "Directory ''", "Fontconfig error", "schema file" )


def _assert_clean( stderr: str ) -> None:
    for message in BAD_MESSAGES:
        assert message not in stderr, stderr


def test_version_stderr_clean():
    stdout, stderr, exitcode = utils.run_and_capture( [ utils.kicad_cli(), "version" ] )

    assert exitcode == 0
    _assert_clean( stderr )


@pytest.mark.parametrize( "args,output_name",
                          [ ( [ "sch", "export", "netlist", "--format", "kicadxml" ], "stderr_clean.xml" ),
                            ( [ "sch", "erc", "--format", "json" ], "stderr_clean.erc.json" ),
                            ( [ "pcb", "drc", "--format", "json" ], "stderr_clean.drc.json" ),
                            ( [ "pcb", "export", "gerbers" ], "stderr_clean_gerbers/" ) ] )
def test_common_exports_stderr_clean( kitest: KiTestFixture, args, output_name ):
    source = "cli/basic_test/basic_test.kicad_pcb" if args[0] == "pcb" \
             else "cli/basic_test/basic_test.kicad_sch"
    input_file = kitest.get_data_file_path( source )
    output_path = kitest.get_output_path( "cli/stderr_clean/" ).joinpath( output_name )

    if output_name.endswith( "/" ):
        output_path.mkdir( parents=True, exist_ok=True )

    stdout, stderr, exitcode = utils.run_and_capture(
            [ utils.kicad_cli(), *args, "-o", str( output_path ), str( input_file ) ] )

    # ERC/DRC report violations through the exit code only with --exit-code-violations
    assert exitcode == 0, stderr
    _assert_clean( stderr )


@pytest.mark.parametrize( "runs", [ 1, 2 ] )
def test_version_stderr_clean_fresh_config( tmp_path, monkeypatch, runs ):
    # A first run on an empty config dir (the state right after install) is what showed the
    # message to users; a reused developer config can hide it, so always start from scratch.
    monkeypatch.setenv( "KICAD_CONFIG_HOME", str( tmp_path / "config" ) )

    for _ in range( runs ):
        stdout, stderr, exitcode = utils.run_and_capture( [ utils.kicad_cli(), "version" ] )

        assert exitcode == 0
        _assert_clean( stderr )
