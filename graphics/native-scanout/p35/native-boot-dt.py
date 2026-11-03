#!/usr/bin/env python3
"""Derive a private native-probe DT from the exact retained P30 binary DT."""
from pathlib import Path
import copy
import hashlib
import struct
import subprocess

P30_DT_SHA = '6a59037eac73f9354d635e3075f326b8a7f40fc62a370b07760daf0643515ac2'
FB = '/chosen/framebuffer@3200000'
MDSS = '/soc/display-subsystem@fd900000'
GIC = '/soc/interrupt-controller@f9000000'
OWNER = '/firmware-display@fd900000'

def expected_nodes(baseline, cmdline, dt_nodes):
    if hashlib.sha256(baseline).hexdigest() != P30_DT_SHA:
        raise ValueError('Exact P30 DT required')
    tokens = cmdline.split()
    required = ['androidboot.superman_native=p34']
    stages = [t for t in tokens if t.startswith('androidboot.superman_native_stage=')]
    if any(tokens.count(t) != 1 for t in required) or len(stages) != 1 or stages[0] not in ['androidboot.superman_native_stage=probe', 'androidboot.superman_native_stage=flip', 'androidboot.superman_native_stage=gpu', 'androidboot.superman_native_stage=hwc']:
        raise ValueError('Exact native candidate and stage flags required')
    if any(t.startswith('androidboot.superman_native=') and t not in required for t in tokens):
        raise ValueError('Conflicting native flag')
    if any(c in cmdline for c in ['"', '\\', '\n', '\r']):
        raise ValueError('Unsafe command line')
    tree = copy.deepcopy(dt_nodes(baseline))
    if OWNER in tree or tree[MDSS]['status'] != b'disabled\0':
        raise ValueError('Unexpected P30 owner state')
    if 'phandle' in tree[FB]:
        raise ValueError('Unexpected P30 framebuffer phandle')
    maximum = max(struct.unpack('>I', n['phandle'])[0] for n in tree.values() if 'phandle' in n)
    fb = struct.pack('>I', maximum + 1)
    tree[FB]['phandle'] = fb
    tree['/chosen']['bootargs'] = cmdline.encode('ascii') + b'\0'
    tree[OWNER] = {
        'compatible': b'nokia,superman-firmware-kms\0',
        'reg': struct.pack('>2I', 0xfd900000, 0x23000),
        'interrupt-parent': tree[GIC]['phandle'],
        'interrupts': struct.pack('>3I', 0, 72, 4),
        'superman,firmware-initialized': b'',
        'superman,retained-framebuffer': fb,
        'superman,normal-mdss': tree[MDSS]['phandle'],
        'status': b'okay\0',
    }
    return tree

def validate_dt(candidate, baseline, cmdline, dt_nodes):
    expected = expected_nodes(baseline, cmdline, dt_nodes)
    actual = dt_nodes(candidate)
    if actual != expected:
        changed = [p for p in sorted(set(actual) | set(expected)) if actual.get(p) != expected.get(p)]
        raise ValueError('DT differs from exact P30 plus permitted native changes: ' + ', '.join(changed))

def build_dt(baseline, cmdline, output, dtc, dt_nodes):
    output = Path(output)
    expected = expected_nodes(baseline, cmdline, dt_nodes)
    source = output / 'p30-reference.dtb'
    candidate = output / 'candidate.dtb'
    if source.exists() or candidate.exists():
        raise ValueError('Preserve existing DT artifacts')
    source.write_bytes(baseline)
    old = subprocess.run([str(dtc), '-q', '-I', 'dtb', '-O', 'dts', str(source)], capture_output=True, text=True, check=True)
    owner = expected[OWNER]
    ph = lambda name: struct.unpack('>I', owner[name])[0]
    addition = f'''\n/ {{
    chosen {{
        bootargs = "{cmdline}";
        framebuffer@3200000 {{ phandle = <0x{ph('superman,retained-framebuffer'):x}>; }};
    }};
    firmware-display@fd900000 {{
        compatible = "nokia,superman-firmware-kms";
        reg = <0xfd900000 0x23000>;
        interrupt-parent = <0x{ph('interrupt-parent'):x}>;
        interrupts = <0 72 4>;
        superman,firmware-initialized;
        superman,retained-framebuffer = <0x{ph('superman,retained-framebuffer'):x}>;
        superman,normal-mdss = <0x{ph('superman,normal-mdss'):x}>;
        status = "okay";
    }};
}};\n'''
    dts = output / 'candidate.dts'
    dts.write_text(old.stdout + addition)
    q = subprocess.run([str(dtc), '-q', '-I', 'dts', '-O', 'dtb', '-o', str(candidate), str(dts)], capture_output=True, text=True)
    (output / 'dtc.log').write_text(old.stderr + q.stdout + q.stderr)
    q.check_returncode()
    validate_dt(candidate.read_bytes(), baseline, cmdline, dt_nodes)
    return candidate.read_bytes()
