#include "check.h"
#include "lls_assembler.h"

#include <string.h>

static uint8_t data[4 * LLS_MAX_VIDEO_PAYLOAD];
static const int64_t start = INT64_C(1000000000);

static LlsVideo fragment(uint32_t frame, uint8_t flags, uint16_t index,
                         uint32_t bytes, uint32_t sequence)
{
    return (LlsVideo){.codec = LLS_CODEC_HEVC, .sequence = sequence, .frame = frame, .frame_bytes = bytes,
        .index = index, .count = (uint16_t)((bytes + LLS_MAX_VIDEO_PAYLOAD - 1) / LLS_MAX_VIDEO_PAYLOAD),
        .flags = flags, .capture_us = (uint64_t)frame * 100, .encoded_us = 400, .sent_us = sequence};
}

static void push(LlsAssembler *a, uint32_t session, LlsVideo v, int64_t at)
{
    size_t offset = (size_t)v.index * LLS_MAX_VIDEO_PAYLOAD;
    size_t size = v.frame_bytes - offset;
    if (size > LLS_MAX_VIDEO_PAYLOAD) size = LLS_MAX_VIDEO_PAYLOAD;
    lls_assembler_push(a, session, &v, data + offset, size, at);
}

static void one(LlsAssembler *a, uint32_t frame, bool key, int64_t at)
{
    push(a, 1, fragment(frame, key ? LLS_FRAME_KEY : 0, 0, 17, frame), at);
}

static LlsOutput expect(LlsAssembler *a, int64_t at, LlsOutputKind kind, uint32_t frame)
{
    LlsOutput out;
    CHECK(lls_assembler_next(a, at, &out));
    CHECK(out.kind == kind && out.frame == frame);
    if (kind != LLS_OUT_FRAME) CHECK(!out.data);
    return out;
}

static void quiet(LlsAssembler *a, int64_t at)
{
    LlsOutput out;
    CHECK(!lls_assembler_next(a, at, &out));
}

static LlsOutput delivered(LlsAssembler *a, int64_t at, uint32_t frame, size_t size)
{
    LlsOutput out = expect(a, at, LLS_OUT_FRAME, frame);
    CHECK(out.span == 1);
    CHECK(out.size == size && out.packets_received == out.packets_expected);
    CHECK(out.capture_us == (uint64_t)frame * 100 && out.encoded_us == 400);
    CHECK(!memcmp(out.data, data, size));
    for (unsigned i = 0; i < LLS_AU_PADDING; ++i) CHECK(!out.data[size + i]);
    free(out.data);
    out.data = NULL;
    return out;
}

static LlsOutput dropped(LlsAssembler *a, int64_t at, uint32_t frame, LlsFrameFate fate)
{
    LlsOutput out = expect(a, at, LLS_OUT_DROPPED, frame);
    CHECK(out.fate == fate && out.span == 1);
    return out;
}

static LlsAssembler *running(void)
{
    LlsAssembler *a = lls_assembler_create(NULL);
    CHECK(a && lls_assembler_deadline(a) == INT64_MAX);
    one(a, 0, true, start);
    CHECK(lls_assembler_deadline(a) <= start);
    expect(a, start, LLS_OUT_RESET, 0);
    delivered(a, start, 0, 17);
    quiet(a, start);
    CHECK(lls_assembler_deadline(a) == INT64_MAX);
    return a;
}

static void test_ordered(void)
{
    LlsAssembler *a = lls_assembler_create(NULL);
    CHECK(a);
    /* A receive batch starting with a key must not request a redundant key. */
    one(a, 0, true, start);
    one(a, 1, false, start + 10);
    expect(a, start + 10, LLS_OUT_RESET, 0);
    delivered(a, start + 10, 0, 17);
    delivered(a, start + 10, 1, 17);
    quiet(a, start + 10);
    for (unsigned frame = 2; frame < 20; ++frame) {
        LlsVideo v = fragment(frame, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 7, frame * 2);
        push(a, 1, v, start + frame * 100);
        quiet(a, start + frame * 100);
        ++v.index;
        ++v.sequence;
        push(a, 1, v, start + frame * 100 + 1);
        LlsOutput out = delivered(a, start + frame * 100 + 1, frame, v.frame_bytes);
        CHECK(out.first_rx_ns == start + frame * 100 && out.complete_ns == start + frame * 100 + 1);
    }
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.frames_delivered == 20 && !stats.frames_lost && !stats.recover_requests);
    lls_assembler_destroy(a);
}

