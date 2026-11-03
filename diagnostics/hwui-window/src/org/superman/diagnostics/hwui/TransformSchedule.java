package org.superman.diagnostics.hwui;

/** Pure schedule, shared by the device workload and host bounds tests. */
final class TransformSchedule {
    static final int FRAMES = 96;
    static float[] pose(int stage, int frame, int index, int width, int height) {
        if (stage < 0 || stage > 3 || frame < 0 || frame >= FRAMES || index < 0 || index > 7
                || width <= 0 || height <= 0) throw new IllegalArgumentException("schedule");
        float wave = (float)Math.sin(frame * 4.0 * Math.PI / (FRAMES - 1));
        float direction = index % 2 == 0 ? 1 : -1;
        float x=0, y=0, scale=1, alpha=1, rotation=0, scroll=0;
        if (stage == 0) scroll = 64 + 64 * wave;
        if (stage == 1) { x=direction*(32+width*.12f*wave); scale=.875f+.10f*wave; }
        if (stage == 2) {
            x=direction*width*(.20f+.12f*wave); y=-(index/2)*height*.16f;
            scale=.90f+.05f*wave; alpha=.75f+.15f*wave;
        }
        if (stage == 3) { rotation=direction*8*wave; y=height*.15f*wave; }
        // Explicit endpoints avoid floating-point residuals in identity control.
        if (frame == FRAMES-1) {
            if (stage == 0) scroll=64;
            if (stage == 1) { x=direction*32; scale=.875f; }
            if (stage == 2) { x=direction*width*.20f; scale=.90f; alpha=.75f; }
            if (stage == 3) { x=0; y=0; scale=1; alpha=1; rotation=0; scroll=0; }
        }
        return new float[]{x,y,scale,alpha,rotation,scroll};
    }
}
