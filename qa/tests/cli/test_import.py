#
# This program source code file is part of KiCad, a free EDA CAD application.
#
# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
#  MA 02110-1301, USA.
#

"""
Tests for the top-level kicad-cli import command
"""

import utils
from pathlib import Path
import pytest
import json
import os
import re
import shutil
import uuid
import xml.etree.ElementTree as ET
import zipfile
from conftest import KiTestFixture


def get_project_stem( kitest: KiTestFixture, test_name: str, stem: str ) -> Path:
    """Keep every run in a fresh output directory so stale artifacts cannot hide failures."""
    output_dir = kitest.get_output_path( f"cli/import/{test_name}/{uuid.uuid4().hex}/" )
    return output_dir / stem


def assert_kemi_project( stem: Path ):
    """Validate the native containers and reject official artifacts, including temporary names."""
    json_types = { ".kemi_pro": "project", ".kemi_prl": "project_local",
                   ".kemi_jobset": "jobset" }
    sexpr_roots = { ".kemi_sch": "kemi_sch", ".kemi_pcb": "kemi_pcb",
                    ".kemi_sym": "kemi_symbol_lib", ".kemi_mod": "kemi_footprint",
                    ".kemi_wks": "kemi_wks", ".kemi_dru": "kemi_dru" }
    assert Path( f"{stem}.kemi_pro" ).is_file()
    assert Path( f"{stem}.kemi_prl" ).is_file()

    for artifact in stem.parent.rglob( "*" ):
        assert ".kicad_" not in artifact.name.lower(), artifact

        if not artifact.is_file() or not artifact.suffix.startswith( ".kemi_" ):
            continue

        text = artifact.read_text( encoding="utf-8" )

        if artifact.suffix in json_types:
            marker = json.loads( text )["kemicad"]
            assert marker["document_type"] == json_types[artifact.suffix]
            assert marker["format_version"] == 1
        else:
            assert text.lstrip().startswith( f"({sexpr_roots[artifact.suffix]}" ), artifact
            assert re.search( r"\(format_version\s+1\)", text ), artifact

        if artifact.suffix == ".kemi_mod":
            assert artifact.parent.suffix == ".pretty"


def snapshot_tree( root: Path ) -> dict:
    """Include directory creation and bytes, so rejected output cannot leave partial work."""
    return { path.relative_to( root ).as_posix(): path.read_bytes() if path.is_file() else None
             for path in root.rglob( "*" ) }


def get_pads_test_file() -> str:
    """Get path to a PADS board test file from the QA test data directory."""
    pads_file = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "pcbnew", "plugins",
                              "pads", "ClaySight_MK1", "ClaySight_MK1.asc" )

    return pads_file if os.path.exists( pads_file ) else None


def get_pads_multisheet_test_file() -> str:
    """Get a hierarchical PADS schematic for flat-output naming regressions."""
    schematic = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "eeschema",
                              "plugins", "pads", "binary", "multisheet_connectivity.txt" )

    return schematic if os.path.exists( schematic ) else None


def get_pcad_test_files() -> tuple:
    """Paths to the P-CAD schematic and board test files, or (None, None)."""
    sch = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "eeschema", "io",
                        "pcad", "pcad_feature_test.sch" )
    pcb = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "pcbnew", "plugins",
                        "pcad", "pcad_4layer_glyph_test_ascii.PCB" )

    if os.path.exists( sch ) and os.path.exists( pcb ):
        return sch, pcb

    return None, None


def get_eagle_test_file() -> str:
    """Get path to an Eagle schematic test file from the QA test data directory."""
    eagle_file = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "eeschema", "io",
                               "eagle", "eagle-import-testfile.sch" )

    return eagle_file if os.path.exists( eagle_file ) else None


