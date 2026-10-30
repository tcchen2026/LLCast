#include "video_decoder.h"
#include "mpp_decoder.h"
#include "mpp_display.h"
#include "common/llstream_proto.h"
static uint32_t codecs(void) { return mpp_decoder_codecs() & LLS_CODEC_BIT(LLS_CODEC_HEVC); }
static void *open_decoder(uint8_t codec, unsigned timeout) { return mpp_decoder_open(codec, timeout); }
static bool put(void *d, const uint8_t *data, size_t size, int64_t pts) { return mpp_decoder_put(d, data, size, pts); }
static int get(void *d, DisplayFrame **out)
{
    MppFrame frame = NULL;
    int result = mpp_decoder_get(d, &frame);
    if (result > 0) {
        *out = mpp_display_frame(&frame);
        if (!*out) result = -1;
    }
    if (frame) mpp_frame_deinit(&frame);
    return result;
}
static bool reset(void *d) { return mpp_decoder_reset(d); }
static bool close_decoder(void *d) { return mpp_decoder_close(d); }
const VideoBackend *llcast_mpp_backend(void)
{
    static const VideoBackend api = {1, codecs, open_decoder, put, get, reset, close_decoder};
    return &api;
}