static void test_codec(void)
{
    LlsAssembler *a = running();
    LlsVideo v = fragment(1, 0, 0, 17, 1);
    v.codec = LLS_CODEC_H264;
    push(a, 1, v, start + 1);
    quiet(a, start + 1);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.rejected == 1);
    v.codec = LLS_CODEC_HEVC;
    push(a, 1, v, start + 2);
    CHECK(delivered(a, start + 2, 1, 17).codec == LLS_CODEC_HEVC);
    v = fragment(0, LLS_FRAME_KEY, 0, 17, 0);
    v.codec = LLS_CODEC_H264;
    push(a, 2, v, start + 3);
    CHECK(expect(a, start + 3, LLS_OUT_RESET, 0).codec == LLS_CODEC_H264);
    CHECK(delivered(a, start + 3, 0, 17).codec == LLS_CODEC_H264);
    lls_assembler_destroy(a);
}

static void test_reorder_duplicates(void)
{
    LlsAssembler *a = running();
    LlsVideo v = fragment(1, 0, 2, 2 * LLS_MAX_VIDEO_PAYLOAD + 13, 12);
    push(a, 1, v, start + 100);
    push(a, 1, v, start + 101);
    v.index = 0;
    v.sequence = 10;
    push(a, 1, v, start + 102);
    quiet(a, start + 102);
    v.index = 1;
    v.sequence = 11;
    push(a, 1, v, start + 103);
    push(a, 1, v, start + 104); /* Duplicate of a complete, undelivered AU. */
    LlsOutput out = delivered(a, start + 104, 1, v.frame_bytes);
    CHECK(out.first_rx_ns == start + 100 && out.complete_ns == start + 103);
    push(a, 1, v, start + 105);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    /* After delivery: complete, so unneeded rather than too late. */
    CHECK(stats.duplicates == 2 && !stats.late && stats.unneeded == 1 && !stats.rejected);
    CHECK(stats.sequence_gaps == 11); /* Forward jumps, not a packet-loss estimate. */
    lls_assembler_destroy(a);
}

static void test_loss_and_key(void)
{
    LlsAssembler *a = running();
    LlsVideo v = fragment(1, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 3, 1);
    push(a, 1, v, start + 100);
    one(a, 2, false, start + 200);
    ++v.index;
    push(a, 1, v, start + 201); /* Too late even before next() emits LOST. */
    LlsOutput lost = dropped(a, start + 201, 1, LLS_FATE_LOST);
    CHECK(lost.packets_received == 1 && lost.packets_expected == 2 && !lost.complete_ns);
    CHECK(lost.first_rx_ns == start + 100 && lost.size == v.frame_bytes);
    expect(a, start + 201, LLS_OUT_RECOVER, 1);
    LlsOutput next = delivered(a, start + 201, 2, 17);
    CHECK(next.damaged && next.packets_received == 1 && next.complete_ns == start + 200);
    one(a, 3, false, start + 300);
    CHECK(delivered(a, start + 300, 3, 17).damaged);
    one(a, 4, true, start + 400);
    CHECK(!delivered(a, start + 400, 4, 17).damaged);
    one(a, 5, false, start + 500);
    CHECK(!delivered(a, start + 500, 5, 17).damaged);
    quiet(a, start + 100000000);
    CHECK(lls_assembler_deadline(a) == INT64_MAX);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.frames_delivered == 5 && stats.frames_lost == 1 && !stats.frames_skipped);
    CHECK(stats.recover_requests == 1 && stats.late == 1);
    lls_assembler_destroy(a);
}