def get_easyedapro_v3_test_file() -> str:
    """Get an EasyEDA Pro v3 project fixture with multiple boards."""
    archive = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "pcbnew", "plugins",
                            "easyedapro", "ProProject_LS2K0300Core_2025-11-14.epro2" )

    return archive if os.path.exists( archive ) else None


def get_easyedapro_v2_test_file() -> str:
    """Get an EasyEDA Pro v2 project fixture with multiple boards."""
    archive = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "pcbnew", "plugins",
                            "easyedapro", "ProProject_Yuzuki Chameleon_2023-09-02.epro" )

    return archive if os.path.exists( archive ) else None


def get_easyedapro_v2_single_board_test_file() -> str:
    """Get an EasyEDA Pro v2 project fixture with one board."""
    archive = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "pcbnew", "plugins",
                            "easyedapro", "Scanning Tunneling Microscope OpenSTM ControlBoard.zip" )

    return archive if os.path.exists( archive ) else None


class TestImportHelp:
    """Test help output for the top-level import command"""

    def test_import_help( self, kitest: KiTestFixture ):
        """Test that import --help shows usage information and no --format flag"""
        command = [ utils.kicad_cli(), "import", "--help" ]

        stdout, _, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert "INPUT_FILES" in stdout
        assert "--format" not in stdout
        assert "--layer-map" in stdout


class TestImportErrors:
    """Test error handling for the top-level import command"""

    def test_import_unsupported_file( self, kitest: KiTestFixture ):
        """Test that a file recognized by neither face fails cleanly"""
        stem = get_project_stem( kitest, "errors", "unsupported" )

        unsupported = stem.with_suffix( ".xyz" )
        unsupported.parent.mkdir( parents=True, exist_ok=True )
        unsupported.write_text( "not a real design file\n", encoding="utf-8" )

        command = [ utils.kicad_cli(), "import", str( unsupported ), "-o", str( stem ) ]

        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code != 0
        assert not stem.with_suffix( ".kemi_pro" ).exists()


class TestImportFootprintLink:
    """IFL01: The paired import must persist the board's real FPID before archiving."""

    @pytest.mark.parametrize( "board_first", [ False, True ] )
    def test_paired_import_persists_real_footprint_ids( self, kitest: KiTestFixture,
                                                      board_first: bool ):
        source = kitest.get_data_file_path( "cli/import/paired-footprint" )
        source_before = snapshot_tree( source )
        stem = get_project_stem( kitest, "paired_footprint_link", "paired" )
        inputs = [ source / "pair.txt", source / "pair.asc" ]

        if board_first:
            inputs.reverse()

        stdout, stderr, return_code = utils.run_and_capture(
            [ utils.kicad_cli(), "import", *inputs, "-o", stem ] )
        assert return_code == 0, ( stdout, stderr )
        assert_kemi_project( stem )
        board = stem.with_suffix( ".kemi_pcb" ).read_text( encoding="utf-8" )
        expected_id = "paired:PAIR_TWO_PAD"
        assert f'(footprint "{expected_id}"' in board
        assert ( stem.parent / "paired.pretty" / "PAIR_TWO_PAD.kemi_mod" ).is_file()

        netlist = stem.with_suffix( ".xml" )
        stdout, stderr, return_code = utils.run_and_capture(
            [ utils.kicad_cli(), "sch", "export", "netlist", "--format", "kicadxml",
              "-o", netlist, stem.with_suffix( ".kemi_sch" ) ] )
        assert return_code == 0, ( stdout, stderr )
        components = ET.parse( netlist ).getroot().findall( "components/comp" )
        assert { item.attrib["ref"]: item.findtext( "footprint", "" ) for item in components } == {
            "R1": expected_id, "R2": expected_id
        }

        schematic = stem.with_suffix( ".kemi_sch" )
        archive_path = stem.parent.parent / "paired.zip"
        assert archive_path.is_file()

        with zipfile.ZipFile( archive_path ) as archive:
            matches = [ name for name in archive.namelist() if name.endswith( schematic.name ) ]
            assert len( matches ) == 1
            assert archive.read( matches[0] ) == schematic.read_bytes()

        assert snapshot_tree( source ) == source_before


