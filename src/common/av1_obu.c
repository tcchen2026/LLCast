#include "av1_obu.h"
#include <stdlib.h>

typedef struct { const uint8_t *data; size_t size, bit; bool bad; } Bits;
static uint32_t bits(Bits *b, unsigned n)
{
    uint32_t value = 0;
    if (n > 32) { b->bad = true; return 0; }
    while (n--) {
        if (b->bit / 8 >= b->size) { b->bad = true; return 0; }
        value = (value << 1) | ((b->data[b->bit / 8] >> (7 - b->bit % 8)) & 1);
        ++b->bit;
    }
    return value;
}
static void uvlc(Bits *b)
{
    unsigned zeros = 0;
    while (!b->bad && !bits(b, 1)) ++zeros;
    if (zeros < 32) bits(b, zeros); // >=32 encodes UINT32_MAX with no suffix
}

bool lls_av1_next_obu(const uint8_t *data, size_t size, size_t *cursor, LlsAv1Obu *out)
{
    if (!data || !cursor || !out || *cursor >= size) return false;
    const size_t begin = *cursor;
    size_t at = begin;
    uint8_t header = data[at++];
    unsigned type = (header >> 3) & 15;
    if ((header & 0x81) || !(header & 2) || !type || (type >= 8 && type <= 14)) return false;
    if (header & 4) {
        /* Extension IDs and reserved bits are zero in this single-layer wire format. */
        if (at == size || data[at++]) return false;
    }
    uint64_t length = 0;
    for (unsigned i = 0; ; ++i) {
        if (i == 8 || at == size) return false;
        uint8_t part = data[at++];
        length |= (uint64_t)(part & 127) << (i * 7);
        if (!(part & 128)) break;
    }
    if (length > UINT32_MAX || length > size - at || (type == 2 && length)) return false;
    *out = (LlsAv1Obu){(uint8_t)type, begin, at + (size_t)length - begin, at, (size_t)length};
    *cursor = at + (size_t)length;
    return true;
}

bool lls_av1_sequence(const uint8_t *payload, size_t size, LlsAv1Sequence *out)
{
    if (!payload || !size || !out || size > 1024 * 1024) return false;
    Bits b = {payload, size, 0, false};
    unsigned profile = bits(&b, 3), still = bits(&b, 1), reduced = bits(&b, 1);
    if (profile > 2 || (reduced && !still)) return false;
    unsigned level = 0, tier = 0, delay_bits = 0, decoder_model = 0;
    if (reduced) level = bits(&b, 5);
    else {
        if (bits(&b, 1)) { // timing_info_present_flag
            bits(&b, 32); bits(&b, 32);
            if (bits(&b, 1)) uvlc(&b);
            decoder_model = bits(&b, 1);
            if (decoder_model) {
                delay_bits = bits(&b, 5) + 1;
                bits(&b, 32); bits(&b, 5); bits(&b, 5);
            }
        }
        unsigned display_delay = bits(&b, 1), ops = bits(&b, 5) + 1;
        for (unsigned i = 0; i < ops; ++i) {
            bits(&b, 12);
            unsigned op_level = bits(&b, 5), op_tier = op_level > 7 ? bits(&b, 1) : 0;
            if (!i) { level = op_level; tier = op_tier; }
            if (decoder_model && bits(&b, 1)) { bits(&b, delay_bits); bits(&b, delay_bits); bits(&b, 1); }
            if (display_delay && bits(&b, 1)) bits(&b, 4);
        }
    }
    unsigned width_bits = bits(&b, 4) + 1, height_bits = bits(&b, 4) + 1;
    uint32_t width = bits(&b, width_bits) + 1, height = bits(&b, height_bits) + 1;
    if (!reduced && bits(&b, 1)) { bits(&b, 4); bits(&b, 3); }
    bits(&b, 3); // superblock/filter/intra-edge flags
    if (!reduced) {
        bits(&b, 4);
        unsigned order_hint = bits(&b, 1);
        if (order_hint) bits(&b, 2);
        unsigned screen = bits(&b, 1) ? 2 : bits(&b, 1);
        if (screen && !bits(&b, 1)) bits(&b, 1);
        if (order_hint) bits(&b, 3);
    }
    bits(&b, 3); // superres, CDEF, restoration
    unsigned high = bits(&b, 1), twelve = profile == 2 && high ? bits(&b, 1) : 0;
    unsigned depth = twelve ? 12 : high ? 10 : 8;
    unsigned mono = profile == 1 ? 0 : bits(&b, 1);
    unsigned primaries = 2, transfer = 2, matrix = 2;
    if (bits(&b, 1)) { primaries = bits(&b, 8); transfer = bits(&b, 8); matrix = bits(&b, 8); }
    unsigned x = 1, y = 1, position = 0;
    if (mono) bits(&b, 1); // range; no separate_uv_delta_q bit in monochrome
    else {
        if (primaries == 1 && transfer == 13 && matrix == 0) x = y = 0;
        else {
            bits(&b, 1); // colour range
            if (profile == 1) x = y = 0;
            else if (profile == 2) {
                x = depth == 12 ? bits(&b, 1) : 1;
                y = depth == 12 && x ? bits(&b, 1) : 0;
            }
            if (x && y) position = bits(&b, 2);
        }
        bits(&b, 1); // separate_uv_delta_q
    }
    bits(&b, 1); // film_grain_params_present
    if (!bits(&b, 1)) return false; // trailing_one_bit
    while (!b.bad && b.bit / 8 < size) if (bits(&b, 1)) return false;
    if (b.bad || position == 3 || (level > 23 && level != 31) || (profile == 0 && (!x || !y))) return false;
    *out = (LlsAv1Sequence){.width = width, .height = height, .reduced_still_picture = reduced != 0,
        .av1c = {0x81, (uint8_t)((profile << 5) | level),
            (uint8_t)((tier << 7) | (high << 6) | (twelve << 5) | (mono << 4) | (x << 3) | (y << 2) | position), 0}};
    return true;
}

