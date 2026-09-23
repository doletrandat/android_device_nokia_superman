#!/usr/bin/env python3
"""Classify every Image difference against the hash-pinned working r37 Image."""
from pathlib import Path
import gzip
import hashlib
import json
import struct

root = Path(__file__).resolve().parents[4]
old = (root / 'handoff/evidence/touch-r33-20260919/r33.Image').read_bytes()
new = (root / '.codex-restart/builds/r37-cpu-control-20260923/arch/arm/boot/Image').read_bytes()
assert hashlib.sha256(old).hexdigest() == '66d0561ed23a3a40f46b23a6b226879c01bb85b010f907b5b56cd7cb3151c5a1'
assert len(old) == len(new)
base = 0xc0008000
symbols = {f[2]: int(f[0], 16) - base for line in
           (root / '.codex-restart/builds/r37-cpu-control-20260923/System.map').read_text().splitlines()
           if (f := line.split())}
starts = [data.index(b'IKCFG_ST') for data in (old, new)]
ends = [data.index(b'IKCFG_ED', start) for data, start in zip((old, new), starts)]
assert starts[0] == starts[1]
configs = [gzip.decompress(data[start+8:end]).decode().splitlines()
           for data, start, end in zip((old, new), starts, ends)]
delta = set(configs[0]) ^ set(configs[1])
assert delta <= {'CONFIG_CC_CAN_LINK=y', '# CONFIG_DEBUG_INFO_SPLIT is not set'}
regions = [(starts[0], max(ends)+8, 'compressed config and end marker')]

# Both changed instructions materialize kernel_config_data_end's low 16 bits.
# Validate the ARM MOVW opcode/register and immediate instead of masking
# arbitrary code differences.
for symbol, relative in [('ikconfig_read_current', 8), ('ikconfig_init', 40)]:
    offset = symbols[symbol] + relative
    words = [struct.unpack_from('<I', data, offset)[0] for data in (old, new)]
    imm_mask = 0x000f0fff
    assert words[0] & ~imm_mask == words[1] & ~imm_mask
    assert words[0] & 0x0ff00000 == 0x03000000
    immediates = [((word >> 4) & 0xf000) | (word & 0xfff) for word in words]
    assert immediates == [(base + end) & 0xffff for end in ends]
    regions.append((offset, offset+4, symbol + ' config-end MOVW'))

signature = struct.pack('<III', 4, 20, 3) + b'GNU\0'
notes = []
offset = 0
while (offset := old.find(signature, offset)) >= 0:
    assert new[offset:offset+16] == signature
    notes.append(offset+16)
    offset += 16
assert len(notes) == 2
for offset in notes:
    regions.append((offset, offset+20, 'GNU build-id descriptor'))

differences = 0
unexpected = []
for offset, (a, b) in enumerate(zip(old, new)):
    if a != b:
        differences += 1
        if not any(start <= offset < end for start, end, _ in regions):
            unexpected.append(offset)
assert not unexpected, 'Unclassified differences: ' + repr(unexpected[:20])
print(json.dumps({'proven_image_sha256': hashlib.sha256(old).hexdigest(),
                  'control_image_sha256': hashlib.sha256(new).hexdigest(),
                  'image_bytes': len(new), 'different_bytes': differences,
                  'config_text_delta': sorted(delta),
                  'regions': [{'start': hex(a), 'end_exclusive': hex(b), 'meaning': c}
                              for a, b, c in regions],
                  'other_bytes_identical': True,
                  'limitation': 'Decompressed Image comparison; not hardware boot validation'}, indent=2))
