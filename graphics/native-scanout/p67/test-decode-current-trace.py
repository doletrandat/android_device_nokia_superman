#!/usr/bin/env python3
"""Negative validation against actual P67 wrapper outputs, not hardware data."""
import copy
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

fixture = Path(sys.argv.pop(1))
spec = importlib.util.spec_from_file_location(
    'decoder', Path(__file__).with_name('decode-current-trace.py'))
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


class DecoderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.source = next(fixture.glob('*-current-*.bgra'))
        self.raw = Path(self.tmp.name) / self.source.name
        self.raw.write_bytes(self.source.read_bytes())
        self.meta = dict(line.split('=', 1) for line in
                         self.source.with_name(self.source.name + '.txt').read_text().splitlines())

    def tearDown(self):
        self.tmp.cleanup()

    def load(self, **changes):
        meta = dict(self.meta, **{k: str(v) for k, v in changes.items()})
        self.raw.with_name(self.raw.name + '.txt').write_text(
            ''.join(f'{k}={v}\n' for k, v in meta.items()))
        return decoder.load(self.raw)

    def test_current_and_actual_wrapper_stages(self):
        self.assertEqual(self.load()[0]['sample_origin'], 'before_commit')
        records = [decoder.load(p) for p in sorted(fixture.glob('*.bgra'))]
        self.assertEqual({m['stage'] for m, _ in records}, {'current', 'latched', 'retiring'})
        # Fixture intentionally changes byte0 after the current capture.
        self.assertTrue(any(v['changed_rows'] == [0] for v in decoder.compare(records)))

    def test_vblank_origin(self):
        self.assertEqual(self.load(sample_origin='real_vblank',
                                   sample_vblank_ns=self.meta['begin_ns'])[0]['stage'], 'current')
        for changes in ({'sample_origin': 'real_vblank', 'sample_vblank_ns': 0},
                        {'sample_origin': 'real_vblank',
                         'sample_vblank_ns': int(self.meta['begin_ns']) + 1},
                        {'sample_origin': 'before_commit', 'sample_vblank_ns': 1},
                        {'sample_origin': 'post_commit'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.load(**changes)

    def test_failed_or_absent_fences(self):
        for key, bad in (('acquire', 0), ('acquire', -62), ('acquire_fd', -1),
                         ('latch', 0), ('latch', -62), ('latch_fd', -1),
                         ('composition_latch', 0), ('composition_latch', -62),
                         ('composition_latch_fd', -1)):
            with self.subTest(key=key, bad=bad), self.assertRaises(ValueError):
                self.load(**{key: bad})

    def test_expired_or_inverted_time(self):
        for changes in ({'arm_begin_ns': 0},
                        {'arm_begin_ns': int(self.meta['begin_ns']) + 1},
                        {'arm_begin_ns': int(self.meta['begin_ns']) - 5000000001},
                        {'read_end_ns': int(self.meta['begin_ns']) - 1}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.load(**changes)

    def test_old_version_and_incomplete_files(self):
        for changes in ({'version': 'p66'}, {'complete': 0}, {'errno': 5},
                        {'stride': 2881}, {'bytes': 1}, {'fnv1a64': '0000000000000000'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.load(**changes)
        self.raw.write_bytes(self.raw.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            self.load()

    def test_no_comparison_across_frames_or_backings(self):
        first = self.load()
        for key in ('arm', 'pid', 'dev', 'ino', 'frame'):
            second = copy.deepcopy(first)
            second[0][key] = 'different' if key == 'arm' else second[0][key] + 1
            self.assertEqual(decoder.compare([first, second]), [])


unittest.main()
