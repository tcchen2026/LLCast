#include "check.h"
#include "mpp_decoder.h"
#include "common/llstream_proto.h"
#include "rk_mpi.h"

#include <string.h>

/* A fake vendor API checks wrapper contracts; it does not emulate decoding. */
typedef struct {
    const uint8_t *data;
    size_t size;
    int64_t pts;
} Packet;

typedef struct {
    bool info, error, discard, buffer;
    int64_t pts;
} Frame;

static struct {
    bool initialized, split, immediate, disable_error, reset_called, have_packet, info_sent;
    bool av1;
    int input_timeout, output_timeout, resets, destroys, acknowledgements, frames;
    Packet packet;
    int mode;
    MpiCmd fail_control;
} fake;

enum { NORMAL, INFO, TIMEOUT, ERROR_FRAME, DISCARD_FRAME, NO_BUFFER, GET_ERROR, PUT_ERROR };

static MPP_RET control(MppCtx context, MpiCmd command, MppParam param)
{
    CHECK(context == &fake);
    if (command == fake.fail_control) return MPP_NOK;
    int32_t value = 0;
    if (param) memcpy(&value, param, sizeof(value));
    switch (command) {
    case MPP_DEC_SET_PARSER_SPLIT_MODE:
        CHECK(!fake.initialized && value == 0);
        fake.split = true;
        break;
    case MPP_SET_INPUT_TIMEOUT:
        CHECK(fake.initialized);
        fake.input_timeout = value;
        break;
    case MPP_SET_OUTPUT_TIMEOUT:
        CHECK(fake.initialized);
        fake.output_timeout = value;
        break;
    case MPP_DEC_SET_IMMEDIATE_OUT:
        CHECK(fake.initialized && value == 1);
        fake.immediate = true;
        break;
    case MPP_DEC_SET_DISABLE_ERROR:
        CHECK(command == 0x31000b && fake.initialized && value == 1);
        fake.disable_error = true;
        break;
    case MPP_DEC_SET_INFO_CHANGE_READY:
        CHECK(!param && fake.info_sent);
        ++fake.acknowledgements;
        break;
    default: CHECK(false);
    }
    return MPP_OK;
}

static MPP_RET reset(MppCtx context)
{
    CHECK(context == &fake && fake.initialized);
    ++fake.resets;
    fake.reset_called = true;
    fake.have_packet = false;
    return MPP_OK;
}

static MPP_RET put(MppCtx context, MppPacket packet)
{
    CHECK(context == &fake && fake.initialized && fake.immediate && fake.disable_error && fake.input_timeout == 20);
    if (fake.mode == PUT_ERROR) return MPP_ERR_BUFFER_FULL;
    fake.packet = *(Packet *)packet; /* Retain the input bytes after packet deinit. */
    fake.have_packet = true;
    fake.info_sent = false;
    fake.reset_called = false;
    return MPP_OK;
}

static MPP_RET get(MppCtx context, MppFrame *out)
{
    CHECK(context == &fake && fake.initialized);
    *out = NULL;
    if (!fake.have_packet || fake.mode == TIMEOUT) return MPP_ERR_TIMEOUT;
    CHECK(fake.packet.data[0] == 0xa5 && fake.packet.data[fake.packet.size - 1] == 0x5a);
    for (unsigned i = 0; i < 64; ++i) CHECK(!fake.packet.data[fake.packet.size + i]);
    Frame *f = calloc(1, sizeof(*f));
    CHECK(f);
    ++fake.frames;
    *out = f;
    if (fake.mode == INFO && !fake.info_sent) {
        f->info = fake.info_sent = true;
        return MPP_OK;
    }
    f->error = fake.mode == ERROR_FRAME;
    f->discard = fake.mode == DISCARD_FRAME;
    f->buffer = fake.mode != NO_BUFFER;
    f->pts = fake.packet.pts;
    fake.have_packet = false;
    return fake.mode == GET_ERROR ? MPP_NOK : MPP_OK;
}

MPP_RET mpp_create(MppCtx *context, MppApi **out)
{
    static MppApi api = {.size = sizeof(MppApi), .decode_put_packet = put,
                         .decode_get_frame = get, .reset = reset, .control = control};
    *context = &fake;
    *out = &api;
    return MPP_OK;
}

MPP_RET mpp_init(MppCtx context, MppCtxType type, MppCodingType coding)
{
    CHECK(context == &fake && type == MPP_CTX_DEC &&
          (coding == MPP_VIDEO_CodingHEVC || coding == MPP_VIDEO_CodingAVC ||
           (coding == MPP_VIDEO_CodingAV1 && fake.av1)) && fake.split);
    fake.initialized = true;
    return MPP_OK;
}

MPP_RET mpp_check_support_format(MppCtxType type, MppCodingType coding)
{
    CHECK(type == MPP_CTX_DEC && (coding == MPP_VIDEO_CodingHEVC || coding == MPP_VIDEO_CodingAVC || coding == MPP_VIDEO_CodingAV1));
    if (coding == MPP_VIDEO_CodingAV1 && !fake.av1) return MPP_NOK;
    return MPP_OK;
}

MPP_RET mpp_destroy(MppCtx context)
{
    CHECK(context == &fake && (!fake.initialized || fake.reset_called) && !fake.frames);
    ++fake.destroys;
    fake.initialized = false;
    return MPP_OK;
}

