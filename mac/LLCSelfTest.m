#import "LLCSelfTest.h"

#import <Foundation/Foundation.h>
#include <math.h>
#include <unistd.h>

#import "LLCDecoder.h"
#import "LLCEncoder.h"
#include "LLCHevc.h"
#include "hevc_au.h"
#include "lls_net.h"
#include "lls_rx_session.h"
#include "lls_sender.h"

enum { WIDTH = 1920, HEIGHT = 1080, FRAMES = 180 };

// A smooth gradient moving 6 pixels a frame, with a bright moving block.
static uint8_t pattern(int frame, int x, int y)
{
    const int bx = (frame * 12) % (WIDTH - 200), by = 400;
    if (x >= bx && x < bx + 200 && y >= by && y < by + 200) return 220;
    return (uint8_t)(16 + ((x + frame * 6) % WIDTH) * 200 / WIDTH + y * 19 / HEIGHT);
}

static void fill(CVPixelBufferRef pixels, int frame)
{
    CVPixelBufferLockBaseAddress(pixels, 0);
    uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(pixels, 0);
    const size_t luma_stride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
    for (int y = 0; y < HEIGHT; ++y)
        for (int x = 0; x < WIDTH; ++x) luma[y * luma_stride + x] = pattern(frame, x, y);
    uint8_t *chroma = CVPixelBufferGetBaseAddressOfPlane(pixels, 1);
    const size_t chroma_stride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
    for (int y = 0; y < HEIGHT / 2; ++y) memset(chroma + y * chroma_stride, 128, WIDTH);
    CVPixelBufferUnlockBaseAddress(pixels, 0);
}

static double luma_psnr(CVPixelBufferRef picture, int frame)
{
    CVPixelBufferLockBaseAddress(picture, kCVPixelBufferLock_ReadOnly);
    const uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(picture, 0);
    const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(picture, 0);
    double error = 0;
    size_t count = 0;
    for (int y = 0; y < HEIGHT; y += 2)
        for (int x = 0; x < WIDTH; x += 2) {
            const double d = (double)luma[y * stride + x] - pattern(frame, x, y);
            error += d * d;
            ++count;
        }
    CVPixelBufferUnlockBaseAddress(picture, kCVPixelBufferLock_ReadOnly);
    const double mse = error / (double)count;
    return mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99;
}

static int compare(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *values, int count)
{
    if (!count) return 0;
    qsort(values, (size_t)count, sizeof(*values), compare);
    return values[count / 2];
}

