import sys, subprocess, math
f, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); step = int(sys.argv[4]) if len(sys.argv) > 4 else 4
raw = subprocess.run(['ffmpeg','-v','error','-i',f,'-f','rawvideo','-pix_fmt','yuv420p','-'], capture_output=True).stdout
fs = w*h*3//2; n = len(raw)//fs
def luma(x, y, k):
    sq = w//8; sx = (k*4) % (w-sq); sy = (k*2) % (w//2)
    return 235 if (sx <= x < sx+sq and sy <= y < sy+sq) else 16 + ((x+y+k*3) % 200)
bad = []; ps = []
for k in range(n):
    fr = raw[k*fs:(k+1)*fs]; se = 0; c = 0
    for y in range(0, h, step):
        o = y*w
        for x in range(0, w, step): d = fr[o+x]-luma(x, y, k); se += d*d; c += 1
    mse = se/c; p = 99 if mse == 0 else 10*math.log10(65025/mse); ps.append(p)
    if p < 40: bad.append((k, round(p, 1)))
print(f"{f.split('/')[-1]}: {n} frames, avg {sum(ps)/len(ps):.1f} dB, frames < 40 dB: {bad}")