@pytest.mark.skipif( get_pads_test_file() is None,
                     reason="PADS test files not available" )
class TestKemiProjectImport:
    """PG05: An explicit official output is rejected before creating design artifacts."""

    @pytest.mark.parametrize( "output_suffix", [
        ".kicad_pro", ".kicad_prl", ".kicad_sch", ".kicad_pcb", ".kicad_sym",
        ".kicad_mod", ".kicad_dru", ".kicad_wks", ".kicad_jobset",
        ".kicad_pro.tmp", ".KICAD_PCB.tmp"
    ] )
    def test_rejects_official_output_without_writes( self, kitest: KiTestFixture,
                                                    output_suffix: str ):
        root = get_project_stem( kitest, "reject_official", "unused" ).parent
        source = root / "source.asc"
        shutil.copyfile( get_pads_test_file(), source )
        output = root / "must_not_be_created" / f"result{output_suffix}"
        before = snapshot_tree( root )

        command = [ utils.kicad_cli(), "import", str( source ), "-o", str( output ) ]
        stdout, stderr, return_code = utils.run_and_capture( command, cwd=root )

        assert return_code == 1, ( stdout, stderr )
        assert snapshot_tree( root ) == before


@pytest.mark.skipif( get_pads_test_file() is None,
                     reason="PADS test files not available" )
class TestImportBoardOnly:
    """Test importing a single board into a project"""

    def test_import_default_output_creates_kemi_project( self, kitest: KiTestFixture ):
        """PG06: Default output uses the working directory and leaves the source intact."""
        stem = get_project_stem( kitest, "default_output", "design" )
        source_directory = stem.parent / "source"
        source_directory.mkdir()
        source = source_directory / "design.asc"
        shutil.copyfile( get_pads_test_file(), source )
        before = snapshot_tree( source_directory )

        command = [ utils.kicad_cli(), "import", str( source ) ]
        stdout, stderr, return_code = utils.run_and_capture( command, cwd=stem.parent )

        assert return_code == 0, ( stdout, stderr )
        assert_kemi_project( stem )
        assert stem.with_suffix( ".kemi_pcb" ).is_file()
        assert not stem.with_suffix( ".kemi_sch" ).exists()
        assert snapshot_tree( source_directory ) == before

    def test_import_board_creates_project( self, kitest: KiTestFixture ):
        """A single board input yields a board-only project"""
        pads_file = get_pads_test_file()
        stem = get_project_stem( kitest, "board_only", "widget" )

        command = [ utils.kicad_cli(), "import", pads_file, "-o", str( stem ) ]

        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )
        assert stem.with_suffix( ".kemi_pro" ).exists()
        assert stem.with_suffix( ".kemi_pcb" ).exists()
        assert not stem.with_suffix( ".kemi_sch" ).exists()

        with open( stem.with_suffix( ".kemi_pcb" ), "r", encoding="utf-8" ) as f:
            assert "(kemi_pcb" in f.read( 100 )

        # Project file must be valid JSON
        with open( stem.with_suffix( ".kemi_pro" ), "r", encoding="utf-8" ) as f:
            json.load( f )

    def test_import_keeps_dots_in_stem( self, kitest: KiTestFixture ):
        """A dot in the output stem is part of the project name"""
        pads_file = get_pads_test_file()
        stem = get_project_stem( kitest, "dotted_stem", "widget.v2" )

        command = [ utils.kicad_cli(), "import", pads_file, "-o", str( stem ) ]

        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )
        assert Path( f"{stem}.kemi_pro" ).exists()
        assert Path( f"{stem}.kemi_pcb" ).exists()
        assert not stem.with_suffix( ".kemi_pro" ).exists()

    def test_import_board_with_layer_map( self, kitest: KiTestFixture ):
        """The top-level import accepts --layer-map and wires it through to the board job"""
        pads_file = get_pads_test_file()
        stem = get_project_stem( kitest, "layer_map", "widget" )

        map_path = stem.with_suffix( ".layermap.json" )
        map_path.parent.mkdir( parents=True, exist_ok=True )
        map_path.write_text( json.dumps( { "Bottom Layer": "B.Cu" } ), encoding="utf-8" )

        command = [ utils.kicad_cli(), "import", "--layer-map", str( map_path ),
                    pads_file, "-o", str( stem ) ]

        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )
        assert stem.with_suffix( ".kemi_pro" ).exists()
        assert stem.with_suffix( ".kemi_pcb" ).exists()



