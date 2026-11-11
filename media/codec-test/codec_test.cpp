// Usage: superman-codec-test <file> [track-mime-prefix=video/] [codec-name] [max-frames]
// Decodes the first track whose MIME starts with the prefix, in ByteBuffer mode, and
// prints per-frame FNV-1a hashes of the visible picture (video: Y plane + chroma as
// laid out by the codec's output format) or of the PCM bytes (audio).
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int64_t nowUs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

static uint32_t fnv(uint32_t h, const uint8_t* p, size_t n) {
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file [mime-prefix] [codec] [max-frames]\n", argv[0]);
        return 2;
    }
    const std::string prefix = argc > 2 ? argv[2] : "video/";
    const char* codecName = argc > 3 && argv[3][0] && strcmp(argv[3], "-") ? argv[3] : nullptr;
    const long maxFrames = argc > 4 ? atol(argv[4]) : 0;

    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror("open"); return 2; }
    AMediaExtractor* ex = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK) {
        fprintf(stderr, "FAIL extractor\n");
        return 3;
    }
    int track = -1;
    AMediaFormat* fmt = nullptr;
    const char* mime = nullptr;
    for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex); i++) {
        AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
        const char* m;
        if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) &&
            !strncmp(m, prefix.c_str(), prefix.size())) {
            track = i; fmt = f; mime = m;
            break;
        }
        AMediaFormat_delete(f);
    }
    if (track < 0) { fprintf(stderr, "FAIL no %s track\n", prefix.c_str()); return 3; }
    AMediaExtractor_selectTrack(ex, track);
    const bool video = !strncmp(mime, "video/", 6);

    AMediaCodec* codec = codecName ? AMediaCodec_createCodecByName(codecName)
                                   : AMediaCodec_createDecoderByType(mime);
    if (!codec) { fprintf(stderr, "FAIL create codec for %s\n", mime); return 4; }
    char* name = nullptr;
    AMediaCodec_getName(codec, &name);
    printf("codec %s mime %s\n", name ? name : "?", mime);
    if (name) AMediaCodec_releaseName(codec, name);
    if (AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0) != AMEDIA_OK) {
        fprintf(stderr, "FAIL configure\n"); return 4;
    }
    if (AMediaCodec_start(codec) != AMEDIA_OK) { fprintf(stderr, "FAIL start\n"); return 4; }

    int32_t width = 0, height = 0, stride = 0, sliceHeight = 0, color = 0;
    bool inEos = false, outEos = false;
    long frames = 0, fed = 0;
    int idle = 0;
    bool stalled = false, failed = false;
    const int64_t t0 = nowUs();
    int64_t lastOut = t0;
    while (!outEos && idle < 200) {
        // A codec that errors before EOS never produces output again: stop after 5 s.
        if (nowUs() - lastOut > 5000000) { stalled = true; break; }
        if (!inEos) {
            ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (ib >= 0) {
                size_t cap;
                uint8_t* buf = AMediaCodec_getInputBuffer(codec, ib, &cap);
                ssize_t n = AMediaExtractor_readSampleData(ex, buf, cap);
                if (n < 0 || (maxFrames && fed >= maxFrames)) {
                    AMediaCodec_queueInputBuffer(codec, ib, 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    inEos = true;
                } else {
                    AMediaCodec_queueInputBuffer(codec, ib, 0, n,
                                                 AMediaExtractor_getSampleTime(ex), 0);
                    AMediaExtractor_advance(ex);
                    fed++;
                }
            }
        }
        AMediaCodecBufferInfo info;
        ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (ob >= 0) {
            idle = 0;
            lastOut = nowUs();
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) outEos = true;
            if (info.size > 0) {
                size_t cap;
                uint8_t* buf = AMediaCodec_getOutputBuffer(codec, ob, &cap) + info.offset;
                uint32_t h = 2166136261u;
                if (video && stride && sliceHeight) {
                    // Visible Y rows, then visible rows of the chroma plane(s).
                    for (int y = 0; y < height; y++) h = fnv(h, buf + y * stride, width);
                    const uint8_t* c = buf + stride * sliceHeight;
                    if (color == 21) {  // NV12: interleaved UV
                        for (int y = 0; y < height / 2; y++) h = fnv(h, c + y * stride, width);
                    } else {            // I420: U then V, half stride
                        for (int p = 0; p < 2; p++)
                            for (int y = 0; y < height / 2; y++)
                                h = fnv(h, c + p * (stride / 2) * (sliceHeight / 2) +
                                               y * (stride / 2), width / 2);
                    }
                } else {
                    h = fnv(h, buf, info.size);
                }
                printf("frame %ld %08x\n", frames, h);
                frames++;
            }
            AMediaCodec_releaseOutputBuffer(codec, ob, false);
        } else if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* of = AMediaCodec_getOutputFormat(codec);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &width);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &height);
            if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_STRIDE, &stride)) stride = width;
            if (!AMediaFormat_getInt32(of, "slice-height", &sliceHeight)) sliceHeight = height;
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color);
            printf("format %s\n", AMediaFormat_toString(of));
            AMediaFormat_delete(of);
        } else if (ob == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            if (inEos) idle++;
        } else if (ob != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            failed = true;  // AMEDIA_ERROR_*: the codec reported an error
            break;
        }
    }
    if (stalled) printf("STALL no output for 5 s\n");
    if (failed) printf("ERROR codec error\n");
    const int64_t us = nowUs() - t0;
    printf("fed %ld decoded %ld %s ms %lld fps*10 %lld\n", fed, frames, outEos ? "eos" : "noeos",
           (long long)(us / 1000), us ? (long long)(frames * 10000000LL / us) : 0LL);
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AMediaFormat_delete(fmt);
    AMediaExtractor_delete(ex);
    close(fd);
    return outEos && frames ? 0 : 5;
}
