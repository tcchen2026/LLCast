package org.llcast;

/** Largest aligned screen fit within a profile and an encoder's size/rate limits. */
final class EncoderSize {
    interface Supported { boolean contains(int width, int height); }

    private EncoderSize() {}

    static int[] fit(int screenWidth, int screenHeight, int modeWidth, int modeHeight,
                     int minWidth, int minHeight, int maxWidth, int maxHeight,
                     int alignWidth, int alignHeight, Supported supported) {
        if (screenWidth <= 0 || screenHeight <= 0 || modeWidth <= 0 || modeHeight <= 0 ||
            minWidth <= 0 || minHeight <= 0 || maxWidth < minWidth || maxHeight < minHeight ||
            alignWidth <= 0 || alignHeight <= 0) return null;
        double scale = Math.min(1.0, Math.min(
            (double) Math.min(modeWidth, maxWidth) / screenWidth,
            (double) Math.min(modeHeight, maxHeight) / screenHeight));
        int width = (int) (screenWidth * scale) / alignWidth * alignWidth;
        int height = (int) (screenHeight * scale) / alignHeight * alignHeight;
        if (width < minWidth || height < minHeight) return null;
        if (supported.contains(width, height)) return new int[]{width, height};
        // Keep the requested fps. A small size reduction can cross a hardware
        // block-rate limit; test aligned fits without assuming support is monotonic.
        for (height -= alignHeight; height >= minHeight; height -= alignHeight) {
            width = (int) ((double) height * screenWidth / screenHeight) / alignWidth * alignWidth;
            if (width < minWidth) break;
            if (supported.contains(width, height)) return new int[]{width, height};
        }
        return null;
    }
}
