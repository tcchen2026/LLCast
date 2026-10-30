package org.llcast;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;
import java.io.File;
import java.util.Locale;

/** Which video hardware this device has, asked without root. */
final class Codecs {
    static final int LLS_HEVC = 1, LLS_H264 = 2, LLS_AV1 = 3; // LLS_CODEC_* (llstream_proto.h)
    static final String HEVC = "video/hevc";
    static final String AVC = "video/avc";
    static final String AV1 = "video/av01";

    private Codecs() {}

    /** The first hardware decoder for the type, or null. */
    static MediaCodecInfo hardware(String type) {
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() || !hardware(info)) continue;
            for (String supported : info.getSupportedTypes())
                if (supported.equalsIgnoreCase(type)) return info;
        }
        return null;
    }

    private static boolean hardware(MediaCodecInfo info) {
        if (Build.VERSION.SDK_INT >= 29) return info.isHardwareAccelerated() && !info.isAlias();
        String name = info.getName().toLowerCase(Locale.ROOT);
        return !name.startsWith("omx.google.") && !name.startsWith("c2.android.") && !name.contains(".sw.");
    }

    static boolean hasEncoder(String type) {
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (!info.isEncoder() || !hardware(info)) continue;
            for (String supported : info.getSupportedTypes())
                if (supported.equalsIgnoreCase(type)) return true;
        }
        return false;
    }

    /** A CBR surface encoder and the largest supported screen fit at the requested fps.
     *  Never enlarge the screen or exceed the profile. Null when no fit is supported. */
    static Encoder encoder(String type, int screenWidth, int screenHeight, int modeWidth, int modeHeight, int fps) {
        Encoder best = null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (!info.isEncoder() || !hardware(info)) continue;
            for (String supported : info.getSupportedTypes()) {
                if (!supported.equalsIgnoreCase(type)) continue;
                MediaCodecInfo.CodecCapabilities capabilities = info.getCapabilitiesForType(type);
                MediaCodecInfo.VideoCapabilities video = capabilities.getVideoCapabilities();
                MediaCodecInfo.EncoderCapabilities encoder = capabilities.getEncoderCapabilities();
                boolean surface = false;
                for (int color : capabilities.colorFormats)
                    surface |= color == MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface;
                if (video == null || encoder == null ||
                    !surface || !encoder.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) continue;
                int[] size = EncoderSize.fit(screenWidth, screenHeight, modeWidth, modeHeight,
                    video.getSupportedWidths().getLower(), video.getSupportedHeights().getLower(),
                    video.getSupportedWidths().getUpper(), video.getSupportedHeights().getUpper(),
                    video.getWidthAlignment(), video.getHeightAlignment(),
                    (width, height) -> video.areSizeAndRateSupported(width, height, fps));
                if (size != null && (best == null || (long) size[0] * size[1] > (long) best.width * best.height))
                    best = new Encoder(info.getName(), size[0], size[1]);
            }
        }
        return best;
    }

    static final class Encoder {
        final String name;
        final int width, height;
        Encoder(String name, int width, int height) { this.name = name; this.width = width; this.height = height; }
    }

    static boolean lowLatency(MediaCodecInfo info, String type) {
        return Build.VERSION.SDK_INT >= 30 && info.getCapabilitiesForType(type)
            .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency);
    }

    /** A 64-bit Rockchip device with the vendor MPP library: Extreme mode's root checks decide the rest. */
    static boolean rockchip() {
        return android.os.Process.is64Bit() && new File("/vendor/lib64/libmpp.so").exists();
    }
}
