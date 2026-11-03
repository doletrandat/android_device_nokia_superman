#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate P68 owned Settings input/composed target pairs; retain originals.

Input and target are different stages, formats and potentially transforms.
Their pixel differences do not themselves prove corruption. The target is
post-draw/pre-queue, with no physical-latch or panel-image claim.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

BYTES = 720 * 1280 * 4
NAME = re.compile(r"([0-9a-f]{16})-([1-9][0-9]*)-([12])-(input|target)-([1-9][0-9]*)\.pixels")
SETTINGS = re.compile(r"com\.android\.settings/com\.android\.settings\.SubSettings#[0-9]{1,10}")
INTEGER = {
    'pid', 'attempt', 'buffer_id', 'input_buffer_id', 'input_frame', 'target_buffer_id',
    'compose_ns', 'arm_begin_ns', 'attempt_begin_ns', 'read_begin_ns', 'read_end_ns',
    'write_end_ns', 'dev', 'ino', 'backing_size', 'width', 'height', 'stride', 'bytes',
    'draw_status', 'wait_result', 'wait_limit_ms', 'input_fd', 'input_status',
    'ready_fd', 'ready_status', 'secure_layers', 'protected', 'output_internal',
    'complete', 'errno',
}
KEYS = INTEGER | {'version', 'arm', 'role', 'format', 'fnv1a64', 'ownership',
                  'settings_name', 'bounds', 'alpha', 'position_transform', 'texture_transform'}
SHARED = KEYS - {'role', 'buffer_id', 'dev', 'ino', 'backing_size', 'stride', 'format',
                 'fnv1a64', 'read_begin_ns', 'read_end_ns', 'write_end_ns'}

def require(condition, reason):
    if not condition:
        raise ValueError(reason)

def parse(text):
    d = {}
    for line in text.splitlines():
        require('=' in line, 'non-key-value metadata')
        key, value = line.split('=', 1)
        require(key not in d, 'duplicate metadata key: ' + key)
        d[key] = value
    require(d.keys() == KEYS, 'missing or unknown metadata keys')
    return d

def numbers(text, count):
    values = [float(x) for x in text.split(',')]
    require(len(values) == count and all(math.isfinite(x) for x in values), 'invalid geometry')
    return values

def validate_metadata(d, filename):
    match = NAME.fullmatch(filename)
    require(match is not None, 'invalid filename')
    arm, pid, attempt, role, ident = match.groups()
    require((d['arm'], d['pid'], d['attempt'], d['role'], d['buffer_id']) ==
            (arm, pid, attempt, role, ident), 'filename identity mismatch')
    values = {}
    for key in INTEGER:
        require(re.fullmatch(r'0|[1-9][0-9]*', d[key]) is not None, 'invalid integer: ' + key)
        values[key] = int(d[key])
        require(values[key] <= 2**64 - 1, 'integer overflow: ' + key)
    require(d['version'] == 'p68' and SETTINGS.fullmatch(d['settings_name']), 'wrong source/version')
    require(d['ownership'] == 'post_draw_before_queue', 'wrong ownership stage')
    for key, expected in {'width': 720, 'height': 1280, 'bytes': BYTES, 'draw_status': 0,
                          'wait_result': 0, 'wait_limit_ms': 250, 'input_status': 1,
                          'ready_status': 1, 'secure_layers': 0, 'protected': 0,
                          'output_internal': 1, 'complete': 1, 'errno': 0}.items():
        require(values[key] == expected, 'invalid guard: ' + key)
    require(values['pid'] > 0 and values['input_frame'] > 0 and values['input_buffer_id'] > 0
            and values['target_buffer_id'] > 0, 'missing frame/buffer identity')
    require(values['input_fd'] >= 0 and values['ready_fd'] >= 3, 'missing real fence')
    require(values['ino'] > 0 and 2880 <= values['stride'] <= 65535 and
            values['stride'] % 4 == 0 and values['backing_size'] >= values['stride'] * 1280,
            'invalid backing/layout')
    require(d['format'] in ('RGBA8888', 'BGRA8888') and
            (role != 'target' or d['format'] == 'BGRA8888'), 'invalid format')
    require(values['buffer_id'] == values[role + '_buffer_id'], 'role buffer identity mismatch')
    require(re.fullmatch(r'[0-9a-f]{16}', d['fnv1a64']) is not None, 'invalid hash')
    require(0 < values['compose_ns'] <= values['attempt_begin_ns'] <= values['read_begin_ns']
            <= values['read_end_ns'] <= values['write_end_ns'], 'non-monotonic capture')
    require(0 < values['arm_begin_ns'] <= values['attempt_begin_ns'] and
            values['attempt_begin_ns'] - values['arm_begin_ns'] <= 5_000_000_000,
            'expired initiation')
    bounds = numbers(d['bounds'], 4)
    require(bounds[0] < bounds[2] and bounds[1] < bounds[3], 'empty bounds')
    require(0 <= numbers(d['alpha'], 1)[0] <= 1, 'invalid alpha')
    numbers(d['position_transform'], 16)
    numbers(d['texture_transform'], 16)
    return values