static void test_timeouts(void)
{
    LlsAssembler *a = running();
    int64_t first = start + 100, expires = first + 20000000;
    LlsVideo v = fragment(1, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 1, 1);
    push(a, 1, v, first);
    CHECK(lls_assembler_deadline(a) == expires);
    quiet(a, expires - 1);
    dropped(a, expires, 1, LLS_FATE_LOST);
    CHECK(lls_assembler_deadline(a) <= expires);
    expect(a, expires, LLS_OUT_RECOVER, 1);
    CHECK(lls_assembler_deadline(a) == expires + 20000000);
    quiet(a, expires + 19999999);
    expect(a, expires + 20000000, LLS_OUT_RECOVER, 1);
    CHECK(lls_assembler_deadline(a) == expires + 40000000);
    v.index = 1;
    push(a, 1, v, expires + 20000001);
    quiet(a, expires + 20000001);
    one(a, 2, true, expires + 40000000);
    delivered(a, expires + 40000000, 2, 17); /* A ready key suppresses a due repeat. */
    CHECK(lls_assembler_deadline(a) == INT64_MAX);
    lls_assembler_destroy(a);

    a = running();
    v = fragment(1, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 1, 1);
    push(a, 1, v, first);
    v.index = 1;
    push(a, 1, v, expires); /* push also expires frames, before copying late bytes. */
    dropped(a, expires, 1, LLS_FATE_LOST);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.late == 1);
    lls_assembler_destroy(a);
}

static void test_external_recovery(void)
{
    LlsAssembler *a = running();
    lls_assembler_recover(a, 0, start + 1); /* Error on the initial key itself. */
    expect(a, start + 1, LLS_OUT_RECOVER, 0);
    one(a, 1, false, start + 2);
    CHECK(delivered(a, start + 2, 1, 17).damaged);
    /* A second loss while damaged must advance the target immediately. */
    one(a, 3, false, start + 3);
    dropped(a, start + 3, 2, LLS_FATE_LOST);
    expect(a, start + 3, LLS_OUT_RECOVER, 2);
    CHECK(delivered(a, start + 3, 3, 17).damaged);
    expect(a, start + 20000003, LLS_OUT_RECOVER, 2);
    one(a, 4, true, start + 20000004);
    CHECK(!delivered(a, start + 20000004, 4, 17).damaged);
    lls_assembler_recover(a, 1, start + 20000005); /* A newer key already delivered. */
    quiet(a, start + 20000005);
    CHECK(lls_assembler_deadline(a) == INT64_MAX);
    lls_assembler_destroy(a);
}

static void test_missing(void)
{
    LlsAssembler *a = running();
    one(a, 4, true, start + 100);
    one(a, 1, false, start + 101); /* A previously unseen gap is already lost. */
    for (unsigned i = 1; i < 4; ++i) {
        LlsOutput out = dropped(a, start + 101, i, LLS_FATE_LOST);
        CHECK(!out.packets_expected && !out.packets_received && !out.first_rx_ns && !out.complete_ns);
        expect(a, start + 101, LLS_OUT_RECOVER, i);
    }
    delivered(a, start + 101, 4, 17);
    quiet(a, start + 101);
    lls_assembler_destroy(a);

    /* If a later incomplete frame expires behind an entirely missing frame,
     * both are lost at that moment: with in-order delivery the missing number
     * cannot still be in flight. */
    a = running();
    push(a, 1, fragment(2, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 1, 2), start + 100);
    quiet(a, start + 100);
    CHECK(lls_assembler_deadline(a) == start + 20000100);
    dropped(a, start + 20000100, 1, LLS_FATE_LOST);
    expect(a, start + 20000100, LLS_OUT_RECOVER, 1);
    dropped(a, start + 20000100, 2, LLS_FATE_LOST);
    expect(a, start + 20000100, LLS_OUT_RECOVER, 2);
    quiet(a, start + 20000100);
    one(a, 3, true, start + 20000200);
    delivered(a, start + 20000200, 3, 17);
    lls_assembler_destroy(a);
}

static void test_overflow(void)
{
    LlsAssemblerConfig config;
    lls_assembler_default_config(&config);
    config.max_pending = 2;
    LlsAssembler *a = lls_assembler_create(&config);
    CHECK(a);
    one(a, 0, true, start);
    expect(a, start, LLS_OUT_RESET, 0);
    delivered(a, start, 0, 17);
    for (unsigned i = 1; i <= 3; ++i)
        push(a, 1, fragment(i, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 1, i), start + i);
    dropped(a, start + 3, 1, LLS_FATE_LOST);
    expect(a, start + 3, LLS_OUT_RECOVER, 1);
    quiet(a, start + 3);
    push(a, 1, fragment(2, 0, 1, LLS_MAX_VIDEO_PAYLOAD + 1, 4), start + 4);
    CHECK(delivered(a, start + 4, 2, LLS_MAX_VIDEO_PAYLOAD + 1).damaged);
    one(a, 4, true, start + 5);
    dropped(a, start + 5, 3, LLS_FATE_LOST);
    expect(a, start + 5, LLS_OUT_RECOVER, 3);
    delivered(a, start + 5, 4, 17);
    lls_assembler_destroy(a);
}