@pytest.mark.skipif( get_easyedapro_v3_test_file() is None,
                     reason="EasyEDA Pro v3 test file not available" )
class TestImportEasyEdaProProject:
    """Test importing every board in an EasyEDA Pro v3 project."""

    def test_import_all_project_boards( self, kitest: KiTestFixture ):
        """Each PCB document becomes a linked KiCad board."""
        archive = get_easyedapro_v3_test_file()
        stem = get_project_stem( kitest, "easyedapro", "project" )
        first_board = stem.parent / "PCB1.kemi_pcb"
        second_board = stem.parent / "PCB1_1.kemi_pcb"

        for board in ( first_board, second_board ):
            if board.exists():
                board.unlink()

        command = [ utils.kicad_cli(), "import", archive, "-o", str( stem ) ]
        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )
        assert first_board.exists()
        assert ( stem.parent / "PCB1.pretty" ).is_dir()
        assert ( stem.parent / "PCB1_1.pretty" ).is_dir()
        assert second_board.exists()

        with open( stem.with_suffix( ".kemi_pro" ), "r", encoding="utf-8" ) as f:
            project = json.load( f )

        assert sorted( filename for _, filename in project.get( "boards", [] ) ) == [
            "PCB1.kemi_pcb",
            "PCB1_1.kemi_pcb"
        ]


@pytest.mark.skipif( get_easyedapro_v2_test_file() is None,
                     reason="EasyEDA Pro v2 test file not available" )
class TestImportEasyEdaProV2Project:
    """Test importing every board in an EasyEDA Pro v2 project."""

    def test_import_all_project_boards( self, kitest: KiTestFixture ):
        """Each PCB document becomes a source-named, linked KiCad board."""
        archive = get_easyedapro_v2_test_file()
        stem = get_project_stem( kitest, "easyedapro_v2", "project" )

        expected_boards = [
            "PCB.kemi_pcb",
            "树莓派A版形状.kemi_pcb",
            "点位图.kemi_pcb",
            "100ASK.kemi_pcb",
            "散热器外形.kemi_pcb"
        ]

        for board in ( stem.parent / name for name in expected_boards ):
            if board.exists():
                board.unlink()

        command = [ utils.kicad_cli(), "import", archive, "-o", str( stem ) ]
        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )

        for name in expected_boards:
            assert ( stem.parent / name ).exists()

        with open( stem.with_suffix( ".kemi_pro" ), "r", encoding="utf-8" ) as f:
            project = json.load( f )

        assert sorted( filename for _, filename in project.get( "boards", [] ) ) == sorted( expected_boards )

        board_text = ( stem.parent / "PCB.kemi_pcb" ).read_text( encoding="utf-8" )
        zone_settings = re.findall(
            r"\(connect_pads\s+\(clearance ([0-9.]+)\)\s+\)\s+\(min_thickness ([0-9.]+)\)",
            board_text,
        )

        copper_zone_settings = [ ( clearance, thickness ) for clearance, thickness in zone_settings
                                  if clearance != "0" ]

        assert copper_zone_settings
        assert { clearance for clearance, _ in copper_zone_settings } == { "0.089" }
        assert { thickness for _, thickness in copper_zone_settings } == { "0.25" }
        assert project["net_settings"]["classes"][0]["clearance"] == 0.2

        rules = ( stem.parent / "PCB.kemi_dru" ).read_text( encoding="utf-8" )
        assert "# Board Outline ↔ Track: 11.8 mil" in rules
        assert "# Copper / Plane Zone ↔ Track: 11.8 mil" not in rules

    @pytest.mark.skipif( get_easyedapro_v2_single_board_test_file() is None,
                         reason="Single-board EasyEDA Pro v2 test file not available" )
    def test_import_single_project_board_uses_project_name( self, kitest: KiTestFixture ):
        """A one-board project uses the requested project board filename."""
        archive = get_easyedapro_v2_single_board_test_file()
        stem = get_project_stem( kitest, "easyedapro_v2_single", "project" )
        source_named_board = stem.parent / "ControlBoard.kemi_pcb"

        if source_named_board.exists():
            source_named_board.unlink()

        command = [ utils.kicad_cli(), "import", archive, "-o", str( stem ) ]
        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )
        assert stem.with_suffix( ".kemi_pcb" ).exists()
        assert not source_named_board.exists()

        with open( stem.with_suffix( ".kemi_pro" ), "r", encoding="utf-8" ) as f:
            project = json.load( f )

        assert [ filename for _, filename in project.get( "boards", [] ) ] == [
            stem.with_suffix( ".kemi_pcb" ).name
        ]


