/* libllcast.so: the Android sender. MediaProjection draws the screen into the
 * hardware encoder's input surface (no copy). Async callbacks enqueue buffer
 * indices; a worker sends AUs and releases buffers outside the codec's looper.
 * A receiver's RECOVER wakes that worker to request a sync frame. Static screens
 * keep the profile's frame rate through repeat-previous-frame-after. */
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "av1_obu.h"
#include "lls_net.h"
#include "lls_probe.h"
#include "lls_sender.h"
#include "lls_stream_profile.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "LLCast", __VA_ARGS__)
#define COLOR_FORMAT_SURFACE 0x7F000789 /* MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface */
#define BITRATE_MODE_CBR 2
#define OUTPUT_SLOTS 64

typedef struct {
    int32_t index;
    AMediaCodecBufferInfo info;
} EncodedBuffer;

typedef struct {
    LlsSender *sender;
    AMediaCodec *codec;
    ANativeWindow *window;
    uint8_t codec_type;
    pthread_mutex_t lock;        /* callbacks enqueue; no codec/network calls under this lock */
    pthread_cond_t wake;
    pthread_t worker;
    bool closing, worker_started, codec_started, sync_requested;
    EncodedBuffer output[OUTPUT_SLOTS];
    unsigned output_head, output_count;
    uint8_t *config;             /* the codec's parameter sets, for IDRs that lack them */
    size_t config_size;
    uint8_t *partial;            /* an AU split over several output buffers */
    size_t partial_size, partial_capacity;
    atomic_uint_fast64_t frames;
    atomic_bool failed, logged_clock;
} Sender;

/* AV1: true if the OBUs include a sequence header. *delimiter: the size of a leading
 * temporal delimiter OBU, or 0. */
static bool av1_has_sequence(const uint8_t *data, size_t size, size_t *delimiter)
{
    size_t cursor = 0;
    LlsAv1Obu obu;
    bool sequence = false, first = true;
    *delimiter = 0;
    while (cursor < size && lls_av1_next_obu(data, size, &cursor, &obu)) {
        if (first && obu.type == 2) *delimiter = obu.size;
        first = false;
        sequence |= obu.type == 1;
    }
    return sequence;
}

/* True if the Annex-B AU carries the codec's parameter sets (HEVC VPS/SPS/PPS, H.264 SPS/PPS). */
static bool has_parameter_sets(const uint8_t *data, size_t size, uint8_t codec)
{
    unsigned found = 0;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] || data[i + 1] || data[i + 2] != 1) continue;
        const uint8_t header = data[i + 3];
        const unsigned type = codec == LLS_CODEC_H264 ? header & 31u : (header >> 1) & 63u;
        if (codec == LLS_CODEC_H264 && (type == 7 || type == 8)) found |= 1u << (type - 7);
        if (codec != LLS_CODEC_H264 && type >= 32 && type <= 34) found |= 1u << (type - 32);
        i += 2;
    }
    return found == (codec == LLS_CODEC_H264 ? 3u : 7u);
}

static bool append(Sender *s, const uint8_t *data, size_t size)
{
    if (s->partial_size + size > s->partial_capacity) {
        size_t capacity = (s->partial_size + size) * 2;
        uint8_t *grown = realloc(s->partial, capacity);
        if (!grown) return false;
        s->partial = grown;
        s->partial_capacity = capacity;
    }
    memcpy(s->partial + s->partial_size, data, size);
    s->partial_size += size;
    return true;
}

