#include "check.h"
#include "lls_assembler.h"
#include "lls_fec.h"
#include <string.h>

typedef struct {
    unsigned data, parity, blocks, m;
    size_t size;
    uint8_t *bytes;
} Encoded;

static Encoded encode(unsigned count)
{
    Encoded f = {.data = count, .size = (size_t)count * LLS_MAX_VIDEO_PAYLOAD - 13};
    f.blocks = (count + 127) / 128;
    f.m = lls_fec_parity_per_block(count, 20, 1);
    f.parity = f.m * f.blocks;
    f.bytes = calloc(count + f.parity, LLS_MAX_VIDEO_PAYLOAD); CHECK(f.bytes);
    for (size_t i = 0; i < f.size; ++i) f.bytes[i] = (uint8_t)(i * 13 + (i >> 8));
    for (unsigned b = 0; b < f.blocks; ++b) {
        const uint8_t *data[128]; uint8_t *parity[255];
        unsigned k = (count - b + f.blocks - 1) / f.blocks;
        for (unsigned i = 0; i < k; ++i) data[i] = f.bytes + (size_t)(b + i * f.blocks) * LLS_MAX_VIDEO_PAYLOAD;
        for (unsigned i = 0; i < f.m; ++i)
            parity[i] = f.bytes + (size_t)(count + b + i * f.blocks) * LLS_MAX_VIDEO_PAYLOAD;
        CHECK(lls_fec_encode(data, k, parity, f.m, LLS_MAX_VIDEO_PAYLOAD));
    }
    return f;
}

static void push(LlsAssembler *a, const Encoded *f, unsigned i, unsigned frame, uint32_t *seq, int64_t now)
{
    uint8_t packet[LLS_MAX_DATAGRAM];
    size_t size = i + 1 == f->data ? f->size - (size_t)i * LLS_MAX_VIDEO_PAYLOAD : LLS_MAX_VIDEO_PAYLOAD;
    LlsVideo v = {.codec = LLS_CODEC_HEVC, .sequence = (*seq)++, .frame = frame, .frame_bytes = (uint32_t)f->size,
        .index = (uint16_t)i, .count = (uint16_t)f->data, .flags = frame == 0 || frame == 3 ? LLS_FRAME_KEY : 0,
        .fec_scheme = LLS_FEC_RS, .fec_count = (uint16_t)f->parity};
    lls_write_video(packet, 1, &v);
    memcpy(packet + LLS_VIDEO_HEADER_SIZE, f->bytes + (size_t)i * LLS_MAX_VIDEO_PAYLOAD, size);
    const uint8_t *payload; size_t length;
    CHECK(lls_read_video(packet, size + LLS_VIDEO_HEADER_SIZE, &v, &payload, &length));
    lls_assembler_push(a, 1, &v, payload, length, now);
}

/* Exact packet-layer erasures: none, last data, burst five (interleaved),
 * whole first block (unrepairable), and parity-first reordering. */
static void scenario(unsigned count, unsigned loss)
{
    Encoded f = encode(count);
    LlsAssembler *a = lls_assembler_create(NULL); CHECK(a);
    uint32_t seq = 0; int64_t now = 1000000;
    unsigned delivered = 0, lost = 0, recover = 0, repaired = 0;
    for (unsigned frame = 0; frame < 4; ++frame) {
        for (unsigned n = 0; n < f.data + f.parity; ++n) {
            unsigned i = loss == 4 ? (n + f.data) % (f.data + f.parity) : n;
            bool drop = frame == 1 && ((loss == 1 && i + 1 == f.data) ||
                        (loss == 2 && i >= 3 && i < 8) ||
                        (loss == 3 && (i < f.data ? i : i - f.data) % f.blocks == 0));
            now += 1000;
            if (drop) { ++seq; continue; }
            push(a, &f, i, frame, &seq, now);
            LlsOutput out;
            while (lls_assembler_next(a, now, &out)) {
                if (out.kind == LLS_OUT_RESET) continue;
                if (out.kind == LLS_OUT_RECOVER) { ++recover; continue; }
                if (out.kind == LLS_OUT_DROPPED) {
                    CHECK(loss == 3 && out.frame == 1 && out.fate == LLS_FATE_LOST);
                    ++lost; continue;
                }
                CHECK(out.size == f.size && !memcmp(out.data, f.bytes, f.size));
                for (unsigned j = 0; j < LLS_AU_PADDING; ++j) CHECK(out.data[out.size + j] == 0);
                CHECK(out.packets_expected == f.data + f.parity && out.fec_packets == f.parity);
                CHECK(out.damaged == (loss == 3 && frame == 2));
                CHECK(out.frames_repaired == (out.fragments_repaired != 0));
                repaired += out.fragments_repaired;
                ++delivered; free(out.data);
            }
        }
    }
    CHECK(delivered == (loss == 3 ? 3u : 4u));
    CHECK((lost != 0) == (loss == 3) && (recover != 0) == (loss == 3));
    if (loss == 1) CHECK(repaired == 1);
    if (loss == 2) CHECK(repaired == 5);
    if (!loss || loss == 3) CHECK(!repaired);
    LlsAssemblerStats stats; lls_assembler_stats(a, &stats);
    /* Parity after its frame is complete is unneeded, not too late. */
    CHECK(stats.fragments_repaired == repaired && stats.unneeded > 0 && !stats.rejected);
    lls_assembler_destroy(a); free(f.bytes);
}

static void late_reconstructed_data(void)
{
    Encoded f = encode(8); // Two parity shards; repair before next() consumes it.
    LlsAssembler *a = lls_assembler_create(NULL); CHECK(a);
    uint32_t sequence = 0;
    int64_t now = 1000000;
    for (unsigned i = 1; i < f.data; ++i) push(a, &f, i, 0, &sequence, now++);
    push(a, &f, f.data, 0, &sequence, now++);
    int64_t complete = now - 1;
    /* A late corrupt copy of the reconstructed shard must not overwrite it. */
    f.bytes[0] ^= 0xff;
    push(a, &f, 0, 0, &sequence, now++);
    f.bytes[0] ^= 0xff;
    push(a, &f, 0, 0, &sequence, now++);
    push(a, &f, f.data + 1, 0, &sequence, now++);
    push(a, &f, 1, 0, &sequence, now++); // A true data duplicate stays a duplicate.
    LlsOutput out;
    CHECK(lls_assembler_next(a, now, &out) && out.kind == LLS_OUT_RESET);
    CHECK(lls_assembler_next(a, now, &out) && out.kind == LLS_OUT_FRAME);
    CHECK(out.packets_received == f.data && out.complete_ns == complete);
    CHECK(out.fragments_repaired == 1 && out.frames_repaired == 1);
    CHECK(out.size == f.size && !memcmp(out.data, f.bytes, f.size));
    free(out.data);
    LlsAssemblerStats stats; lls_assembler_stats(a, &stats);
    /* Both copies of the rebuilt shard and the parity arrived after completion: unneeded. */
    CHECK(!stats.late && stats.unneeded == 3 && stats.duplicates == 1 && !stats.rejected);
    CHECK(stats.frames_repaired == 1 && stats.fragments_repaired == 1);
    lls_assembler_destroy(a); free(f.bytes);
}

int main(void)
{
    late_reconstructed_data();
    for (unsigned loss = 0; loss <= 4; ++loss) scenario(385, loss);
    scenario(1, 0); scenario(1, 1); scenario(128, 1); scenario(129, 2);
    puts("FEC packets: single/tail/burst/block erasures, interleaving, parity-first, late parity and damaged recovery verified");
}
