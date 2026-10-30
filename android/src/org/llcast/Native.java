package org.llcast;

import android.view.Surface;

/** libllcast.so: the portable C receiver and sender through JNI. */
final class Native {
    static { System.loadLibrary("llcast"); }

    private Native() {}

    /** Called from a native thread when the decoded picture's visible size changes. */
    interface VideoSize { void onVideoSize(int width, int height); }

    /** Native dav1d fallback for 8-bit AV1 when there is no hardware AV1 decoder. */
    static native boolean softwareAv1();

    /** Binds the port and starts decoding to the surface with the named hardware decoders (null when
     *  absent; AV1 then uses dav1d when bundled). Returns 0 on failure, errno in error[0]. */
    static native long receiverOpen(Surface surface, int port, String hevcDecoder, boolean hevcLowLatency,
                                    String avcDecoder, boolean avcLowLatency, String av1Decoder, boolean av1LowLatency,
                                    VideoSize listener, int[] error);
    /** status: receiving (0/1), frames handed to the decoder, frames shown, decoder failed (0/1),
     *  the current sender's codec when this device can't decode it (LLS_CODEC_*, else 0). */
    static native void receiverStatus(long receiver, long[] status);
    static native void receiverClose(long receiver);

    /** Asks the receiver whether it is there. out: LLS_PROBE_* result (0 answered, 1 silent, 2 refused,
     *  3 unreachable, 4 failed), reply time in us, its LLS_CODEC_BIT set, errno. About a second at most. */
    static native void probe(String host, int port, long[] out);
    /** The hardware encoder and the sender session. Returns 0 on failure. */
    /** budget: video + FEC in bit/s; the encoder gets the share after FEC. */
    static native long senderOpen(String host, int port, int codec, String encoder, int width, int height,
                                  int fps, int budget);
    /** The Auto bandwidth budget (bit/s) for a profile and codec (0: Auto, as HEVC), or 0 if invalid. */
    static native int autoBudget(int width, int height, int fps, int codec);
    /** The encoder's input surface, for the virtual display. */
    static native Surface senderSurface(long sender);
    /** out: 18 longs. First five: frames, bytes, reply time us (-1 before the first), failed (0/1),
     *  receiver's codec set. Reception follows: median latency at 9, minimum at 16,
     *  estimated median flag at 17. */
    static native void senderStatus(long sender, long[] out);
    static native void senderClose(long sender);
}
