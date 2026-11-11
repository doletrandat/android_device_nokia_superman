"""Split an Annex B H.264 stream with AUDs into <len u32 LE><AU> records."""
import sys, struct
d = open(sys.argv[1], 'rb').read(); aud = b'\x00\x00\x00\x01\x09'
idx = []; i = d.find(aud)
while i >= 0: idx.append(i); i = d.find(aud, i + 1)
idx.append(len(d))
with open(sys.argv[2], 'wb') as f:
    for a, b in zip(idx, idx[1:]): f.write(struct.pack('<I', b - a) + d[a:b])
print(len(idx) - 1, 'AUs', max(b - a for a, b in zip(idx, idx[1:])), 'max bytes')
