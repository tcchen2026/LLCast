#define _POSIX_C_SOURCE 200809L
#include "mpp_decoder.h"
#include "common/llstream_proto.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rk_mpi.h"

struct MppDecoder {
    MppCtx context;
    MppApi *api;
    MppPollType output_timeout;
    bool initialized;
};

_Static_assert(sizeof(MppPollType) == 4, "Vendor MPP timeouts are 32-bit");
_Static_assert(MPP_DEC_SET_DISABLE_ERROR == 0x31000b && sizeof(RK_U32) == 4,
               "Verified vendor error-output control ABI");

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

uint32_t mpp_decoder_codecs(void)
{
    uint32_t result = 0;
    if (!mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingHEVC)) result |= LLS_CODEC_BIT(LLS_CODEC_HEVC);
    if (!mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingAVC)) result |= LLS_CODEC_BIT(LLS_CODEC_H264);
    if (!mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingAV1)) result |= LLS_CODEC_BIT(LLS_CODEC_AV1);
    return result;
}

MppDecoder *mpp_decoder_open(uint8_t codec, unsigned output_timeout_ms)
{
    if (output_timeout_ms > 8000 || (codec < LLS_CODEC_HEVC || codec > LLS_CODEC_AV1)) return NULL;
    MppCodingType coding = codec == LLS_CODEC_AV1 ? MPP_VIDEO_CodingAV1 :
        codec == LLS_CODEC_HEVC ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;
    MppDecoder *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->output_timeout = (MppPollType)output_timeout_ms;
    MPP_RET ret = mpp_check_support_format(MPP_CTX_DEC, coding);
    if (!ret) ret = mpp_create(&d->context, &d->api);
    if (ret || !d->api || d->api->size < offsetof(MppApi, control) + sizeof(d->api->control)) {
        fprintf(stderr, "MPP create/API check failed: %d\n", ret);
        goto fail;
    }
    RK_U32 split = 0, immediate = 1, disable_error = 1;
    MppPollType input_timeout = (MppPollType)20;
    ret = d->api->control(d->context, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);
    if (!ret) ret = mpp_init(d->context, MPP_CTX_DEC, coding);
    if (ret) { fprintf(stderr, "MPP initialization: %d\n", ret); goto fail; }
    d->initialized = true;
    if ((ret = d->api->control(d->context, MPP_SET_INPUT_TIMEOUT, &input_timeout)) ||
        (ret = d->api->control(d->context, MPP_SET_OUTPUT_TIMEOUT, &d->output_timeout)) ||
        (ret = d->api->control(d->context, MPP_DEC_SET_IMMEDIATE_OUT, &immediate)) ||
        (ret = d->api->control(d->context, MPP_DEC_SET_DISABLE_ERROR, &disable_error))) {
        fprintf(stderr, "MPP configuration: %d\n", ret);
        goto fail;
    }
    return d;
fail:
    mpp_decoder_close(d);
    return NULL;
}

bool mpp_decoder_put(MppDecoder *d, const uint8_t *data, size_t size, int64_t pts)
{
    if (!d || !data || !size) return false;
    MppPacket packet = NULL;
    MPP_RET ret = mpp_packet_init(&packet, (void *)data, size);
    if (!ret) {
        mpp_packet_set_pts(packet, pts);
        ret = d->api->decode_put_packet(d->context, packet);
    }
    if (packet) mpp_packet_deinit(&packet);
    if (ret) fprintf(stderr, "MPP put pts=%" PRId64 ": %d\n", pts, ret);
    return ret == MPP_OK;
}

int mpp_decoder_get(MppDecoder *d, MppFrame *frame)
{
    *frame = NULL;
    int result = -1;
    bool timeout_changed = false;
    int64_t deadline = now_ns() + (int64_t)d->output_timeout * 1000000;
    for (;;) {
        MPP_RET ret = d->api->decode_get_frame(d->context, frame);
        if (ret && ret != MPP_ERR_TIMEOUT) {
            fprintf(stderr, "MPP get: %d\n", ret);
            break;
        }
        if (!*frame) { result = 0; break; }
        if (!mpp_frame_get_info_change(*frame)) {
            if (!mpp_frame_get_buffer(*frame) && !mpp_frame_get_errinfo(*frame) &&
                !mpp_frame_get_discard(*frame)) {
                fprintf(stderr, "MPP returned a picture without a buffer\n");
                break;
            }
            result = 1;
            break;
        }
        fprintf(stderr, "MPP info: %ux%u stride=%ux%u format=0x%x\n",
                mpp_frame_get_width(*frame), mpp_frame_get_height(*frame),
                mpp_frame_get_hor_stride(*frame), mpp_frame_get_ver_stride(*frame),
                mpp_frame_get_fmt(*frame));
        mpp_frame_deinit(frame);
        ret = d->api->control(d->context, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        if (ret) { fprintf(stderr, "MPP info-change acknowledgement: %d\n", ret); break; }
        int64_t left = deadline - now_ns();
        if (left <= 0) { result = 0; break; }
        MppPollType remaining = (MppPollType)((left + 999999) / 1000000);
        timeout_changed = true;
        if (d->api->control(d->context, MPP_SET_OUTPUT_TIMEOUT, &remaining)) break;
    }
    if (timeout_changed && d->api->control(d->context, MPP_SET_OUTPUT_TIMEOUT, &d->output_timeout))
        result = -1;
    if (result < 0 && *frame) mpp_frame_deinit(frame);
    return result;
}

bool mpp_decoder_reset(MppDecoder *d)
{
    MPP_RET ret = d->api->reset(d->context);
    if (ret) fprintf(stderr, "MPP reset: %d\n", ret);
    return ret == MPP_OK;
}

bool mpp_decoder_close(MppDecoder *d)
{
    if (!d) return true;
    bool ok = !d->initialized || mpp_decoder_reset(d);
    if (d->context && mpp_destroy(d->context)) ok = false;
    free(d);
    return ok;
}
