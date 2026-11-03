package org.superman.diagnostics.hwui;
import android.view.Surface;
final class DirectBuffer {
    static { System.loadLibrary("superman_direct_buffer"); }
    static native String capture(Surface surface, String filename);
}