def fnv(data):
    value = 14695981039346656037
    for b in data:
        value = ((value ^ b) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'

def load(path):
    require(not path.is_symlink() and path.is_file() and path.stat().st_size <= 4096,
            'unsafe metadata file')
    d = parse(path.read_text(encoding='ascii'))
    filename = path.name.removesuffix('.txt')
    validate_metadata(d, filename)
    raw = path.with_name(filename)
    require(not raw.is_symlink() and raw.is_file() and raw.stat().st_size == BYTES,
            'missing/short/unsafe pixels')
    data = raw.read_bytes()
    require(fnv(data) == d['fnv1a64'], 'pixel hash mismatch')
    return {'name': filename, 'metadata': d, 'sha256': hashlib.sha256(data).hexdigest(), 'data': data}

def pairs(records):
    require(0 < len(records) <= 4, 'invalid snapshot budget')
    grouped = {}
    identities = set()
    for r in records:
        d = r['metadata']
        validate_metadata(d, r['name'])
        identities.add((d['arm'], d['pid']))
        key = int(d['attempt'])
        group = grouped.setdefault(key, {})
        require(d['role'] not in group, 'duplicate pair role')
        group[d['role']] = r
    require(len(identities) == 1 and len(grouped) <= 2, 'multiple processes/arms')
    result = []
    for attempt, group in sorted(grouped.items()):
        require(group.keys() == {'input', 'target'}, 'incomplete pair')
        a, b = group['input']['metadata'], group['target']['metadata']
        require(all(a[k] == b[k] for k in SHARED), 'pair identity/geometry/fence mismatch')
        require((a['dev'], a['ino']) != (b['dev'], b['ino']), 'aliased input/target')
        require(int(a['read_end_ns']) <= int(b['read_begin_ns']) and
                int(b['read_end_ns']) <= int(a['write_end_ns']), 'incorrect read/write order')
        result.append({'attempt': attempt, **group})
    return result

def decode(directory, output):
    from PIL import Image, ImageDraw
    records = [load(p) for p in sorted(directory.glob('*.pixels.txt'))]
    matched = pairs(records)
    require(not output.exists(), 'output already exists')
    output.mkdir(parents=True)
    contact = Image.new('RGB', (720 * 2, 1320 * len(matched)), 'white')
    draw = ImageDraw.Draw(contact)
    report = []
    for row, pair in enumerate(matched):
        item = {'attempt': pair['attempt'], 'stage': 'post_draw_before_queue', 'input': {}, 'target': {}}
        for column, role in enumerate(('input', 'target')):
            r = pair[role]
            d = r['metadata']
            mode = 'RGBA' if d['format'] == 'RGBA8888' else 'BGRA'
            im = Image.frombytes('RGBA', (720, 1280), r['data'], 'raw', mode).convert('RGB')
            im.save(output / (r['name'] + '.png'))
            contact.paste(im, (720 * column, 1320 * row + 40))
            draw.text((720 * column + 8, 1320 * row + 8),
                      f"Attempt {pair['attempt']} {role}; buffer {d['buffer_id']}; input frame {d['input_frame']}", fill='black')
            item[role] = {'file': r['name'], 'sha256': r['sha256'], 'metadata': d}
        report.append(item)
    contact.save(output / 'pairs.png')
    (output / 'comparison.json').write_text(json.dumps({
        'passed': True, 'pairs': report,
        'interpretation': 'Owned CPU readbacks, after draw and before queue. Input/target pixel differences may reflect transforms, blending, other layers, color conversion or CPU visibility. No physical latch/panel synchronization or corruption attribution.'
    }, indent=2) + '\n')
    return len(matched)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        print('PASS decoded pairs:', decode(args.directory, args.output))
    except (ValueError, OSError, UnicodeError) as error:
        parser.exit(1, 'REJECT: ' + str(error) + '\n')
