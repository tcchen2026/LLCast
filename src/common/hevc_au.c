#include "hevc_au.h"
#include "av1_obu.h"

#include <stdio.h>
#include <stdlib.h>

static size_t start_code(const uint8_t *data, size_t size, size_t from, size_t *prefix)
{
    for (size_t i = from; i + 3 <= size; ++i) {
        if (data[i] || data[i + 1]) continue;
        if (data[i + 2] == 1) { *prefix = 3; return i; }
        if (i + 4 <= size && !data[i + 2] && data[i + 3] == 1) {
            *prefix = 4;
            return i;
        }
    }
    return size;
}

static bool append_au(HevcAccessUnit **units, size_t *count, size_t *capacity,
                      size_t offset, size_t size, bool key)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity ? *capacity * 2 : 256;
        if (new_capacity > 1048576) return false;
        HevcAccessUnit *p = realloc(*units, new_capacity * sizeof(**units));
        if (!p) return false;
        *units = p;
        *capacity = new_capacity;
    }
    (*units)[(*count)++] = (HevcAccessUnit){.offset = offset, .size = size, .key = key};
    return true;
}

bool lls_split_access_units(uint8_t codec, const uint8_t *data, size_t size,
                            LlsAccessUnit **units, size_t *count)
{
    if (codec == LLS_CODEC_AV1) return lls_av1_split_units(data, size, units, count);
    if (codec != LLS_CODEC_HEVC && codec != LLS_CODEC_H264) return false;
    const bool hevc = codec == LLS_CODEC_HEVC;
    const unsigned all_key_parts = hevc ? 15 : 7;
    size_t prefix = 0, at = start_code(data, size, 0, &prefix);
    size_t begin = at, capacity = 0;
    bool have_picture = false;
    unsigned key_parts = 0, previous_parts = 0;
    if (at == size) {
        fprintf(stderr, "No Annex-B start code found\n");
        return false;
    }
    for (size_t i = 0; i < at; ++i) {
        if (data[i]) {
            fprintf(stderr, "Unexpected data before first Annex-B NAL\n");
            return false;
        }
    }
    while (at < size) {
        size_t header = at + prefix, next_prefix = 0;
        size_t next = start_code(data, size, header, &next_prefix);
        size_t header_size = hevc ? 2 : 1;
        if (header + header_size > next || (data[header] & 0x80) ||
            (hevc ? !(data[header + 1] & 7) : !(data[header] & 31))) {
            fprintf(stderr, "Invalid %s NAL header at byte %zu\n", lls_codec_name(codec), at);
            return false;
        }
        unsigned type = hevc ? (data[header] >> 1) & 63 : data[header] & 31;
        unsigned layer = hevc ? ((data[header] & 1) << 5) | (data[header + 1] >> 3) : 0;
        if (layer) {
            fprintf(stderr, "Only single-layer HEVC is supported\n");
            return false;
        }
        bool vcl = hevc ? type <= 31 : type >= 1 && type <= 5;
        if (vcl && header + header_size + 1 > next) {
            fprintf(stderr, "Truncated slice header\n");
            return false;
        }
        /* H.264 first_mb_in_slice is ue(v): its first bit is 1 exactly
         * when it is zero. HEVC has a first_slice_segment_in_pic flag. */
        bool first_slice = vcl && (data[header + header_size] & 0x80);
        bool starts_next = first_slice || (hevc ?
            (type >= 32 && type <= 35) || type == 39 || (type >= 41 && type <= 44) ||
            (type >= 48 && type <= 55) : type == 6 || type == 7 || type == 8 || type == 9);
        if (have_picture && starts_next) {
            if (!append_au(units, count, &capacity, begin, at - begin, key_parts == all_key_parts))
                return false;
            previous_parts = key_parts;
            key_parts = 0;
            begin = at;
            have_picture = false;
        }
        if (hevc) {
            if (type == 19 || type == 20) key_parts |= 1;
            if (type >= 32 && type <= 34) key_parts |= 2u << (type - 32);
        } else {
            if (type == 5) key_parts |= 1;
            if (type == 7 || type == 8) key_parts |= 2u << (type - 7);
        }
        if (vcl) {
            if (!have_picture && !first_slice) {
                fprintf(stderr, "Access unit starts with a non-first slice\n");
                return false;
            }
            have_picture = true;
        }
        at = next;
        prefix = next_prefix;
    }
    if (have_picture) {
        if (!append_au(units, count, &capacity, begin, size - begin, key_parts == all_key_parts))
            return false;
    } else if (*count) {
        (*units)[*count - 1].size = size - (*units)[*count - 1].offset;
        (*units)[*count - 1].key = (previous_parts | key_parts) == all_key_parts;
    }
    if (!*count) fprintf(stderr, "No %s pictures found\n", lls_codec_name(codec));
    return *count != 0;
}

bool hevc_split_access_units(const uint8_t *data, size_t size,
                             HevcAccessUnit **units, size_t *count)
{
    return lls_split_access_units(LLS_CODEC_HEVC, data, size, units, count);
}
