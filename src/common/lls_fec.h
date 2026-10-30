#ifndef LLSTREAM_FEC_H
#define LLSTREAM_FEC_H

/* Systematic Reed-Solomon erasure code over GF(2^8), the coding used by
 * LLS_FEC_RS (llstream_proto.h). A block has k data and m parity shards of
 * equal size, k + m <= 255; any k of the k + m shards rebuild the data.
 * Its field polynomial, generator matrix and shard order are part of the
 * wire format: changing them needs a new FEC scheme number.
 * Pure computation, no allocation in encode; portable C99. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LLS_FEC_MAX_SHARDS 255u

/* Computes m parity shards from k data shards, each `size` bytes. */
bool lls_fec_encode(const uint8_t *const *data, unsigned k,
                    uint8_t *const *parity, unsigned m, size_t size);

/* Rebuilds the missing data shards in place. shards[0..k) are data and
 * shards[k..k+m) parity; present[i] says which arrived. Needs at least k
 * present shards; returns false otherwise. Parity shards are not rebuilt. */
bool lls_fec_decode(uint8_t *const *shards, const bool *present,
                    unsigned k, unsigned m, size_t size);

/* Parity shards per block for a frame of `count` data fragments, given a
 * percentage and a per-block minimum: equal for every block, as the wire
 * format requires, and capped so each block stays within 255 shards.
 * Returns 0 when percent is 0. */
unsigned lls_fec_parity_per_block(unsigned count, unsigned percent, unsigned minimum);

#endif
