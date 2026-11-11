"""encpsnr.py stream.h264 w h: decode with ffmpeg, compare to superman-encode-test's pattern."""
import sys, subprocess, math
f, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', f, '-f', 'rawvideo', '-pix_fmt', 'yuv420p', '-'],
                     capture_output=True).stdout
fs = w * h * 3 // 2; n = len(raw) // fs
def luma(x, y, k):
    sq = w // 8; sx = (k * 4) % (w - sq); sy = (k * 2) % (w // 2)
    if sx <= x < sx + sq and sy <= y < sy + sq: return 235
    return 16 + ((x + y + k * 3) % 200)
ps = []; means = []
for k in range(n):
    fr = raw[k * fs:(k + 1) * fs]
    se = 0
    for y in range(0, h, 2):           # subsample rows/cols for speed
        row = fr[y * w:(y + 1) * w]
        for x in range(0, w, 2):
            d = row[x] - luma(x, y, k); se += d * d
    mse = se / ((h // 2) * (w // 2))
    ps.append(99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse))
    means.append(sum(fr[:w * h]) / (w * h))
print(f'{f}: frames {n} luma PSNR min {min(ps):.1f} avg {sum(ps)/len(ps):.1f} dB, mean luma first {means[0]:.0f} last {means[-1]:.0f}')
