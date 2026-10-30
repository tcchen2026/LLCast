#ifndef LLS_AV1_OBU_H
#define LLS_AV1_OBU_H
#include "hevc_au.h"

/* Low-overhead AV1 OBUs (size fields required), single spatial/temporal layer.
 * No IVF or AV1 Annex-B length wrappers. All offsets refer to caller-owned data. */
typedef struct {
    uint8_t type;
    size_t offset, size, payload_offset, payload_size;
} LlsAv1Obu;
typedef struct {
    uint32_t width, height;
    uint8_t av1c[4]; /* AV1CodecConfigurationRecord prefix, no configOBUs */
    bool reduced_still_picture;
} LlsAv1Sequence;

bool lls_av1_next_obu(const uint8_t *data, size_t size, size_t *cursor, LlsAv1Obu *obu);
bool lls_av1_sequence(const uint8_t *payload, size_t size, LlsAv1Sequence *sequence);
/* One temporal delimiter per unit; a KEY is a shown KEY_FRAME with its own
 * valid sequence header. Split frame headers require at least one tile group. */
bool lls_av1_split_units(const uint8_t *data, size_t size, LlsAccessUnit **units, size_t *count);
#endif