@pytest.mark.skipif( get_eagle_test_file() is None,
                     reason="Eagle schematic test file not available" )
class TestImportSchematicOnly:
    """Test importing a single schematic into a project"""

    def test_import_schematic_classification( self, kitest: KiTestFixture ):
        """A schematic input is autodetected and yields a schematic-only project"""
        eagle_file = get_eagle_test_file()
        stem = get_project_stem( kitest, "sch_only", "design" )

        command = [ utils.kicad_cli(), "import", eagle_file, "-o", str( stem ) ]

        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )
        assert stem.with_suffix( ".kemi_pro" ).exists()
        assert stem.with_suffix( ".kemi_sch" ).exists()
        assert not stem.with_suffix( ".kemi_pcb" ).exists()

        with open( stem.with_suffix( ".kemi_sch" ), "r", encoding="utf-8" ) as f:
            assert "(kemi_sch" in f.read( 100 )


@pytest.mark.skipif( get_pads_test_file() is None or get_eagle_test_file() is None,
                     reason="Both PADS board and Eagle schematic test files are required" )
class TestImportLinkedProject:
    """Test importing a board and a schematic into one linked project"""

    def test_import_dual_input_linked( self, kitest: KiTestFixture ):
        """Board + schematic inputs yield a single linked project sharing a stem"""
        pads_file = get_pads_test_file()
        eagle_file = get_eagle_test_file()
        stem = get_project_stem( kitest, "linked", "board" )

        command = [ utils.kicad_cli(), "import", pads_file, eagle_file, "-o", str( stem ) ]

        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )
        assert stem.with_suffix( ".kemi_pro" ).exists()
        assert stem.with_suffix( ".kemi_pcb" ).exists()
        assert stem.with_suffix( ".kemi_sch" ).exists()

        with open( stem.with_suffix( ".kemi_pro" ), "r", encoding="utf-8" ) as f:
            project = json.load( f )

        # The project should reference the produced board and sheet.
        assert project.get( "boards" ), "project boards[] should list the imported board"
        assert project.get( "sheets" ), "project sheets[] should list the imported schematic"


@pytest.mark.skipif( get_pads_test_file() is None or get_pads_multisheet_test_file() is None,
                     reason="PADS schematic and board test files are required" )
