#ifndef LLSTREAM_MPP_DECODER_H
#define LLSTREAM_MPP_DECODER_H

/* Low-delay HEVC/H.264/AV1 decoding with the box's vendor MPP: complete access units,
 * parser split mode 0, immediate output, MPP-internal frame buffers. Use from
 * one thread. Encodes the invariants hevc_decode_test established on this
 * firmware, including reset() before mpp_destroy(). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mpp_frame.h"

typedef struct MppDecoder MppDecoder;

/* LLS_CODEC_BIT set checked against the installed vendor library. */
uint32_t mpp_decoder_codecs(void);
MppDecoder *mpp_decoder_open(uint8_t codec, unsigned output_timeout_ms);
/* Submits one complete AU, which must be followed by 64 readable bytes. The
 * bytes must stay valid until the picture with this pts has been returned,
 * or until reset/close. Returns false on a decoder error. */
bool mpp_decoder_put(MppDecoder *decoder, const uint8_t *data, size_t size, int64_t pts);
/* Waits up to the output timeout for a decoded picture and acknowledges info
 * changes internally. Returns 1 with *frame set (the caller owns it), 0 on
 * timeout, -1 on error. Frames flagged with errinfo or discard are still
 * returned; the caller decides. */
int mpp_decoder_get(MppDecoder *decoder, MppFrame *frame);
/* Drops queued input and reference pictures, e.g. for a new session. */
bool mpp_decoder_reset(MppDecoder *decoder);
/* Resets, then destroys. Release display references to its frames first. */
bool mpp_decoder_close(MppDecoder *decoder);

#endif
