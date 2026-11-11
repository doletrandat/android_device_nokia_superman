// Parses media_profiles through libmedia's MediaProfiles (the same code zygote uses) in a
// throwaway process: run with media.settings.xml pointing at the candidate file.
#include <media/MediaProfiles.h>
#include <stdio.h>
using namespace android;
int main() {
    MediaProfiles *p = MediaProfiles::getInstance();
    if (!p) { printf("no instance\n"); return 1; }
    const camcorder_quality q[] = {CAMCORDER_QUALITY_HIGH, CAMCORDER_QUALITY_LOW, CAMCORDER_QUALITY_720P,
                                   CAMCORDER_QUALITY_480P, CAMCORDER_QUALITY_QCIF};
    for (auto x : q)
        printf("quality %d: has=%d w=%d h=%d fps=%d\n", x, p->hasCamcorderProfile(0, x),
               p->getCamcorderProfileParamByName("vid.width", 0, x),
               p->getCamcorderProfileParamByName("vid.height", 0, x),
               p->getCamcorderProfileParamByName("vid.fps", 0, x));
    printf("enc.vid.width.max h264=%d\n", p->getVideoEncoderParamByName("enc.vid.width.max", VIDEO_ENCODER_H264));
    printf("OK\n");
    return 0;
}