static void test_large_gaps(void)
{
    LlsAssembler *a = running();
    const uint32_t jump = UINT32_C(1) << 30;
    one(a, jump, false, start + 1);
    LlsOutput out = expect(a, start + 1, LLS_OUT_DROPPED, 1);
    CHECK(out.span == jump - 1 && out.fate == LLS_FATE_LOST);
    CHECK(!out.first_rx_ns && !out.complete_ns && !out.packets_received && !out.size);
    expect(a, start + 1, LLS_OUT_RECOVER, jump - 1);
    CHECK(delivered(a, start + 1, jump, 17).damaged);
    quiet(a, start + 1); /* Exactly three outputs, independent of gap size. */
    one(a, jump + 1, false, start + 2);
    CHECK(delivered(a, start + 2, jump + 1, 17).damaged);
    one(a, jump + 2, true, start + 3);
    delivered(a, start + 3, jump + 2, 17);
    quiet(a, start + 100000000);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.frames_lost == jump - 1 && !stats.frames_skipped);
    CHECK(stats.frames_delivered == 4 && stats.recover_requests == 1);
    lls_assembler_destroy(a);

    /* A known partial frame splits gaps and retains its own loss metadata. */
    a = running();
    push(a, 1, fragment(jump / 2, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 1, 1), start + 1);
    one(a, jump, true, start + 2);
    out = expect(a, start + 2, LLS_OUT_DROPPED, 1);
    CHECK(out.span == jump / 2 - 1 && out.fate == LLS_FATE_LOST);
    expect(a, start + 2, LLS_OUT_RECOVER, jump / 2 - 1);
    out = dropped(a, start + 2, jump / 2, LLS_FATE_LOST);
    CHECK(out.packets_received == 1 && out.packets_expected == 2 && out.first_rx_ns == start + 1);
    expect(a, start + 2, LLS_OUT_RECOVER, jump / 2);
    out = expect(a, start + 2, LLS_OUT_DROPPED, jump / 2 + 1);
    CHECK(out.span == jump / 2 - 1 && out.fate == LLS_FATE_LOST);
    expect(a, start + 2, LLS_OUT_RECOVER, jump - 1);
    delivered(a, start + 2, jump, 17);
    quiet(a, start + 2);
    lls_assembler_destroy(a);

    /* Inclusive threshold, zero threshold, and a coalesced run across wrap. */
    for (unsigned limit = 0; limit <= 3; limit += 3) {
        for (unsigned gap = 3; gap <= 4; ++gap) {
            LlsAssemblerConfig config;
            lls_assembler_default_config(&config);
            config.max_gap = limit;
            a = lls_assembler_create(&config);
            CHECK(a);
            uint32_t first = UINT32_MAX - 1, last = first + gap + 1;
            one(a, first, true, start);
            expect(a, start, LLS_OUT_RESET, first);
            delivered(a, start, first, 17);
            one(a, last, true, start + 1);
            if (gap > limit) {
                out = expect(a, start + 1, LLS_OUT_DROPPED, first + 1);
                CHECK(out.span == gap && out.fate == LLS_FATE_LOST);
                expect(a, start + 1, LLS_OUT_RECOVER, last - 1);
            } else {
                for (unsigned i = 1; i <= gap; ++i) {
                    dropped(a, start + 1, first + i, LLS_FATE_LOST);
                    expect(a, start + 1, LLS_OUT_RECOVER, first + i);
                }
            }
            delivered(a, start + 1, last, 17);
            quiet(a, start + 1);
            lls_assembler_stats(a, &stats);
            CHECK(stats.frames_lost == gap);
            lls_assembler_destroy(a);
        }
    }
}