static void process_output(Sender *s, const EncodedBuffer *event)
{
    AMediaCodec *codec = s->codec;
    const int32_t index = event->index;
    const AMediaCodecBufferInfo *info = &event->info;
    media_status_t status;
    size_t capacity = 0;
    uint8_t *buffer = AMediaCodec_getOutputBuffer(codec, (size_t)index, &capacity);
    if (buffer && info->size > 0 && info->offset >= 0 && (size_t)info->offset <= capacity &&
        (size_t)info->size <= capacity - (size_t)info->offset) {
        const uint8_t *data = buffer + info->offset;
        const size_t size = (size_t)info->size;
        if (info->flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
            const uint8_t *config = data;
            size_t config_size = size;
            if (s->codec_type == LLS_CODEC_AV1 && size > 4 && data[0] == 0x81) { /* av1C: marker, version 1 */
                config += 4;
                config_size -= 4;
            }
            uint8_t *copy = malloc(config_size);
            if (copy) {
                memcpy(copy, config, config_size);
                free(s->config);
                s->config = copy;
                s->config_size = config_size;
            }
        } else if (info->flags & AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME) {
            if (!append(s, data, size)) atomic_store(&s->failed, true);
        } else {
            const bool key = info->flags & 1; /* BUFFER_FLAG_KEY_FRAME */
            const uint8_t *au = data;
            size_t au_size = size;
            if (s->partial_size) { /* the last piece of a split AU */
                if (!append(s, data, size)) { atomic_store(&s->failed, true); goto release; }
                au = s->partial;
                au_size = s->partial_size;
            }
            /* Receivers start at an IDR only if it carries the parameter sets. */
            uint8_t *joined = NULL;
            if (s->codec_type == LLS_CODEC_AV1) {
                /* A temporal unit starts with a temporal delimiter; a key frame carries its
                 * sequence header. MediaCodec may leave either out. */
                size_t delimiter = 0;
                const bool sequence = av1_has_sequence(au, au_size, &delimiter);
                const bool add_sequence = key && !sequence && s->config;
                if (!delimiter || add_sequence) {
                    joined = malloc(au_size + 2 + (add_sequence ? s->config_size : 0));
                    if (!joined) { atomic_store(&s->failed, true); goto release; }
                    uint8_t *w = joined;
                    *w++ = 0x12; /* OBU_TEMPORAL_DELIMITER with a size field */
                    *w++ = 0x00;
                    if (add_sequence) { memcpy(w, s->config, s->config_size); w += s->config_size; }
                    memcpy(w, au + delimiter, au_size - delimiter);
                    au_size = (size_t)(w - joined) + au_size - delimiter;
                    au = joined;
                }
                if (key) {
                    LlsAccessUnit *units = NULL;
                    size_t count = 0;
                    const bool valid = lls_split_access_units(LLS_CODEC_AV1, au, au_size, &units, &count) &&
                                       count == 1 && units[0].key;
                    free(units);
                    if (!valid) {
                        LOG("The AV1 encoder's key frame isn't a complete temporal unit");
                        atomic_store(&s->failed, true);
                        free(joined);
                        goto release;
                    }
                }
            } else if (key && s->config && !has_parameter_sets(au, au_size, s->codec_type)) {
                joined = malloc(s->config_size + au_size);
                if (!joined) { atomic_store(&s->failed, true); goto release; }
                memcpy(joined, s->config, s->config_size);
                memcpy(joined + s->config_size, au, au_size);
                au = joined;
                au_size += s->config_size;
            }
            const uint64_t now = lls_net_now_us();
            /* presentationTimeUs: the virtual display's frame time. Both are CLOCK_MONOTONIC. */
            const uint64_t capture = info->presentationTimeUs > 0 ? (uint64_t)info->presentationTimeUs : now;
            if (!atomic_exchange(&s->logged_clock, true))
                LOG("First frame: now - presentation = %lld us", (long long)(now - capture));
            LlsSenderFrame frame = {au, au_size, key, capture, now};
            uint32_t number = 0;
            if (!lls_sender_send(s->sender, &frame, &number)) atomic_store(&s->failed, true);
            else atomic_fetch_add(&s->frames, 1);
            free(joined);
            s->partial_size = 0;
        }
    } else if (info->size > 0) {
        LOG("Encoder supplied an invalid output buffer");
        atomic_store(&s->failed, true);
    }
release:
    status = AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
    if (status != AMEDIA_OK) {
        LOG("Releasing encoder output failed (%d)", status);
        atomic_store(&s->failed, true);
    }
}

static void on_output(AMediaCodec *codec, void *opaque, int32_t index, AMediaCodecBufferInfo *info)
{
    (void)codec;
    Sender *s = opaque;
    pthread_mutex_lock(&s->lock);
    if (!s->closing) {
        if (index < 0 || s->output_count == OUTPUT_SLOTS) {
            LOG("Encoder output queue overflow or invalid index");
            atomic_store(&s->failed, true);
        } else {
            unsigned tail = (s->output_head + s->output_count++) % OUTPUT_SLOTS;
            s->output[tail] = (EncodedBuffer){index, *info};
            pthread_cond_signal(&s->wake);
        }
    }
    pthread_mutex_unlock(&s->lock);
}

static void on_input(AMediaCodec *codec, void *opaque, int32_t index) { (void)codec, (void)opaque, (void)index; }

static void on_format(AMediaCodec *codec, void *opaque, AMediaFormat *format)
{
    (void)codec, (void)opaque;
    LOG("Encoder output: %s", AMediaFormat_toString(format));
}

static void on_error(AMediaCodec *codec, void *opaque, media_status_t error, int32_t code, const char *detail)
{
    (void)codec;
    Sender *s = opaque;
    LOG("Encoder error %d/%d: %s", error, code, detail ? detail : "");
    atomic_store(&s->failed, true);
}

/* A new RECOVER may arrive on either the send or receive thread. Never make
 * synchronous codec calls from its callback, including from output delivery. */
