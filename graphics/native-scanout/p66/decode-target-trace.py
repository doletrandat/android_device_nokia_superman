#!/usr/bin/env python3
"""Validate and compare P66 target snapshots; preserve input raw bytes."""
import argparse
import hashlib
import json
import re
from pathlib import Path

WIDTH, HEIGHT, ROW = 720, 1280, 2880
NAME = re.compile(r'([0-9a-f]{16})-(\d+)-([12])-(before|latched|retiring)-(\d+)\.bgra')


def load(path):
    match = NAME.fullmatch(path.name)
    if not match:
        raise ValueError(f'invalid filename: {path.name}')
    pairs = [line.split('=', 1) for line in path.with_name(path.name + '.txt').read_text().splitlines()]
    if any(len(p) != 2 for p in pairs) or len({p[0] for p in pairs}) != len(pairs):
        raise ValueError(f'invalid or duplicate metadata: {path.name}')
    meta = dict(pairs)
    if (meta['version'], meta['arm'], meta['attempt'], meta['stage'], meta['frame']) != (
            'p66', match[1], match[3], match[4], match[5]):
        raise ValueError(f'filename/metadata mismatch: {path.name}')
    if meta['format'] != 'BGRA8888' or meta['complete'] != '1' or meta['errno'] != '0':
        raise ValueError(f'incomplete/failed snapshot: {path.name}')
    numeric = ('attempt', 'frame', 'latches', 'fb', 'gem', 'dev', 'ino', 'backing_size',
               'stride', 'acquire_fd', 'acquire', 'latch', 'begin_ns', 'read_end_ns',
               'write_end_ns', 'bytes')
    for key in numeric:
        meta[key] = int(meta[key])
    if (meta['acquire_fd'] < 0 or meta['acquire'] != 1 or
            (meta['stage'] != 'before' and meta['latch'] != 1)):
        raise ValueError(f'unsuccessful fence: {path.name}')
    if not (ROW <= meta['stride'] <= 65535 and meta['stride'] % 4 == 0 and
            meta['backing_size'] >= meta['stride'] * HEIGHT and
            0 < meta['begin_ns'] <= meta['read_end_ns'] <= meta['write_end_ns']):
        raise ValueError(f'invalid layout/timestamps: {path.name}')
    data = path.read_bytes()
    if len(data) != ROW * HEIGHT or meta['bytes'] != len(data):
        raise ValueError(f'incomplete raw pixels: {path.name}')
    h = 14695981039346656037
    for byte in data:
        h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)
    if f'{h:016x}' != meta['fnv1a64']:
        raise ValueError(f'content hash mismatch: {path.name}')
    meta.update(file=path.name, sha256=hashlib.sha256(data).hexdigest(), pid=int(match[2]))
    return meta, data


def compare(records):
    results = []
    for i, (a, pixels_a) in enumerate(records):
        for b, pixels_b in records[i + 1:]:
            # Compare the same source frame/backing across its three ownership points.
            # GEM handles and fd integers alone do not identify the backing.
            if ((a['arm'], a['pid'], a['dev'], a['ino'], a['frame']) !=
                    (b['arm'], b['pid'], b['dev'], b['ino'], b['frame'])):
                continue
            rows = [y for y in range(HEIGHT)
                    if pixels_a[y * ROW:(y + 1) * ROW] != pixels_b[y * ROW:(y + 1) * ROW]]
            results.append(dict(first=a['file'], second=b['file'], identical=not rows,
                                changed_rows=rows))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output-dir', type=Path, help='new derived PNG/report directory')
    args = parser.parse_args()
    paths = sorted(args.capture.glob('*.bgra'))
    if not paths:
        parser.error('no raw target snapshots; skipped reads are not captures')
    records = [load(p) for p in paths]
    if len(records) > 6 or len({(m['arm'], m['pid']) for m, _ in records}) != 1:
        parser.error('expected one bounded HAL arm, at most six snapshots')
    if len({(m['attempt'], m['stage']) for m, _ in records}) != len(records):
        parser.error('duplicate attempt/stage')
    report = dict(snapshots=[m for m, _ in records], comparisons=compare(records),
                  note='Diagnostic reads add latency; equality does not prove correct physical scanout.')
    if args.output_dir:
        args.output_dir.mkdir(exist_ok=False)
        from PIL import Image
        for meta, pixels in records:
            Image.frombytes('RGBA', (WIDTH, HEIGHT), pixels, 'raw', 'BGRA').save(
                args.output_dir / (meta['file'] + '.png'))
        (args.output_dir / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