class TestImportNaming:
    """Fresh imports use the output project stem for both libraries and keep sheets beside it."""

    @pytest.mark.parametrize( "requested_name, project_name", [
        ( "demo-board-V7-20241210-1210", "demo-board-V7" ),
        ( "renamed-V7", "renamed-V7" )
    ] )
    def test_dated_source_and_renamed_output_share_flat_project_names(
            self, kitest: KiTestFixture, requested_name: str, project_name: str ):
        root = get_project_stem( kitest, "project_names", "unused" ).parent
        source_directory = root / "source"
        source_directory.mkdir()
        source_name = "demo-board-V7-20241210-1210"
        schematic = source_directory / f"{source_name}.txt"
        board = source_directory / f"{source_name}.asc"
        shutil.copyfile( get_pads_multisheet_test_file(), schematic )
        shutil.copyfile( get_pads_test_file(), board )
        source_before = snapshot_tree( source_directory )
        output_directory = root / "kemi"
        requested = output_directory / requested_name
        stem = output_directory / project_name

        command = [ utils.kicad_cli(), "import", str( schematic ), str( board ),
                    "-o", str( requested ) ]
        stdout, stderr, return_code = utils.run_and_capture( command, cwd=root )

        assert return_code == 0, ( stdout, stderr )
        assert_kemi_project( stem )
        assert stem.with_suffix( ".kemi_sch" ).is_file()
        assert stem.with_suffix( ".kemi_pcb" ).is_file()
        assert sorted( path.name for path in output_directory.glob( "*.kemi_sym" ) ) == [
            f"{project_name}.kemi_sym"
        ]
        assert sorted( path.name for path in output_directory.glob( "*.pretty" ) ) == [
            f"{project_name}.pretty"
        ]

        for table_name, library in ( ( "sym-lib-table", f"{project_name}.kemi_sym" ),
                                     ( "fp-lib-table", f"{project_name}.pretty" ) ):
            table = ( output_directory / table_name ).read_text( encoding="utf-8" )
            assert re.search( rf'\(name\s+"?{re.escape( project_name )}"?\)', table ), table
            assert f"${{KIPRJMOD}}/{library}" in table
            assert "-import-fps" not in table and "-import-syms" not in table
            assert "-20241210-1210" not in table

        schematics = list( output_directory.rglob( "*.kemi_sch" ) )
        assert len( schematics ) == 3
        assert all( path.parent == output_directory for path in schematics )
        assert not ( output_directory / "kemi" ).exists()
        project = json.loads( stem.with_suffix( ".kemi_pro" ).read_text( encoding="utf-8" ) )
        assert len( project["sheets"] ) == len( schematics )
        top_level_sheets = project["schematic"]["top_level_sheets"]
        assert len( top_level_sheets ) == 1
        assert { sheet["filename"] for sheet in top_level_sheets } == {
            stem.with_suffix( ".kemi_sch" ).name
        }

        for sheet in top_level_sheets:
            assert "/" not in sheet["filename"] and "\\" not in sheet["filename"]
            assert ( output_directory / sheet["filename"] ).is_file()

        assert snapshot_tree( source_directory ) == source_before


@pytest.mark.skipif( get_pcad_test_files() == ( None, None ),
                     reason="P-CAD test files not available" )
class TestImportPcadProject:
    """Test importing a P-CAD schematic and board pair"""

    def test_import_pcad_pair_linked( self, kitest: KiTestFixture ):
        """P-CAD schematic + board inputs are content-detected and linked into one project"""
        sch_file, pcb_file = get_pcad_test_files()
        stem = get_project_stem( kitest, "pcad_pair", "design" )

        command = [ utils.kicad_cli(), "import", sch_file, pcb_file, "-o", str( stem ) ]

        stdout, stderr, return_code = utils.run_and_capture( command )

        assert return_code == 0
        assert_kemi_project( stem )
        assert stem.with_suffix( ".kemi_pro" ).exists()
        assert stem.with_suffix( ".kemi_sch" ).exists()
        assert stem.with_suffix( ".kemi_pcb" ).exists()
