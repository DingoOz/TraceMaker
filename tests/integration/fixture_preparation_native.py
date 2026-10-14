#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Own synthetic legacy board: native zero-zone-ID control and prepare-twice.

Run from the repository: python3 tests/integration/fixture_preparation_native.py
Needs version-matched KiCad CLI/pcbnew; unavailable tools exit 77. Reports are
validated JSON, not compared byte for byte. No downloaded boards are needed.
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import uuid
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'bench'))
import prepare_dac2020 as dac
import prepare_demos as demos
import prepare_inputs as prep

# Generated here, not copied from a benchmark. Legacy zero timestamps reproduce
# the KiCad loader's shared all-zero zone IDs before any refill has taken place.
BOARD = '''(kicad_pcb (version 20171130) (host pcbnew "5.1.0")
 (general (thickness 1.6)) (page A4)
 (layers (0 F.Cu signal) (31 B.Cu signal) (36 B.SilkS user) (37 F.SilkS user)
  (38 B.Mask user) (39 F.Mask user) (44 Edge.Cuts user) (49 F.Fab user))
 (setup (last_trace_width 0.25) (trace_clearance 0.2) (zone_clearance 0.2)
  (zone_45_only no) (trace_min 0.1) (via_size 0.6) (via_drill 0.3)
  (via_min_size 0.4) (via_min_drill 0.2))
 (net 0 "") (net 1 "GND")
 (net_class Default "" (clearance 0.2) (trace_width 0.25)
  (via_dia 0.6) (via_drill 0.3) (uvia_dia 0.3) (uvia_drill 0.1) (add_net "GND"))
 (module "synthetic" (layer F.Cu) (tstamp 12345678) (at 5 5)
  (fp_text reference U1 (at 0 -2) (layer F.SilkS) (effects (font (size 1 1) (thickness 0.15))))
  (fp_text value "synthetic" (at 0 2) (layer F.Fab) (effects (font (size 1 1) (thickness 0.15))))
  (pad 1 thru_hole circle (at 0 0) (size 2 2) (drill 1) (layers *.Cu *.Mask) (net 1 "GND")))
 (gr_line (start 0 0) (end 10 0) (layer Edge.Cuts) (width 0.05))
 (gr_line (start 10 0) (end 10 10) (layer Edge.Cuts) (width 0.05))
 (gr_line (start 10 10) (end 0 10) (layer Edge.Cuts) (width 0.05))
 (gr_line (start 0 10) (end 0 0) (layer Edge.Cuts) (width 0.05))
 (segment (start 5 5) (end 6 5) (width 0.25) (layer F.Cu) (net 1))
 (via (at 6 5) (size 0.6) (drill 0.3) (layers F.Cu B.Cu) (net 1))
 (zone (net 1) (net_name "GND") (layer B.Cu) (tstamp 00000000) (hatch edge 0.508)
  (connect_pads (clearance 0.2)) (min_thickness 0.25)
  (fill yes (thermal_gap 0.3) (thermal_bridge_width 0.3))
  (polygon (pts (xy 1 1) (xy 9 1) (xy 9 9) (xy 1 9))))
 (zone (net 0) (net_name "") (layer F.Cu) (tstamp 00000000) (hatch edge 0.508)
  (connect_pads (clearance 0.2)) (min_thickness 0.25)
  (keepout (tracks not_allowed) (vias allowed) (copperpour not_allowed))
  (fill (thermal_gap 0.3) (thermal_bridge_width 0.3))
  (polygon (pts (xy 1 1) (xy 2 1) (xy 2 2) (xy 1 2))))
)\n'''


