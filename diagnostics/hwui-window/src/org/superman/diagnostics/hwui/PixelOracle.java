package org.superman.diagnostics.hwui;

/** Independent of Android, so the actual tolerance predicate is host-testable. */
final class PixelOracle {
    static boolean near(int actual, int expected, int tolerance) {
        if (tolerance < 0 || tolerance > 255) throw new IllegalArgumentException("tolerance");
        for (int shift = 0; shift < 32; shift += 8)
            if (Math.abs(((actual >>> shift) & 255) - ((expected >>> shift) & 255)) > tolerance)
                return false;
        return true;
    }
}
