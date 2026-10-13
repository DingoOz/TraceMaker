#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixture-independent identity and failed-publication regressions; no KiCad needed."""
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'bench'))
import prepare_dac2020 as dac
import prepare_demos as demos
import prepare_inputs as prep

ZERO = str(uuid.UUID(int=0))
VALID = 'aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa'
DUP = 'bbbbbbbb-2222-4222-8222-bbbbbbbbbbbb'


def ids(text):
    return [prep.parse(text[s:e])[1] for s, e, _, head in prep.expressions(text) if head == 'uuid']


def canonical(text, original):
    return prep.canonicalize_item_order(prep.repair_design_ids(
        prep.canonicalize_generated_ids(text, original, 'synthetic'), 'synthetic'))


def converted(reverse=False):
    """Model a converter's UUID-dependent save order, not a native API echo."""
    drawings = [f'(fp_line (start {x} 0) (end {x} 1) (uuid "{uuid.uuid4()}"))' for x in (1, 2)]
    pads = [f'(pad "{x}" smd rect (at {x} 0) (uuid "{uuid.uuid4()}"))' for x in (1, 2)]
    zones = [f'(zone (net {x}) (uuid "{uuid.uuid4()}"))' for x in (1, 2)]
    if reverse:
        drawings.reverse()
        pads.reverse()
        zones.reverse()
    footprint = f'(footprint "test" (uuid "{uuid.uuid4()}") {" ".join(drawings + pads)})'
    return f'(kicad_pcb (gr_text "preserve" (uuid "{VALID}")) {footprint} {" ".join(zones)})'


class FixturePreparation(unittest.TestCase):
    def test_generated_identity_and_nested_order_do_not_leak_into_geometry_input(self):
        original = f'(kicad_pcb (gr_text "preserve" (tstamp "{VALID}")) (module "test"))'
        left, right = canonical(converted(), original), canonical(converted(True), original)
        self.assertEqual(left, right)
        self.assertIn(VALID, ids(left))
        self.assertEqual(len(ids(left)), len(set(ids(left))))
        self.assertNotIn(ZERO, ids(left))

    def test_zero_duplicate_and_missing_zone_ids_preserve_unique_native_ids(self):
        original = f'''(kicad_pcb
          (zone (net 1) (tstamp "{VALID}"))
          (zone (net 2) (tstamp "{ZERO}"))
          (zone (net 3) (tstamp "{DUP}"))
          (zone (net 4) (tstamp "{DUP}"))
          (zone (net 5)))'''
        saved = original.replace('tstamp', 'uuid')
        result = canonical(saved, original)
        values = ids(result)
        self.assertEqual(len(values), 5)
        self.assertEqual(len(set(values)), 5)
        self.assertIn(VALID, values)
        self.assertNotIn(ZERO, values)
        self.assertNotIn(DUP, values)
        self.assertEqual(result, canonical(saved, original))
        for net in range(1, 6):
            self.assertIn(f'(net {net})', result)

    def test_post_save_duplicate_repair_is_idempotent(self):
        text = f'(kicad_pcb (dimension (uuid "{VALID}") (gr_text "5" (uuid "{VALID}"))))'
        repaired = prep.repair_design_ids(text, 'synthetic')
        self.assertEqual(ids(repaired)[0], VALID)
        self.assertEqual(len(set(ids(repaired))), 2)
        self.assertEqual(repaired, prep.repair_design_ids(repaired, 'synthetic'))

    def test_legacy_path_components_are_stable_without_changing_native_paths(self):
        text = f'(kicad_pcb (module "test" (path "/1234ABCD/{VALID}/non-native")))'
        repaired = prep.stabilize_legacy_paths(text, 'synthetic')
        self.assertIn(f'/1234ABCD/{VALID}/', repaired)
        self.assertNotIn('non-native', repaired)
        self.assertEqual(repaired, prep.stabilize_legacy_paths(text, 'synthetic'))
        self.assertEqual(repaired, prep.stabilize_legacy_paths(repaired, 'synthetic'))

    def test_strip_policy_keeps_zone_geometry_and_removes_all_top_level_routing(self):
        text = '(kicad_pcb (footprint "x" (arc (start 0 0)))\n (segment (start 0 0))\n (arc (start 0 0))\n (via (at 1 1))\n (zone (net 1) (filled_polygon (pts (xy 0 0)))))'
        stripped = dac.strip_routing(text)
        self.assertIn('(footprint "x" (arc (start 0 0)))', stripped)
        self.assertIn('(zone (net 1) (filled_polygon (pts (xy 0 0))))', stripped)
        self.assertNotIn('(segment', stripped)
        self.assertNotIn('(via', stripped)

    def test_failed_conversion_removes_stale_runnable_artifacts_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            source = base / 'source.kicad_pcb'
            source.write_bytes(b'(kicad_pcb)\n')
            source.with_suffix('.kicad_pro').write_bytes(b'{"source": true}\n')
            destination = base / 'fixture/unrouted.kicad_pcb'
            destination.parent.mkdir()
            artifacts = [destination, destination.with_suffix('.kicad_pro'), destination.with_suffix('.kicad_dru'),
                         destination.parent / 'preparation.drc.json']
            for path in artifacts:
                path.write_bytes(b'stale-success')
            raw = destination.parent / 'raw.kicad_pcb'
            raw.write_bytes(b'human-original')
            unrelated = destination.parent / 'notes.txt'
            unrelated.write_bytes(b'user-owned')
            with patch.object(prep, 'native_tools', return_value=('unused', 'unused')), \
                 patch.object(prep, 'convert_board', side_effect=RuntimeError('conversion failed')):
                with self.assertRaisesRegex(RuntimeError, 'conversion failed'):
                    prep.prepare_board(source, destination, dac.strip_routing)
            self.assertTrue(all(not path.exists() for path in artifacts))
            self.assertEqual(source.read_bytes(), b'(kicad_pcb)\n')
            self.assertEqual(source.with_suffix('.kicad_pro').read_bytes(), b'{"source": true}\n')
            self.assertEqual(raw.read_bytes(), b'human-original')
            self.assertEqual(unrelated.read_bytes(), b'user-owned')
            self.assertEqual(list(destination.parent.glob('.prepare-*')), [])

    def test_missing_demo_source_invalidates_an_earlier_generated_board(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp)
            board = out / 'missing/unrouted.kicad_pcb'
            board.parent.mkdir()
            board.write_bytes(b'stale')
            with patch.object(demos, 'SEARCH', []):
                with self.assertRaises(SystemExit):
                    demos.prepare('missing', out)
            self.assertFalse(board.exists())

    def test_source_cannot_be_its_own_destination(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp) / 'unrouted.kicad_pcb'
            source.write_bytes(b'source-owned')
            with self.assertRaises(ValueError):
                prep.prepare_board(source, source, dac.strip_routing)
            self.assertEqual(source.read_bytes(), b'source-owned')


if __name__ == '__main__':
    unittest.main()
