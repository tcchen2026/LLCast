#ifndef LLSTREAM_HEVC_AU_H
#define LLSTREAM_HEVC_AU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "llstream_proto.h"

typedef struct {
    size_t offset, size;
    bool key; /* IDR with parameter sets, or shown AV1 KEY_FRAME with sequence header */
} LlsAccessUnit;
typedef LlsAccessUnit HevcAccessUnit; /* HEVC benchmark/source compatibility */

/* Annex-B HEVC/H.264 or AV1 temporal units with sized OBUs and delimiters.
 * Unsupported codecs are rejected. Ownership below. */
bool lls_split_access_units(uint8_t codec, const uint8_t *data, size_t size,
                            LlsAccessUnit **units, size_t *count);

/* Single-layer Annex-B HEVC, with the same splitting rules and diagnostics
 * as hevc_decode_test. Pass *units = NULL and *count = 0. The caller must
 * free(*units), also on failure (a partial list may have been allocated).
 * Offsets refer to data, which remains owned by the caller. */
bool hevc_split_access_units(const uint8_t *data, size_t size,
                             HevcAccessUnit **units, size_t *count);

#endif