static void test_sessions_and_wrap(void)
{
    LlsAssembler *a = running();
    one(a, 1, false, start + 1); /* Will be discarded, without delivery. */
    push(a, 9, fragment(UINT32_MAX - 1, LLS_FRAME_KEY, 0, 17, UINT32_MAX - 1), start + 2);
    LlsOutput out = expect(a, start + 2, LLS_OUT_RESET, UINT32_MAX - 1);
    CHECK(out.session == 9);
    delivered(a, start + 2, UINT32_MAX - 1, 17);
    push(a, 9, fragment(UINT32_MAX, 0, 0, 17, UINT32_MAX), start + 3);
    delivered(a, start + 3, UINT32_MAX, 17);
    push(a, 9, fragment(0, 0, 0, 17, 0), start + 4);
    delivered(a, start + 4, 0, 17);
    push(a, 9, fragment(UINT32_MAX, 0, 0, 17, UINT32_MAX), start + 5);
    quiet(a, start + 5);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.frames_delivered == 4 && !stats.sequence_gaps && !stats.late && stats.unneeded == 1);
    lls_assembler_destroy(a);

    a = lls_assembler_create(NULL);
    CHECK(a);
    one(a, UINT32_MAX - 1, true, start);
    expect(a, start, LLS_OUT_RESET, UINT32_MAX - 1);
    delivered(a, start, UINT32_MAX - 1, 17);
    one(a, 1, true, start + 1);
    dropped(a, start + 1, UINT32_MAX, LLS_FATE_LOST);
    expect(a, start + 1, LLS_OUT_RECOVER, UINT32_MAX);
    dropped(a, start + 1, 0, LLS_FATE_LOST);
    expect(a, start + 1, LLS_OUT_RECOVER, 0);
    delivered(a, start + 1, 1, 17);
    lls_assembler_destroy(a);
}

static void test_rejected(void)
{
    LlsAssemblerConfig config;
    lls_assembler_default_config(&config);
    config.max_frame_bytes = 2 * LLS_MAX_VIDEO_PAYLOAD;
    LlsAssembler *a = lls_assembler_create(&config);
    CHECK(a);
    one(a, 0, true, start);
    expect(a, start, LLS_OUT_RESET, 0);
    delivered(a, start, 0, 17);
    LlsVideo base = fragment(1, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 10, 1), v = base;
    push(a, 1, base, start + 1);
    for (unsigned i = 0; i < 5; ++i) {
        v = base;
        v.index = 1;
        switch (i) {
        case 0: ++v.frame_bytes; break;
        case 1: v.frame_bytes += LLS_MAX_VIDEO_PAYLOAD; ++v.count; break;
        case 2: v.flags = LLS_FRAME_KEY; break;
        case 3: ++v.capture_us; break;
        case 4: ++v.encoded_us; break;
        }
        push(a, 1, v, start + 2 + i);
    }
    uint8_t changed[LLS_MAX_VIDEO_PAYLOAD];
    memcpy(changed, data, sizeof(changed));
    changed[1] ^= 0xff;
    lls_assembler_push(a, 1, &base, changed, sizeof(changed), start + 7);
    push(a, 1, fragment(2, 0, 0, 3 * LLS_MAX_VIDEO_PAYLOAD, 3), start + 8);
    v = base;
    v.index = 1;
    v.sequence = 4;
    v.sent_us = 9999; /* Per-fragment fields legitimately differ. */
    push(a, 1, v, start + 9);
    delivered(a, start + 9, 1, base.frame_bytes);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.rejected == 7 && !stats.duplicates && !stats.frames_lost);
    one(a, 3, true, start + 10);
    dropped(a, start + 10, 2, LLS_FATE_LOST);
    expect(a, start + 10, LLS_OUT_RECOVER, 2);
    delivered(a, start + 10, 3, 17);
    lls_assembler_destroy(a);
}

