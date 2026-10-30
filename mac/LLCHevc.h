// HEVC/H.264 Annex-B and AV1 sized OBUs to VideoToolbox samples/formats.
// AV1 keeps its OBUs (without temporal delimiters) and gets an av1C description.
#pragma once

#include <CoreMedia/CoreMedia.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "llstream_proto.h"

// A growable byte buffer.
typedef struct {
    uint8_t *data;
    size_t size, capacity;
} LLCBytes;

void llc_bytes_free(LLCBytes *bytes);

// The format description from in-band parameter sets (AV1: sequence header),
// or NULL if missing/invalid. The caller releases it. Names predate AV1 support.
CMVideoFormatDescriptionRef llc_video_format_from_annexb(uint8_t codec, const uint8_t *au, size_t size);

// One sample of length-prefixed NAL units (4-byte lengths) from an Annex-B
// access unit, without parameter sets or access unit delimiters. The caller
// releases it. NULL on allocation failure or an empty access unit.
CMSampleBufferRef llc_video_sample_from_annexb(uint8_t codec, const uint8_t *au, size_t size,
                                              CMVideoFormatDescriptionRef format);

// Appends an encoded sample as Annex-B to out (cleared first); with
// parameter_sets, the codec's parameter sets from its format description come first.
bool llc_video_annexb_from_sample(uint8_t codec, CMSampleBufferRef sample, bool parameter_sets, LLCBytes *out);

// True unless the sample is marked not-sync (a keyframe).
bool llc_sample_is_key(CMSampleBufferRef sample);