static bool append(LlsAccessUnit **units, size_t *count, size_t *capacity, size_t at, size_t size, bool key)
{
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 64;
        if (next > 1048576) return false;
        LlsAccessUnit *p = realloc(*units, next * sizeof(*p));
        if (!p) return false;
        *units = p; *capacity = next;
    }
    (*units)[(*count)++] = (LlsAccessUnit){at, size, key};
    return true;
}

bool lls_av1_split_units(const uint8_t *data, size_t size, LlsAccessUnit **units, size_t *count)
{
    if (!data || !size || !units || !count || *count) return false;
    size_t cursor = 0, begin = 0, capacity = 0;
    bool delimiter = false, sequence = false, picture = false, pending_tiles = false, key = false;
    LlsAv1Sequence seq = {0};
    while (cursor < size) {
        LlsAv1Obu obu;
        if (!lls_av1_next_obu(data, size, &cursor, &obu)) return false;
        if (obu.type == 2) {
            if (delimiter && (!picture || pending_tiles ||
                !append(units, count, &capacity, begin, obu.offset - begin, key))) return false;
            begin = obu.offset;
            delimiter = true; sequence = picture = pending_tiles = key = false;
        } else if (!delimiter) return false;
        else if (obu.type == 1) {
            if (picture || sequence || !lls_av1_sequence(data + obu.payload_offset, obu.payload_size, &seq)) return false;
            sequence = true;
        } else if (obu.type == 3 || obu.type == 6) {
            if (pending_tiles || !obu.payload_size) return false;
            uint8_t header = data[obu.payload_offset];
            bool reduced = sequence && seq.reduced_still_picture;
            bool existing = !reduced && (header & 0x80);
            bool shown_key = reduced || (!existing && !(header & 0x60) && (header & 0x10));
            if (!picture) key = sequence && shown_key;
            picture = true;
            pending_tiles = obu.type == 3 && !existing;
        } else if (obu.type == 4) {
            if (!picture || !obu.payload_size) return false;
            pending_tiles = false;
        } else if (obu.type == 7 && (!picture || !obu.payload_size)) return false;
    }
    return delimiter && picture && !pending_tiles && append(units, count, &capacity, begin, size - begin, key);
}
