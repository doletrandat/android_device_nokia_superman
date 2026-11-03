#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import copy
import importlib.util
from pathlib import Path
import sys
import unittest
import shutil

spec = importlib.util.spec_from_file_location('decoder', Path(__file__).with_name('decode-pair-trace.py'))
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
FIXTURE = Path(sys.argv.pop(1))

class DecodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.records = []
        for p in sorted(FIXTURE.glob('*.pixels.txt')):
            cls.records.append({'name': p.name.removesuffix('.txt'),
                                'metadata': decoder.parse(p.read_text())})
        assert len(cls.records) == 4

    def test_valid_actual_writer_metadata(self):
        self.assertEqual(len(decoder.pairs(self.records)), 2)

    def test_duplicate_or_unknown_key(self):
        text = next(FIXTURE.glob('*.pixels.txt')).read_text()
        for suffix in ('version=p68\n', 'unexpected=1\n'):
            with self.assertRaises(ValueError):
                decoder.parse(text + suffix)

    def test_metadata_guards(self):
        record = self.records[0]
        for key, value in {
            'input_status': '0', 'ready_status': '0', 'draw_status': '1', 'wait_result': '110',
            'wait_limit_ms': '251', 'secure_layers': '1', 'protected': '1',
            'output_internal': '0', 'complete': '0', 'errno': '5', 'input_frame': '0',
            'input_buffer_id': '0', 'stride': '2945', 'backing_size': '1', 'width': '719',
            'height': '1279', 'bytes': '1', 'version': 'p67', 'ownership': 'post_latch',
            'settings_name': 'com.android.systemui#0', 'alpha': 'nan', 'bounds': '0,0,0,1280',
            'position_transform': '0,0', 'texture_transform': ','.join(['inf'] * 16),
            'compose_ns': '0', 'read_begin_ns': '1', 'arm_begin_ns': '0',
            'fnv1a64': 'BAD', 'buffer_id': '999', 'dev': '-1', 'ino': str(2**64),
        }.items():
            with self.subTest(key=key):
                altered = copy.deepcopy(record['metadata']); altered[key] = value
                with self.assertRaises(ValueError):
                    decoder.validate_metadata(altered, record['name'])

    def test_identity_alias_order_and_budget(self):
        good = decoder.pairs(self.records)[0]
        base = [good['input'], good['target']]
        cases = []
        for key in ('input_frame', 'compose_ns'):
            altered = copy.deepcopy(base)
            altered[1]['metadata'][key] = str(int(altered[1]['metadata'][key]) + 1)
            cases.append(altered)
        alias = copy.deepcopy(base)
        for key in ('dev', 'ino'):
            alias[1]['metadata'][key] = alias[0]['metadata'][key]
        cases.append(alias)
        order = copy.deepcopy(base)
        order[1]['metadata']['read_begin_ns'] = order[0]['metadata']['read_begin_ns']
        cases.append(order)
        cases.extend([base[:1], base + base, self.records + base])
        for altered in cases:
            with self.assertRaises(ValueError):
                decoder.pairs(altered)

    def test_expiry_and_filename(self):
        r = copy.deepcopy(self.records[0])
        r['metadata']['arm_begin_ns'] = str(int(r['metadata']['attempt_begin_ns']) - 5_000_000_001)
        with self.assertRaises(ValueError):
            decoder.validate_metadata(r['metadata'], r['name'])
        with self.assertRaises(ValueError):
            decoder.validate_metadata(self.records[0]['metadata'], '../../elsewhere.pixels')

    def test_failed_no_overwrite_capture_rejected(self):
        p = next((FIXTURE.parent / 'no-overwrite').glob('*-input-*.pixels.txt'))
        with self.assertRaises(ValueError):
            decoder.load(p)

    def test_raw_integrity_and_symlinks(self):
        source = next(FIXTURE.glob('*.pixels.txt'))
        raw = source.with_name(source.name.removesuffix('.txt'))
        root = FIXTURE.parent.parent / 'decoder-negative-fixtures'
        root.mkdir()
        for variant in ('hash', 'short', 'missing', 'raw-symlink', 'metadata-symlink'):
            directory = root / variant
            directory.mkdir()
            meta = directory / source.name
            target = directory / raw.name
            if variant == 'metadata-symlink':
                meta.symlink_to(source)
            else:
                shutil.copyfile(source, meta)
            if variant == 'hash':
                data = bytearray(raw.read_bytes()); data[0] ^= 255; target.write_bytes(data)
            elif variant == 'short':
                target.write_bytes(raw.read_bytes()[:4])
            elif variant == 'raw-symlink':
                target.symlink_to(raw)
            with self.subTest(variant=variant), self.assertRaises(ValueError):
                decoder.load(meta)

if __name__ == '__main__':
    unittest.main()
