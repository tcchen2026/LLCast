#include "lls_fec.h"
#include "llstream_proto.h"

#include <stdlib.h>
#include <string.h>

/* Scheme 1, original implementation. Bytes are polynomials modulo 0x11d
 * (x^8+x^4+x^3+x^2+1), primitive element x=2. The systematic generator is
 * [I; C], C[r,c] = inverse(r XOR (m+c)), r=0..m-1, c=0..k-1.
 * The two coordinate sets are disjoint; every square Cauchy minor is
 * invertible. Thus any k rows of [I; C] determine the data. Shard order is
 * data 0..k-1 followed by parity 0..m-1. These choices define wire scheme 1.
 * Tables are local, so concurrent encoders/decoders need no initialization
 * lock or mutable global state. No third-party code is used. */
typedef struct { uint8_t exp[510], log[256]; } Field;

static void field_init(Field *f)
{
    unsigned v = 1;
    f->log[0] = 0;
    for (unsigned i = 0; i < 255; ++i) {
        f->exp[i] = (uint8_t)v;
        f->log[v] = (uint8_t)i;
        v <<= 1;
        if (v & 256) v ^= 0x11d;
    }
    memcpy(f->exp + 255, f->exp, 255);
}

static uint8_t multiply(const Field *f, uint8_t a, uint8_t b)
{
    return a && b ? f->exp[f->log[a] + f->log[b]] : 0;
}

static uint8_t inverse(const Field *f, uint8_t v)
{
    return f->exp[255 - f->log[v]]; /* Caller ensures v != 0. */
}

static void add_scaled(const Field *f, uint8_t *dst, const uint8_t *src,
                        uint8_t coefficient, size_t size)
{
    if (!coefficient) return;
    if (coefficient == 1) {
        for (size_t i = 0; i < size; ++i) dst[i] ^= src[i];
    } else {
        uint8_t table[256];
        table[0] = 0;
        unsigned exponent = f->log[coefficient];
        for (unsigned i = 1; i < 256; ++i) table[i] = f->exp[exponent + f->log[i]];
        for (size_t i = 0; i < size; ++i) dst[i] ^= table[src[i]];
    }
}

static bool valid(unsigned k, unsigned m)
{
    return k && k <= LLS_FEC_MAX_SHARDS && m <= LLS_FEC_MAX_SHARDS - k;
}

bool lls_fec_encode(const uint8_t *const *data, unsigned k,
                    uint8_t *const *parity, unsigned m, size_t size)
{
    if (!valid(k, m) || !data || (m && !parity)) return false;
    for (unsigned c = 0; c < k; ++c) if (!data[c]) return false;
    for (unsigned r = 0; r < m; ++r) if (!parity[r]) return false;
    Field field;
    field_init(&field);
    for (unsigned r = 0; r < m; ++r) {
        memset(parity[r], 0, size);
        for (unsigned c = 0; c < k; ++c)
            add_scaled(&field, parity[r], data[c], inverse(&field, (uint8_t)(r ^ (m + c))), size);
    }
    return true;
}

bool lls_fec_decode(uint8_t *const *shards, const bool *present,
                    unsigned k, unsigned m, size_t size)
{
    if (!valid(k, m) || !shards || !present) return false;
    unsigned missing[LLS_FEC_MAX_SHARDS], rows[LLS_FEC_MAX_SHARDS], count = 0, available = 0;
    for (unsigned c = 0; c < k; ++c) {
        if (!shards[c]) return false;
        if (!present[c]) missing[count++] = c;
    }
    for (unsigned r = 0; r < m; ++r) {
        if (present[k + r]) {
            if (!shards[k + r]) return false;
            rows[available++] = r;
        }
    }
    if (available < count) return false;
    if (!count) return true;
    /* Solve only for the erased data columns, not a full k-by-k inverse.
     * Missing data buffers hold the RHS; known data and all parity stay intact. */
    uint8_t *matrix = malloc((size_t)count * count);
    if (!matrix) return false;
    Field field;
    field_init(&field);
    for (unsigned r = 0; r < count; ++r) {
        uint8_t *dst = shards[missing[r]];
        memcpy(dst, shards[k + rows[r]], size);
        for (unsigned c = 0; c < k; ++c)
            if (present[c]) add_scaled(&field, dst, shards[c],
                inverse(&field, (uint8_t)(rows[r] ^ (m + c))), size);
        for (unsigned c = 0; c < count; ++c)
            matrix[r * count + c] = inverse(&field, (uint8_t)(rows[r] ^ (m + missing[c])));
    }
    for (unsigned c = 0; c < count; ++c) {
        unsigned pivot = c;
        while (pivot < count && !matrix[pivot * count + c]) ++pivot;
        if (pivot == count) { free(matrix); return false; }
        if (pivot != c) {
            for (unsigned j = 0; j < count; ++j) {
                uint8_t v = matrix[c * count + j];
                matrix[c * count + j] = matrix[pivot * count + j];
                matrix[pivot * count + j] = v;
            }
            for (size_t j = 0; j < size; ++j) {
                uint8_t v = shards[missing[c]][j];
                shards[missing[c]][j] = shards[missing[pivot]][j];
                shards[missing[pivot]][j] = v;
            }
        }
        uint8_t scale = inverse(&field, matrix[c * count + c]);
        for (unsigned j = c; j < count; ++j)
            matrix[c * count + j] = multiply(&field, matrix[c * count + j], scale);
        for (size_t j = 0; j < size; ++j)
            shards[missing[c]][j] = multiply(&field, shards[missing[c]][j], scale);
        for (unsigned r = 0; r < count; ++r) {
            if (r == c) continue;
            uint8_t coefficient = matrix[r * count + c];
            for (unsigned j = c; j < count; ++j)
                matrix[r * count + j] ^= multiply(&field, coefficient, matrix[c * count + j]);
            add_scaled(&field, shards[missing[r]], shards[missing[c]], coefficient, size);
        }
    }
    free(matrix);
    return true;
}

unsigned lls_fec_parity_per_block(unsigned count, unsigned percent, unsigned minimum)
{
    if (!count || count > UINT16_MAX || !percent) return 0;
    unsigned blocks = (count + LLS_FEC_MAX_DATA - 1) / LLS_FEC_MAX_DATA;
    unsigned largest = (count + blocks - 1) / blocks;
    uint64_t parity = ((uint64_t)largest * percent + 99) / 100;
    if (parity < minimum) parity = minimum;
    unsigned limit = LLS_FEC_MAX_SHARDS - largest;
    /* Indices, expected/received counts and fec_count are all uint16. */
    unsigned wire_limit = (UINT16_MAX - count) / blocks;
    if (limit > wire_limit) limit = wire_limit;
    return parity < limit ? (unsigned)parity : limit;
}
