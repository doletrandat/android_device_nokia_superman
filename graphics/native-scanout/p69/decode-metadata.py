#!/usr/bin/env python3
"""Offline P69 metadata index. Counts describe sampled records, never correct pixels."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

TAG = 'superman_meta '
TOKEN = re.compile(r'm69-[0-9a-f]{16}')


def analyze(lines):
    groups = defaultdict(dict)
    errors = []
    phase_counts = Counter()
    backing_roles = defaultdict(set)
    references = []
    for number, line in enumerate(lines, 1):
        if TAG not in line:
            continue
        try:
            text = line.split(TAG, 1)[1].strip()
            fields = text.split()
            row = dict(field.split('=', 1) for field in fields)
            if len(row) != len(fields) or not TOKEN.fullmatch(row['arm']):
                raise ValueError('duplicate field or invalid arm')
            role, phase = row['role'], row['phase']
            allowed = {'sf': {'begin', 'input', 'matrix', 'end'},
                       'gralloc': {'map_begin', 'map_end', 'unmap_begin', 'unmap_end'}}
            if role not in allowed or phase not in allowed[role]:
                raise ValueError('unknown role/phase')
            pid, seq = int(row['pid']), int(row['seq'])
            if pid <= 0 or not 1 <= seq <= (480 if role == 'sf' else 64):
                raise ValueError('invalid pid or over-budget sequence')
            suffix = ''
            if phase in {'input', 'matrix'}:
                index = int(row['index'])
                if not 0 <= index < 16:
                    raise ValueError('over-budget input index')
                suffix = '/' + str(index)
            if phase == 'matrix':
                if row['kind'] not in {'position', 'texture'} or len(row['values'].split(',')) != 16:
                    raise ValueError('invalid matrix')
                suffix += '/' + row['kind']
            if phase in {'begin', 'map_begin', 'input'}:
                if row['known'] not in {'0', '1'}:
                    raise ValueError('unknown identity status')
                dev, ino = int(row['dev']), int(row['ino'])
                if dev < 0 or ino < 0 or (row['known'] == '0' and (dev or ino)):
                    raise ValueError('invalid identity')
            if phase == 'begin':
                requests, truncated = int(row['requests']), int(row['truncated'])
                if requests < 0 or truncated != int(requests > 16):
                    raise ValueError('inconsistent truncation')
            if phase == 'end':
                if row['cache'] not in {'hit', 'miss', 'disabled'} or row['drawn'] not in {'0', '1'}:
                    raise ValueError('invalid cache decision')
                if (row['cache'] == 'hit') != (row['drawn'] == '0'):
                    raise ValueError('inconsistent draw decision')
                int(row['draw_status'])
                if row['ready_present'] not in {'0', '1'}:
                    raise ValueError('invalid fence presence')
                if row['cache'] == 'hit' and (row['ready_present'] != '0' or int(row['draw_status']) != 0):
                    raise ValueError('cache hit unexpectedly claims ready fence')
            if phase in {'map_end', 'unmap_end'}:
                int(row['status'])
                if phase == 'unmap_end' and int(row['status']) != -61:
                    raise ValueError('unmap status must be explicitly unknown')
            key = (row['arm'], pid, role, seq)
            label = phase + suffix
            if label in groups[key]:
                raise ValueError('duplicate phase; do not merge repeated buffers/log streams')
            groups[key][label] = row
            phase_counts[role + '/' + phase] += 1
            if row.get('known') == '1':
                backing_roles[(row['arm'], int(row['dev']), int(row['ino']))].add(role)
        except (KeyError, ValueError) as exc:
            if len(errors) < 64:
                errors.append({'line': number, 'error': str(exc), 'text': line[:2048]})
    summary = Counter()
    cache = Counter()
    durations = {'sf_cpu_request_to_return_ns': [], 'ordinary_map_ns': [], 'ordinary_unmap_ns': []}
    for key, rows in groups.items():
        role = key[2]
        if role == 'sf':
            begin, end = rows.get('begin'), rows.get('end')
            expected = min(int(begin['requests']), 16) if begin else -1
            complete = begin is not None and end is not None and all(
                'input/' + str(i) in rows and 'matrix/' + str(i) + '/position' in rows and
                'matrix/' + str(i) + '/texture' in rows for i in range(expected))
            summary['sf_complete_sample' if complete else 'sf_incomplete_sample'] += 1
            if begin:
                summary['sf_truncated_sample'] += int(begin['truncated'])
            if end:
                cache[end['cache']] += 1
                summary['draw_nonzero_status'] += int(int(end['draw_status']) != 0)
            pairs = [('begin', 'end', 'sf_cpu_request_to_return_ns')]
        else:
            mapped = rows.get('map_end')
            failed = mapped is not None and int(mapped['status']) != 0
            complete = 'map_begin' in rows and mapped is not None and (
                failed or ('unmap_begin' in rows and 'unmap_end' in rows))
            summary['map_complete_sample' if complete else 'map_incomplete_sample'] += 1
            summary['map_nonzero_status'] += int(failed)
            pairs = [('map_begin', 'map_end', 'ordinary_map_ns'),
                     ('unmap_begin', 'unmap_end', 'ordinary_unmap_ns')]
        for left, right, metric in pairs:
            if left in rows and right in rows:
                try:
                    start, stop = int(rows[left]['ns']), int(rows[right]['ns'])
                    if not 0 < start <= stop:
                        raise ValueError('backward/zero clock')
                    durations[metric].append(stop - start)
                except (KeyError, ValueError) as exc:
                    if len(errors) < 64:
                        errors.append({'group': list(key), 'error': str(exc)})
        if len(references) < 64 and (not complete or role == 'sf' and end and int(end['draw_status'])):
            references.append({'group': list(key), 'phases': sorted(rows)})
    return {'phase_counts': dict(phase_counts), 'summary': dict(summary),
            'cache_record_counts': dict(cache), 'validation_errors': errors,
            'incomplete_or_error_references': references,
            'shared_sf_gralloc_backing_count': sum(roles == {'sf', 'gralloc'} for roles in backing_roles.values()),
            'duration_samples_ns': durations,
            'limitations': ['Sampled CPU metadata only; no pixel, GPU completion or physical-frame validation.',
                           'Missing records may reflect sampling, ring loss, process death or an unfinished map.',
                           'A ready/acquire fence is recorded as present/absent, not queried or waited.',
                           'A cache hit and an absent fence are not themselves failures.',
                           'Unmap returns void; its logged -ENODATA is unknown status, not a failed unmap.',
                           'Shared device/inode supports backing correlation during this trace, not causality.',
                           'Only selected render settings are recorded; this is not a full cache-key equivalence test.',
                           'Hook/stat/log overhead remains; CPU durations are not GPU time or FPS.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    path = parser.parse_args().log
    # Follow the established collector mount guard before opening a log.
    import importlib.util
    spec = importlib.util.spec_from_file_location('observer', Path(__file__).resolve().parents[3] /
                                                'diagnostics/gpu/superman-systemwide-observe.py')
    observer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(observer)
    observer.linux_path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 32 * 1024**2:
        raise ValueError('Expected nonsymlink log of at most 32 MiB')
    print(json.dumps(analyze(path.read_text(errors='replace').splitlines()), indent=2))