static void on_recover(void *opaque)
{
    Sender *s = opaque;
    pthread_mutex_lock(&s->lock);
    if (!s->closing) {
        s->sync_requested = true;
        pthread_cond_signal(&s->wake);
    }
    pthread_mutex_unlock(&s->lock);
}

static void *output_worker(void *opaque)
{
    Sender *s = opaque;
    pthread_setname_np(pthread_self(), "llcast-encode");
    for (;;) {
        pthread_mutex_lock(&s->lock);
        while (!s->closing && !s->output_count && !s->sync_requested)
            pthread_cond_wait(&s->wake, &s->lock);
        if (s->closing) { pthread_mutex_unlock(&s->lock); break; }
        bool sync = s->sync_requested;
        s->sync_requested = false;
        bool have_output = s->output_count != 0;
        EncodedBuffer event = {0};
        if (have_output) {
            event = s->output[s->output_head];
            s->output_head = (s->output_head + 1) % OUTPUT_SLOTS;
            --s->output_count;
        }
        pthread_mutex_unlock(&s->lock);
        if (sync) {
            AMediaFormat *request = AMediaFormat_new();
            if (request) {
                AMediaFormat_setInt32(request, "request-sync", 0);
                AMediaCodec_setParameters(s->codec, request);
                AMediaFormat_delete(request);
            }
        }
        if (have_output) process_output(s, &event);
    }
    return NULL;
}

static void close_sender(JNIEnv *env, Sender *s)
{
    (void)env;
    pthread_mutex_lock(&s->lock);
    s->closing = true;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    /* The worker finishes its current output with both codec and socket alive.
     * Stopping the codec then releases any queued, unprocessed output buffers. */
    if (s->worker_started) pthread_join(s->worker, NULL);
    if (s->sender) lls_sender_close(s->sender);
    if (s->codec) {
        if (s->codec_started) AMediaCodec_stop(s->codec);
        AMediaCodec_delete(s->codec);
    }
    if (s->window) ANativeWindow_release(s->window);
    pthread_cond_destroy(&s->wake);
    pthread_mutex_destroy(&s->lock);
    free(s->config);
    free(s->partial);
    free(s);
}

/* LLS_PROBE_* in out[0], then reply time (us), the receiver's codec set and errno. */
JNIEXPORT void JNICALL Java_org_llcast_Native_probe(JNIEnv *env, jclass type, jstring host, jint port, jlongArray out)
{
    (void)type;
    const char *name = (*env)->GetStringUTFChars(env, host, NULL);
    uint32_t rtt = 0, codecs = 0;
    int error = 0;
    LlsProbeResult result = name ? lls_probe_receiver(name, (uint16_t)port, &rtt, &codecs, &error) : LLS_PROBE_FAILED;
    if (name) (*env)->ReleaseStringUTFChars(env, host, name);
    const jlong values[4] = {result, rtt, codecs, error};
    (*env)->SetLongArrayRegion(env, out, 0, 4, values);
}

JNIEXPORT jint JNICALL Java_org_llcast_Native_autoBudget(JNIEnv *env, jclass type, jint width, jint height,
                                                         jint fps, jint codec)
{
    (void)env; (void)type;
    const LlsStreamProfile profile = {(uint32_t)width, (uint32_t)height, (uint32_t)fps};
    return lls_stream_profile_valid(&profile) ? (jint)lls_auto_budget_bps(&profile, (uint8_t)codec) : 0;
}