MPP_RET mpp_packet_init(MppPacket *out, void *data, size_t size)
{
    Packet *packet = malloc(sizeof(*packet));
    CHECK(packet);
    *packet = (Packet){.data = data, .size = size};
    *out = packet;
    return MPP_OK;
}

MPP_RET mpp_packet_deinit(MppPacket *packet)
{
    free(*packet);
    *packet = NULL;
    return MPP_OK;
}

void mpp_packet_set_pts(MppPacket packet, RK_S64 pts) { ((Packet *)packet)->pts = pts; }
MPP_RET mpp_frame_deinit(MppFrame *frame)
{
    CHECK(*frame && fake.frames > 0);
    --fake.frames;
    free(*frame);
    *frame = NULL;
    return MPP_OK;
}
RK_U32 mpp_frame_get_info_change(const MppFrame frame) { return ((Frame *)frame)->info; }
RK_U32 mpp_frame_get_errinfo(const MppFrame frame) { return ((Frame *)frame)->error; }
RK_U32 mpp_frame_get_discard(const MppFrame frame) { return ((Frame *)frame)->discard; }
RK_S64 mpp_frame_get_pts(const MppFrame frame) { return ((Frame *)frame)->pts; }
MppBuffer mpp_frame_get_buffer(const MppFrame frame) { return ((Frame *)frame)->buffer ? frame : NULL; }
RK_U32 mpp_frame_get_width(const MppFrame frame) { (void)frame; return 1920; }
RK_U32 mpp_frame_get_height(const MppFrame frame) { (void)frame; return 1080; }
RK_U32 mpp_frame_get_hor_stride(const MppFrame frame) { (void)frame; return 1920; }
RK_U32 mpp_frame_get_ver_stride(const MppFrame frame) { (void)frame; return 1088; }
MppFrameFormat mpp_frame_get_fmt(MppFrame frame) { (void)frame; return MPP_FMT_YUV420SP; }

int main(void)
{
    CHECK(mpp_decoder_codecs() == (LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264)));
    CHECK(!mpp_decoder_open(0, 20) && !mpp_decoder_open(LLS_CODEC_AV1, 20));
    MppDecoder *avc = mpp_decoder_open(LLS_CODEC_H264, 20);
    CHECK(avc && mpp_decoder_close(avc));
    fake.av1 = true;
    CHECK(mpp_decoder_codecs() & LLS_CODEC_BIT(LLS_CODEC_AV1));
    MppDecoder *av1 = mpp_decoder_open(LLS_CODEC_AV1, 20);
    CHECK(av1 && mpp_decoder_close(av1));
    memset(&fake, 0, sizeof(fake));
    CHECK(!mpp_decoder_open(LLS_CODEC_HEVC, 8001));
    uint8_t bytes[66] = {0xa5, 0x5a};
    MppDecoder *decoder = mpp_decoder_open(LLS_CODEC_HEVC, 20);
    CHECK(decoder && fake.input_timeout == 20 && fake.output_timeout == 20);
    MppFrame frame = NULL;
    CHECK(mpp_decoder_get(decoder, &frame) == 0 && !frame);
    for (int mode = NORMAL; mode <= GET_ERROR; ++mode) {
        fake.mode = mode;
        CHECK(mpp_decoder_put(decoder, bytes, 2, 123));
        int got = mpp_decoder_get(decoder, &frame);
        if (mode == TIMEOUT) {
            CHECK(got == 0 && !frame && fake.have_packet);
            CHECK(mpp_decoder_reset(decoder) && !fake.have_packet);
        } else if (mode == NO_BUFFER || mode == GET_ERROR) {
            CHECK(got == -1 && !frame && !fake.frames);
        } else {
            CHECK(got == 1 && frame && mpp_frame_get_pts(frame) == 123);
            CHECK(mpp_frame_get_errinfo(frame) == (mode == ERROR_FRAME));
            CHECK(mpp_frame_get_discard(frame) == (mode == DISCARD_FRAME));
            mpp_frame_deinit(&frame);
        }
        CHECK(fake.output_timeout == 20);
    }
    CHECK(fake.acknowledgements == 1);
    fake.mode = PUT_ERROR;
    CHECK(!mpp_decoder_put(decoder, bytes, 2, 456));
    fake.mode = NORMAL;
    CHECK(mpp_decoder_put(decoder, bytes, 2, 789));
    CHECK(mpp_decoder_close(decoder) && !fake.have_packet && fake.destroys == 1);

    memset(&fake, 0, sizeof(fake));
    fake.fail_control = MPP_DEC_SET_IMMEDIATE_OUT;
    CHECK(!mpp_decoder_open(LLS_CODEC_HEVC, 20) && fake.resets == 1 && fake.destroys == 1);
    memset(&fake, 0, sizeof(fake));
    fake.fail_control = MPP_DEC_SET_PARSER_SPLIT_MODE;
    CHECK(!mpp_decoder_open(LLS_CODEC_HEVC, 20) && !fake.resets && fake.destroys == 1);
    puts("MPP wrapper: configuration, info change, frame errors, timeouts and reset-before-destroy verified");
    return 0;
}