def main():
    cli, interpreter = prep.native_tools()
    try:
        version = subprocess.run([cli, 'version'], capture_output=True, text=True, timeout=30)
        bindings = subprocess.run([interpreter, '-c', 'import pcbnew'], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as error:
        print(f'KiCad unavailable: {error}')
        return 77
    if version.returncode or bindings.returncode:
        print(f'KiCad CLI/pcbnew unavailable: {version.stderr} {bindings.stderr}')
        return 77
    work_root = ROOT / 'build/fixture-preparation-native'
    work_root.mkdir(parents=True, exist_ok=True)  # Docker wrappers mount the checkout, not host /tmp.
    with tempfile.TemporaryDirectory(prefix='fixture-native-', dir=work_root) as tmp:
        base = pathlib.Path(tmp)
        source = base / 'sources/synthetic/synthetic.kicad_pcb'
        source.parent.mkdir(parents=True)
        source.write_text(BOARD, encoding='utf-8')
        source.with_suffix('.kicad_pro').write_bytes(b'{}\n')
        source.with_suffix('.kicad_dru').write_bytes(b'(version 1)\n(rule "synthetic" (constraint clearance (min 0.1mm)))\n')
        source_bytes = {p.name: p.read_bytes() for p in source.parent.iterdir()}
        control = base / 'control.kicad_pcb'
        control.write_text(dac.strip_routing(BOARD), encoding='utf-8')
        prep.convert_board(control, cli, interpreter, 120)
        converted = prep.parse(control.read_text(encoding='utf-8'))
        zones = [node for node in converted if isinstance(node, list) and node[0] == 'zone']
        zero_zones = [node for node in zones if prep.child(node, 'uuid') is None
                      or uuid.UUID(prep.child(node, 'uuid')[1]).int == 0]
        assert len(zones) == 2, 'native control lost a design zone'
        print(f'Native conversion control: {len(zero_zones)}/{len(zones)} missing/zero zone identities')
        # Controls diagnose the upstream loader; newer KiCad may repair these
        # itself. The prepared fixture must be unique regardless of that result.
        demos.SEARCH = [base / 'sources']
        left = demos.prepare('synthetic', base / 'left')
        right = demos.prepare('synthetic', base / 'right')
        for ext in ('.kicad_pcb', '.kicad_pro', '.kicad_dru'):
            assert left.with_suffix(ext).read_bytes() == right.with_suffix(ext).read_bytes(), ext + ' differs'
        saved = prep.parse(left.read_text(encoding='utf-8'))
        saved_zones = [node for node in saved if isinstance(node, list) and node[0] == 'zone']
        assert len(saved_zones) == 2, 'prepared fixture lost a design zone'
        values = [uuid.UUID(prep.parse(text[s:e])[1]) for text in [left.read_text(encoding='utf-8')]
                  for s, e, _, head in prep.expressions(text) if head == 'uuid']
        assert all(value.int for value in values), 'prepared fixture contains zero IDs'
        assert len(values) == len(set(values)), 'prepared fixture contains duplicate IDs'
        assert not any(isinstance(node, list) and node[0] in ('segment', 'arc', 'via') for node in saved)
        assert any(head == 'filled_polygon' for _, _, _, head in prep.expressions(left.read_text(encoding='utf-8'))), 'native refill did not produce copper'
        for board in (left, right):
            assert isinstance(json.loads((board.parent / 'preparation.drc.json').read_text()), dict)
        assert {p.name: p.read_bytes() for p in source.parent.iterdir()} == source_bytes, 'source/rules mutated'
        print('Prepared board/project/custom-rule bytes identical; source bytes unchanged; IDs unique and nonzero')
        replace = prep.os.replace

        def fail_board_publication(src, dst):
            if pathlib.Path(dst) == left:
                raise OSError('injected board publication failure')
            return replace(src, dst)

        with patch.object(prep.os, 'replace', side_effect=fail_board_publication):
            try:
                demos.prepare('synthetic', base / 'left')
            except OSError as error:
                assert 'injected board publication failure' in str(error)
            else:
                raise AssertionError('publication failure was not propagated')
        assert not left.exists(), 'failed publication left a runnable board'
        assert not left.with_suffix('.kicad_pro').exists()
        assert not left.with_suffix('.kicad_dru').exists()
        assert not (left.parent / 'preparation.drc.json').exists()
        assert {p.name: p.read_bytes() for p in source.parent.iterdir()} == source_bytes
        source.with_suffix('.kicad_dru').unlink()
        demos.prepare('synthetic', base / 'right')
        assert not right.with_suffix('.kicad_dru').exists(), 'reused a stale custom-rule file'
        assert right.with_suffix('.kicad_pro').read_bytes() == source.with_suffix('.kicad_pro').read_bytes()

        # Exercise the DAC wrapper too, including its byte-exact human/DSN publications.
        dac.SRC, dac.DSN, dac.OUT = base / 'dac', base / 'dsn', base / 'dac-prepared'
        pcb = dac.SRC / 'bm1/synthetic.kicad_pcb'
        pcb.parent.mkdir(parents=True)
        pcb.write_text(BOARD, encoding='utf-8')
        dac.DSN.mkdir()
        dsn = dac.DSN / 'DAC2020_bm01.dsn'
        dsn.write_bytes(b'(pcb synthetic)\n')
        dac.main()
        result = dac.OUT / 'bm1/unrouted.kicad_pcb'
        first = result.read_bytes()
        dac.main()
        assert result.read_bytes() == first, 'DAC preparation is not repeatable'
        assert (result.parent / 'raw.kicad_pcb').read_bytes() == pcb.read_bytes()
        assert (result.parent / 'unrouted.dsn').read_bytes() == dsn.read_bytes()
        print('Failed publication invalidated generated artifacts; absent rules not reused; DAC bytes repeatable')
    return 0


if __name__ == '__main__':
    sys.exit(main())