/* budget: video + FEC (user decision: 10% parity, paid from the budget). */
JNIEXPORT jlong JNICALL Java_org_llcast_Native_senderOpen(JNIEnv *env, jclass type, jstring host, jint port,
    jint codec, jstring encoder, jint width, jint height, jint fps, jint budget)
{
    enum { FEC_PERCENT = 10 };
    const jint bitrate = (jint)((int64_t)budget * 100 / (100 + FEC_PERCENT));
    (void)type;
    Sender *s = calloc(1, sizeof(*s));
    if (!s) return 0;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    s->codec_type = (uint8_t)codec;
    const char *name = (*env)->GetStringUTFChars(env, encoder, NULL);
    const char *address = (*env)->GetStringUTFChars(env, host, NULL);
    if (!name || !address) goto failed;
    LlsSenderConfig config;
    lls_sender_default_config(&config);
    config.host = address;
    config.port = (uint16_t)port;
    config.codec = (uint8_t)codec;
    config.qos = true;
    config.fec_percent = FEC_PERCENT;
    config.budget_bps = (uint32_t)budget;
    config.fps = (uint32_t)fps;
    config.on_recover = on_recover;
    config.recover_context = s;
    s->codec = AMediaCodec_createCodecByName(name);
    if (!s->codec) goto failed;
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME,
        codec == LLS_CODEC_H264 ? "video/avc" : codec == LLS_CODEC_AV1 ? "video/av01" : "video/hevc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FORMAT_SURFACE);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BITRATE_MODE, BITRATE_MODE_CBR);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setFloat(format, "max-fps-to-encoder", (float)fps); /* the profile's rate, not the screen's */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 3600); /* keyframes on request */
    /* H.264 High; HEVC Main; AV1 Main 8-bit (AV1ProfileMain8). */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE, codec == LLS_CODEC_H264 ? 8 : 1);
    AMediaFormat_setInt32(format, "max-bframes", 0);
    AMediaFormat_setInt32(format, "latency", 1);
    AMediaFormat_setInt32(format, "priority", 0);
    AMediaFormat_setInt32(format, "prepend-sps-pps-to-idr-frames", 1);
    AMediaFormat_setInt64(format, "repeat-previous-frame-after", 1000000 / fps + 1000);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, 1 /* BT.709 */);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, 2 /* limited */);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, 3 /* SDR video */);
    media_status_t status = AMediaCodec_configure(s->codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (status != AMEDIA_OK) { LOG("%s configure failed (%d)", name, status); goto failed; }
    if (AMediaCodec_createInputSurface(s->codec, &s->window) != AMEDIA_OK) goto failed;
    AMediaCodecOnAsyncNotifyCallback callbacks = {on_input, on_output, on_format, on_error};
    if (AMediaCodec_setAsyncNotifyCallback(s->codec, callbacks, s) != AMEDIA_OK) goto failed;
    s->sender = lls_sender_open(&config);
    if (!s->sender) goto failed;
    if (AMediaCodec_start(s->codec) != AMEDIA_OK) goto failed;
    s->codec_started = true;
    if (pthread_create(&s->worker, NULL, output_worker, s)) goto failed;
    s->worker_started = true;
    LOG("Sending %dx%d at %d fps %s, %d bps video in a %d bps budget (%d%% FEC), with %s", width, height, fps,
        lls_codec_name((uint8_t)codec), bitrate, budget, FEC_PERCENT, name);
    (*env)->ReleaseStringUTFChars(env, encoder, name);
    (*env)->ReleaseStringUTFChars(env, host, address);
    return (jlong)(intptr_t)s;
failed:
    if (name) (*env)->ReleaseStringUTFChars(env, encoder, name);
    if (address) (*env)->ReleaseStringUTFChars(env, host, address);
    close_sender(env, s);
    return 0;
}

/* The encoder's input surface, for MediaProjection's virtual display. */
JNIEXPORT jobject JNICALL Java_org_llcast_Native_senderSurface(JNIEnv *env, jclass type, jlong handle)
{
    (void)type;
    Sender *s = (Sender *)(intptr_t)handle;
    return ANativeWindow_toSurface(env, s->window);
}

/* frames sent, bytes sent, receiver reply time (us, or -1), failed (0/1), the receiver's codec set. */
JNIEXPORT void JNICALL Java_org_llcast_Native_senderStatus(JNIEnv *env, jclass type, jlong handle, jlongArray out)
{
    (void)type;
    Sender *s = (Sender *)(intptr_t)handle;
    int64_t offset = 0;
    uint32_t rtt = 0;
    const bool clock = lls_sender_clock(s->sender, &offset, &rtt);
    LlsQuality q;
    if (!lls_sender_quality(s->sender, &q)) q = (LlsQuality){0};
    // 5..15: the receiver's reception (level 0: none yet): level, fps x100, packets received % x100
    // (rounded down), frames corrupt % x100 (up), latency ms, FEC repairs, too late, the doctor's
    // hint (LLS_HINT_*), suggested budget in 0.1 Mbps, frames sent per second x100, profile fps.
    // 16..17: minimum latency ms, median is estimated (0/1). Slot 9 is the median.
    const jlong values[18] = {(jlong)atomic_load(&s->frames), (jlong)lls_sender_sent_bytes(s->sender),
                              clock ? (jlong)rtt : -1, atomic_load(&s->failed), lls_sender_peer_codecs(s->sender),
                              q.level, llround(q.fps * 100), (jlong)floor(q.received_percent * 100),
                              (jlong)ceil(q.corrupt_percent * 100), llround(q.latency_ms), q.repaired, q.late,
                              q.hint, q.suggested_budget_bps / 100000, llround(q.sent_fps * 100), q.profile_fps,
                              llround(q.latency_min_ms), q.latency_sampled};
    (*env)->SetLongArrayRegion(env, out, 0, 18, values);
}

JNIEXPORT void JNICALL Java_org_llcast_Native_senderClose(JNIEnv *env, jclass type, jlong handle)
{
    (void)type;
    close_sender(env, (Sender *)(intptr_t)handle);
}
