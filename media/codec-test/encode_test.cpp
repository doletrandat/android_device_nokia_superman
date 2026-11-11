// Usage: superman-encode-test <out.h264> <codec-name|-> <width> <height> <frames> [bitrate] [fps]
// Encodes a deterministic moving test pattern (see pattern() below; the host regenerates it
// to measure PSNR) through MediaCodec in ByteBuffer mode, as an app would, and writes the
// H.264 elementary stream (codec config first). Prints codec, frame/byte counts and speed.
// Ends with "STALL" or "ERROR" instead of hanging when the encoder stops producing output.
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int64_t nowUs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

// Test pattern for frame n: moving diagonal luma gradient plus a sliding bright square, and
// slowly varying flat chroma. Kept simple so a host script can reproduce it bit-exactly.
static uint8_t lumaAt(int x, int y, int n, int w) {
    const int sq = w / 8, sx = (n * 4) % (w - sq), sy = (n * 2) % (w / 2);
    if (x >= sx && x < sx + sq && y >= sy && y < sy + sq) return 235;
    return uint8_t(16 + ((x + y + n * 3) % 200));
}
static uint8_t cbAt(int n) { return uint8_t(128 + ((n % 64) - 32)); }
static uint8_t crAt(int n) { return uint8_t(128 - ((n % 48) - 24)); }

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s out.h264 codec|- width height frames [bitrate] [fps]\n", argv[0]);
        return 2;
    }
    const char* codecName = strcmp(argv[2], "-") ? argv[2] : nullptr;
    const int w = atoi(argv[3]), h = atoi(argv[4]), frames = atoi(argv[5]);
    const int bitrate = argc > 6 ? atoi(argv[6]) : 4000000;
    const int fps = argc > 7 ? atoi(argv[7]) : 30;
    const int pauseMs = getenv("ENC_PAUSE_MS") ? atoi(getenv("ENC_PAUSE_MS")) : 0;
    const bool dsb = getenv("ENC_DSB") != nullptr;  // test aid: full-system barrier after writing

    AMediaCodec* codec = codecName ? AMediaCodec_createCodecByName(codecName)
                                   : AMediaCodec_createEncoderByType("video/avc");
    if (!codec) { fprintf(stderr, "FAIL create\n"); return 3; }
    char* name = nullptr;
    AMediaCodec_getName(codec, &name);
    printf("codec %s\n", name ? name : "?");
    if (name) AMediaCodec_releaseName(codec, name);

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F420888);  // YUV420Flexible
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    if (AMediaCodec_configure(codec, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE)) {
        fprintf(stderr, "FAIL configure\n"); return 4;
    }
    if (AMediaCodec_start(codec)) { fprintf(stderr, "FAIL start\n"); return 4; }

    // Input layout: planar I420 with the stride/slice-height the codec reports.
    AMediaFormat* in = AMediaCodec_getInputFormat(codec);
    int32_t stride = w, slice = h;
    if (in) {
        AMediaFormat_getInt32(in, AMEDIAFORMAT_KEY_STRIDE, &stride);
        AMediaFormat_getInt32(in, "slice-height", &slice);
        printf("input %s\n", AMediaFormat_toString(in));
        AMediaFormat_delete(in);
    }
    if (stride < w) stride = w;
    if (slice < h) slice = h;

    int out = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { perror("open out"); return 2; }

    int fed = 0, got = 0, keys = 0;
    long bytes = 0;
    bool inEos = false, outEos = false, stalled = false, failed = false;
    const int64_t t0 = nowUs();
    int64_t lastOut = t0;
    while (!outEos) {
        if (nowUs() - lastOut > 5000000) { stalled = true; break; }
        if (!inEos) {
            ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (ib >= 0) {
                size_t cap;
                uint8_t* buf = AMediaCodec_getInputBuffer(codec, ib, &cap);
                const size_t need = size_t(stride) * slice * 3 / 2;
                if (fed >= frames || cap < need) {
                    if (fed < frames) fprintf(stderr, "input buffer %zu < %zu\n", cap, need);
                    AMediaCodec_queueInputBuffer(codec, ib, 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    inEos = true;
                } else {
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) buf[y * stride + x] = lumaAt(x, y, fed, w);
                    uint8_t* u = buf + stride * slice;
                    uint8_t* v = u + (stride / 2) * (slice / 2);
                    for (int y = 0; y < h / 2; y++) {
                        memset(u + y * (stride / 2), cbAt(fed), w / 2);
                        memset(v + y * (stride / 2), crAt(fed), w / 2);
                    }
#if defined(__arm__)
                    if (dsb) asm volatile("dsb sy" ::: "memory");
#endif
                    AMediaCodec_queueInputBuffer(codec, ib, 0, need,
                                                 int64_t(fed) * 1000000 / fps, 0);
                    fed++;
                    if (pauseMs) usleep(pauseMs * 1000);  // test aid: pace input
                }
            }
        }
        AMediaCodecBufferInfo info;
        ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (ob >= 0) {
            lastOut = nowUs();
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) outEos = true;
            if (info.size > 0) {
                size_t cap;
                uint8_t* buf = AMediaCodec_getOutputBuffer(codec, ob, &cap) + info.offset;
                if (write(out, buf, info.size) != info.size) { perror("write"); return 2; }
                bytes += info.size;
                if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                    got++;
                    if (info.flags & 1 /* key frame */) keys++;
                }
            }
            AMediaCodec_releaseOutputBuffer(codec, ob, false);
        } else if (ob != AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
                   ob != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
                   ob != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            failed = true;
            break;
        }
    }
    const int64_t us = nowUs() - t0;
    close(out);
    printf("fed %d encoded %d keyframes %d bytes %ld %s ms %lld fps*10 %lld kbps %ld\n", fed, got,
           keys, bytes, outEos ? "eos" : "noeos", (long long)(us / 1000),
           (long long)(us ? got * 10000000LL / us : 0),
           got ? bytes * 8 * fps / got / 1000 : 0L);
    if (stalled) printf("STALL no output for 5 s\n");
    if (failed) printf("ERROR codec error\n");
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AMediaFormat_delete(fmt);
    return got == frames && outEos ? 0 : 5;
}
