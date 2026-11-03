#!/usr/bin/env python3
"""Offline index of sealed systemwide logs; stdout only, never calls the phone."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location('observer', Path(__file__).with_name('superman-metadata-observe.py'))
observer = importlib.util.module_from_spec(spec); spec.loader.exec_module(observer)
PATTERNS = {
    'GPU_lifecycle_live_error': r'SUPERMAN_GPU_LIFECYCLE .*history=0 .*ret=-[1-9]',
    'GPU_lifecycle_history_error': r'SUPERMAN_GPU_LIFECYCLE .*history=1 .*ret=-[1-9]',
    'GPU_recovery_live_record': r'SUPERMAN_GPU_LIFECYCLE .*history=0 .*stage=recovery_',
    'GPU_lockup_fault_message': r'(?:gpu|adreno|msm).*(?:lockup|hangcheck|gpu fault|gpu_fault)',
    'IOMMU_fault_message': r'(?:IOMMU|iommu|arm-smmu).*(?:faulted=[1-9]|Unhandled.*fault|context fault|translation fault)',
    'native_acquire_rejection': r'FW_ACQUIRE rejected',
    'native_backing_failure': r'FW_PIN (?:latch_timeout|backing_extent_changed|distinct_backing_limit|cleanup_unmatched)',
    'composition_failure': r'Failed to apply the frame composition|Failed to (?:commit|create).*composition',
    'graphics_API_error': r'EGL_CONTEXT_LOST|EGL_BAD_(?:ALLOC|CONTEXT|MATCH|SURFACE)|GL_OUT_OF_MEMORY|GL_INVALID_OPERATION',
    'allocation_texture_error': r'(?:[Tt]exture|[Aa]tlas|GMEM).*(?:[Ff]ail|[Ee]rror)|(?:Unable|Failed) to allocate',
    'app_crash_ANR_message': r'FATAL EXCEPTION|Fatal signal [0-9]|ANR in ',
    'pixel_diagnostic_tag': r'superman_target_trace |superman_sf_pair ',
}
PATTERNS = {k:re.compile(v) for k,v in PATTERNS.items()}


def index_lines(lines, source):
    counts = Counter(); hits = []; lifecycle = Counter(); presents = Counter(); wall = []
    for number, line in enumerate(lines, 1):
        for kind, pattern in PATTERNS.items():
            if pattern.search(line):
                counts[kind] += 1
                if len(hits) < 120:
                    hits.append({'kind':kind, 'source':source, 'line':number, 'text':line[:2048]})
        if 'SUPERMAN_GPU_LIFECYCLE' in line:
            row = dict(re.findall(r'(history|stage|ret)=([^ ]+)',line))
            lifecycle['history=' + row.get('history','?') + '/stage=' + row.get('stage','?')] += 1
        if 'NATIVE_SF_HWC_PRESENT' in line:
            match = re.search(r'present_fence=(\w+)',line)
            presents[match.group(1) if match else 'missing_field'] += 1
        if 'SUPERMAN_SF_POSTCOMP end_ns=' in line:
            match = re.search(r'wall_ns=(\d+)',line)
            if match: wall.append(int(match.group(1)))
    wall.sort()
    metric = {'samples':len(wall), 'metric':'SF postComposition CPU wall time; not frame/render/scanout time or FPS'}
    if wall:
        metric.update(min_ns=wall[0], max_ns=wall[-1], median_sample_ns=wall[len(wall)//2])
    return {'source':source,'lines':number if 'number' in locals() else 0, 'event_line_counts':dict(counts),
        'references':hits, 'references_capped':sum(counts.values()) > len(hits),
        'lifecycle_record_counts':dict(lifecycle), 'present_fence_record_counts':dict(presents),
        'postComposition_wall':metric}


def analyze(run):
    if run.is_symlink() or not run.is_dir():raise ValueError('Existing nonsymlink session required')
    observer.linux_path(run)
    def data(name):
        path=run/name
        if path.is_symlink() or path.stat().st_size > 32*1024**2:raise ValueError('Symlink/oversize file refused')
        observer.linux_path(path)
        return path.read_bytes()
    entries=data('SHA256SUMS').decode().splitlines()
    if not entries or len(entries)>180:raise ValueError('Invalid seal size')
    sealed={}
    for row in entries:
        match=re.fullmatch(r'([0-9a-f]{64})  ([a-zA-Z0-9][a-zA-Z0-9._-]*)',row)
        if not match or match[2] in sealed:raise ValueError('Invalid/duplicate seal path')
        sealed[match[2]]=match[1]
        if hashlib.sha256(data(match[2])).hexdigest()!=match[1]:raise ValueError('Seal mismatch: '+match[2])
    for name in ['binding.json','result.json','window.json']:
        if name not in sealed:raise ValueError('No observed sealed window: '+name)
    result=json.loads(data('result.json')); binding=json.loads(data('binding.json'))
    streams=[]
    for buffer in observer.BUFFERS:
        name='logcat-'+buffer+'.txt'
        if name in sealed:streams.append(index_lines(data(name).decode(errors='replace').splitlines(),name))
    decoder_path = Path(__file__).resolve().parents[2] / 'graphics/native-scanout/p69/decode-metadata.py'
    decoder_spec = importlib.util.spec_from_file_location('metadata_decoder', decoder_path)
    decoder = importlib.util.module_from_spec(decoder_spec); decoder_spec.loader.exec_module(decoder)
    metadata = decoder.analyze(data('logcat-main.txt').decode(errors='replace').splitlines()) if 'logcat-main.txt' in sealed else None
    return {'metadata': metadata, 'seal_verified':True,'boot':binding['boot'],'session_result':result,
        'window':json.loads(data('window.json')),'streams':streams,
        'limitations':['Counts are log-line occurrences, not independent failures or GPU recovery cycles.',
            'Each logcat -T 1 stream includes a preceding tail entry; do not assume every indexed entry happened in the window.',
            'History=1 lifecycle records replay prior software history; they are separate from live records.',
            'A zero-time present-fence poll marked unconfirmed is not itself proof of a failed job.',
            'Signaled fences and no matching errors do not prove correct pixels or tear-free scanout.',
            'User markers use host timestamps and are not physical-frame synchronized.',
            'Observer load and existing image logging remain; pixel reads were not issued by this collector.']}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('session',type=Path)
    print(json.dumps(analyze(p.parse_args().session),indent=2))
