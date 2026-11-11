import sys
w, h = int(sys.argv[2]), int(sys.argv[3]); n = w * h * 3 // 2
d = open(sys.argv[1], 'rb').read()
for k in range(len(d) // n):
    hv = 2166136261
    for c in d[k * n:(k + 1) * n]: hv = ((hv ^ c) * 16777619) & 0xffffffff
    print(f'frame {k} {hv:08x}')