static BOOL av1_selftest(void)
{
    // Two red 256x128 frames (key + inter), libaom realtime, lag=0, CRF=10.
    // The fixture needs neither an AV1 encoder nor external files on the Mac.
    static const uint8_t clip[] = {
        0x12,0x00,0x0a,0x0a,0x00,0x00,0x00,0x03,0xb7,0xff,0xf3,0x6b,
        0xe4,0x01,0x32,0x23,0x10,0x00,0x80,0xa0,0x00,0x00,0x80,0x00,
        0x00,0xb4,0x4c,0x3a,0x4a,0x26,0xb5,0xaa,0xa8,0xfa,0x08,0xf9,
        0x44,0xfb,0xfb,0xc5,0xb8,0xc1,0x89,0xf3,0x34,0x7d,0x50,0x02,
        0x4f,0xb1,0xc0,0x12,0x00,0x32,0x15,0x30,0x03,0x80,0x80,0xfd,
        0xf8,0x46,0x85,0x00,0x0a,0x28,0xa2,0x88,0x00,0x00,0x80,0x00,
        0x97,0x82,0x30,0x3c,
    };
    LlsAccessUnit *units = NULL;
    size_t count = 0;
    if (!lls_split_access_units(LLS_CODEC_AV1, clip, sizeof(clip), &units, &count) ||
        count != 2 || !units[0].key || units[1].key) {
        free(units);
        printf("FAIL: AV1 fixture framing\n");
        return NO;
    }
    const BOOL hardware = [LLCDecoder hardwareCodecs] & LLS_CODEC_BIT(LLS_CODEC_AV1);
    CMVideoFormatDescriptionRef format = llc_video_format_from_annexb(LLS_CODEC_AV1, clip, units[0].size);
    CMSampleBufferRef sample = format ? llc_video_sample_from_annexb(LLS_CODEC_AV1, clip, units[0].size, format) : NULL;
    const CMVideoDimensions dimensions = format ? CMVideoFormatDescriptionGetDimensions(format) : (CMVideoDimensions){0};
    uint8_t payload[sizeof(clip)];
    CMBlockBufferRef block = sample ? CMSampleBufferGetDataBuffer(sample) : NULL;
    BOOL ok = format && dimensions.width == 256 && dimensions.height == 128 &&
        CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_AV1 && block &&
        CMBlockBufferGetDataLength(block) == units[0].size - 2 &&
        CMBlockBufferCopyDataBytes(block, 0, units[0].size - 2, payload) == noErr &&
        !memcmp(payload, clip + 2, units[0].size - 2); // only the temporal delimiter is removed
    if (sample) CFRelease(sample);
    if (format) CFRelease(format);
    if (ok && hardware) {
        LLCDecoder *decoder = [LLCDecoder new];
        CVPixelBufferRef picture = NULL;
        // A delta frame cannot initialize a fresh decoder.
        ok = [decoder decode:clip + units[1].offset size:units[1].size key:NO
                       codec:LLS_CODEC_AV1 picture:&picture] == LLCDecodeNeedKey && !picture;
        if (picture) CVPixelBufferRelease(picture);
        for (size_t i = 0; i < count && ok; ++i) {
            picture = NULL;
            ok = [decoder decode:clip + units[i].offset size:units[i].size key:units[i].key
                           codec:LLS_CODEC_AV1 picture:&picture] == LLCDecodePicture && picture;
            if (ok) ok = CVPixelBufferGetWidth(picture) == 256 && CVPixelBufferGetHeight(picture) == 128 &&
                CVPixelBufferGetPixelFormatType(picture) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
                CVPixelBufferGetPlaneCount(picture) == 2 && CVPixelBufferGetIOSurface(picture);
            if (ok) {
                ok = CVPixelBufferLockBaseAddress(picture, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess;
                if (ok) {
                    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(picture, 0);
                    const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(picture, 1);
                    ok = y && uv;
                    for (size_t row = 8; row < 128 && ok; row += 16)
                        for (size_t x = 8; x < 256 && ok; x += 16) {
                            const size_t at = row / 2 * CVPixelBufferGetBytesPerRowOfPlane(picture, 1) + x;
                            ok = abs((int)y[row * CVPixelBufferGetBytesPerRowOfPlane(picture, 0) + x] - 81) <= 3 &&
                                abs((int)uv[at] - 90) <= 3 && abs((int)uv[at + 1] - 240) <= 3;
                        }
                    CVPixelBufferUnlockBaseAddress(picture, kCVPixelBufferLock_ReadOnly);
                }
            }
            if (picture) CVPixelBufferRelease(picture);
        }
    }
    free(units);
    printf("%s: AV1 format/sample conversion; %s\n", ok ? "PASS" : "FAIL",
           hardware ? "hardware key/inter decode and NV12 pixels" : "hardware decode skipped (unavailable)");
    return ok;
}

int LLCRunSelfTest(void)
{
    if (!av1_selftest()) return 1;
    printf("LLCast self-test: HEVC encode -> UDP loopback -> receiver session -> HEVC decode, %d frames\n", FRAMES);
    if (![LLCDecoder hardwareAvailable]) {
        printf("SKIP: no hardware HEVC decoder (virtual machines have none)\n");
        return 77;
    }
    NSError *error = nil;
    LLCEncoder *encoder = [[LLCEncoder alloc] initWithWidth:WIDTH height:HEIGHT bitrate:20000000 error:&error];
    if (!encoder) {
        printf("SKIP: no hardware HEVC encoder (%ld; virtual machines have none)\n", (long)error.code);
        return 77;
    }
    LLCDecoder *decoder = [LLCDecoder new];
    LlsRxConfig rx_config;
    lls_rx_default_config(&rx_config);
    rx_config.port = (uint16_t)(46000 + getpid() % 2000);
    int socket_error = 0;
    LlsRxSession *rx = lls_rx_open(&rx_config, &socket_error);
    LlsSenderConfig config;
    lls_sender_default_config(&config);
    config.codec = LLS_CODEC_HEVC;
    config.host = "127.0.0.1";
    config.port = rx_config.port;
    LlsSender *sender = rx ? lls_sender_open(&config) : NULL;
    if (!rx || !sender) {
        printf("FAIL: loopback sockets (error %d)\n", socket_error);
        return 1;
    }
    NSDictionary *attributes = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey: @(WIDTH), (id)kCVPixelBufferHeightKey: @(HEIGHT),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferPoolRef pool = NULL;
    CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (__bridge CFDictionaryRef)attributes, &pool);
    static int source_of[FRAMES]; // wire frame number -> pattern frame
    static double encode_ms[FRAMES], decode_ms[FRAMES], total_ms[FRAMES];
    int encoded = 0, decoded = 0, keys = 0, failures = 0;
    double worst = 99;
    size_t bytes = 0;
    for (int i = 0; i < FRAMES; ++i) {
        CVPixelBufferRef pixels = NULL;
        if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixels) != kCVReturnSuccess) {
            printf("FAIL: pixel buffer\n");
            return 1;
        }
        fill(pixels, i);
        const uint64_t start = lls_net_now_us();
        LLCEncodedFrame frame;
        const BOOL ok = [encoder encode:pixels key:i == 0 frame:&frame];
        CVPixelBufferRelease(pixels);
        const uint64_t done = lls_net_now_us();
        if (!ok) { ++failures; continue; }
        if (!frame.size) continue; // dropped by rate control
        encode_ms[encoded] = (double)(done - start) / 1000;
        const LlsSenderFrame send = {frame.data, frame.size, frame.key, start, done};
        uint32_t number = 0;
        if (!lls_sender_send(sender, &send, &number) || number >= FRAMES) { ++failures; continue; }
        source_of[number] = i;
        ++encoded;
        keys += frame.key;
        bytes += frame.size;
        bool reset = false;
        LlsRxFrame *received = lls_rx_next(rx, 500, &reset);
        if (!received) continue;
        if (reset) [decoder reset];
        CVPixelBufferRef picture = NULL;
        const uint64_t before = lls_net_now_us();
        const LLCDecodeResult result = [decoder decode:received->data size:received->size key:received->key codec:received->codec
                                               picture:&picture];
        const uint64_t after = lls_net_now_us();
        if (result == LLCDecodePicture) {
            decode_ms[decoded] = (double)(after - before) / 1000;
            total_ms[decoded] = (double)(after - start) / 1000;
            const double psnr = luma_psnr(picture, source_of[received->frame]);
            if (psnr < worst) worst = psnr;
            ++decoded;
            CVPixelBufferRelease(picture);
        }
        lls_rx_finish(rx, received, result == LLCDecodePicture ? LLS_FATE_DELIVERED : LLS_FATE_DECODE_ERROR,
                      (int64_t)after * 1000, 0, 0);
    }
    CVPixelBufferPoolRelease(pool);
    lls_sender_close(sender);
    lls_rx_close(rx);
    const BOOL pass = !failures && encoded >= FRAMES - 3 && decoded == encoded && worst >= 30;
    printf("encoded=%d decoded=%d keys=%d failures=%d mean_frame_bytes=%zu low_latency=%s\n", encoded, decoded,
           keys, failures, encoded ? bytes / (size_t)encoded : 0, encoder.lowLatency ? "yes" : "no");
    printf("median ms: encode=%.2f decode=%.2f encode-start-to-decoded=%.2f; worst luma PSNR %.1f dB\n",
           median(encode_ms, encoded), median(decode_ms, decoded), median(total_ms, decoded), worst);
    if (keys != 1) printf("warning: %d keyframes; only the first was requested\n", keys);
    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