static void test_join_and_resync(void)
{
    LlsAssembler *a = lls_assembler_create(NULL);
    CHECK(a);
    push(a, 1, fragment(100, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 1, 20), start);
    expect(a, start, LLS_OUT_RESET, 100);
    expect(a, start, LLS_OUT_RECOVER, 100);
    quiet(a, start);
    push(a, 1, fragment(100, 0, 1, LLS_MAX_VIDEO_PAYLOAD + 1, 21), start + 1);
    dropped(a, start + 1, 100, LLS_FATE_SKIPPED);
    one(a, 101, true, start + 2);
    delivered(a, start + 2, 101, 17);
    one(a, 102, false, start + 3);
    lls_assembler_resync(a, 101, start + 4);
    expect(a, start + 4, LLS_OUT_RECOVER, 101);
    dropped(a, start + 4, 102, LLS_FATE_SKIPPED);
    one(a, 103, true, start + 5);
    delivered(a, start + 5, 103, 17);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.frames_lost == 0 && stats.frames_skipped == 2 && stats.recover_requests == 2);
    one(a, 104, false, start + 6);
    lls_assembler_resync(a, 104, start + 7);
    push(a, 2, fragment(0, LLS_FRAME_KEY, 0, 17, 0), start + 8);
    expect(a, start + 8, LLS_OUT_RESET, 0);
    delivered(a, start + 8, 0, 17);
    quiet(a, start + 100000000);
    lls_assembler_destroy(a);
}

static void test_incomplete_initial_key(void)
{
    LlsAssembler *a = lls_assembler_create(NULL);
    CHECK(a);
    push(a, 1, fragment(0, LLS_FRAME_KEY, 0, LLS_MAX_VIDEO_PAYLOAD + 1, 0), start);
    expect(a, start, LLS_OUT_RESET, 0);
    quiet(a, start);
    push(a, 1, fragment(1, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 1, 2), start + 1);
    CHECK(lls_assembler_deadline(a) <= start + 1);
    expect(a, start + 1, LLS_OUT_RECOVER, 0);
    quiet(a, start + 1);
    push(a, 1, fragment(0, LLS_FRAME_KEY, 1, LLS_MAX_VIDEO_PAYLOAD + 1, 1), start + 2);
    delivered(a, start + 2, 0, LLS_MAX_VIDEO_PAYLOAD + 1);
    push(a, 1, fragment(1, 0, 1, LLS_MAX_VIDEO_PAYLOAD + 1, 3), start + 3);
    delivered(a, start + 3, 1, LLS_MAX_VIDEO_PAYLOAD + 1);
    CHECK(lls_assembler_deadline(a) == INT64_MAX);
    lls_assembler_destroy(a);
}

/* A frame that never arrives, followed only by partial frames: each later
 * frame times out, which also settles the missing number before it, so the
 * frames still awaiting a fate stay bounded (Intern reproducer, H4 review). */
static void test_missing_then_partial(void)
{
    LlsAssembler *a = running();
    uint32_t settled = 1, worst = 0;
    int64_t at = start;
    for (uint32_t frame = 2; frame <= 2001; ++frame) {
        at = start + (int64_t)frame * 16667000;
        push(a, 1, fragment(frame, 0, 0, LLS_MAX_VIDEO_PAYLOAD + 10, frame), at);
        LlsOutput out;
        while (lls_assembler_next(a, at, &out)) {
            if (out.kind == LLS_OUT_DROPPED) {
                CHECK(out.frame == settled && out.fate == LLS_FATE_LOST);
                settled = out.frame + out.span;
            }
        }
        CHECK(lls_assembler_deadline(a) > at);
        if (frame - settled > worst) worst = frame - settled;
    }
    CHECK(worst <= 2);
    at += 40000000;
    LlsOutput out;
    while (lls_assembler_next(a, at, &out))
        if (out.kind == LLS_OUT_DROPPED) settled = out.frame + out.span;
    CHECK(settled == 2002);
    LlsAssemblerStats stats;
    lls_assembler_stats(a, &stats);
    CHECK(stats.frames_lost == 2001);
    lls_assembler_destroy(a);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 37 ^ (i >> 8));
    test_ordered();
    test_codec();
    test_reorder_duplicates();
    test_loss_and_key();
    test_external_recovery();
    test_timeouts();
    test_missing();
    test_large_gaps();
    test_overflow();
    test_sessions_and_wrap();
    test_rejected();
    test_join_and_resync();
    test_incomplete_initial_key();
    test_missing_then_partial();
    puts("assembler: ok");
    return 0;
}
