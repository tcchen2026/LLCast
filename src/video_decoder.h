#ifndef LLCAST_VIDEO_DECODER_H
#define LLCAST_VIDEO_DECODER_H
#include "display_frame.h"
typedef struct VideoDecoder VideoDecoder;
uint32_t video_decoder_codecs(void);
VideoDecoder *video_decoder_open(uint8_t codec, unsigned timeout_ms);
bool video_decoder_put(VideoDecoder *decoder, const uint8_t *data, size_t size, int64_t pts);
/* 1: owned picture, 0: MPP wait timed out, -1: decoder failure. */
int video_decoder_get(VideoDecoder *decoder, DisplayFrame **frame);
bool video_decoder_reset(VideoDecoder *decoder);
bool video_decoder_close(VideoDecoder *decoder);

/* Private, versioned boundary to the optional Rockchip plugin. The receiver
 * has no libmpp dependency: tablets can start and advertise dav1d AV1 alone. */
typedef struct {
    uint32_t version;
    uint32_t (*codecs)(void);
    void *(*open)(uint8_t, unsigned);
    bool (*put)(void *, const uint8_t *, size_t, int64_t);
    int (*get)(void *, DisplayFrame **);
    bool (*reset)(void *);
    bool (*close)(void *);
} VideoBackend;
const VideoBackend *llcast_mpp_backend(void);
#endif
