#include "color_math.h"
#include "frame_policy.h"
#include "pattern_motion.h"

#include "check.h"
#include <climits>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void color_vectors()
{
    // BT.709 studio-swing 8-bit reference vectors (round to nearest).
    const lls::Rgb rgb[] = {{0,0,0},{1,1,1},{1,0,0},{0,1,0},{0,0,1},
        {1,1,0},{0,1,1},{1,0,1},{0.5,0.5,0.5}};
    const lls::Yuv expected[] = {{16,128,128},{235,128,128},{63,102,240},
        {173,42,26},{32,240,118},{219,16,138},{188,154,16},
        {78,214,230},{126,128,128}};
    for (size_t i = 0; i < sizeof(rgb) / sizeof(rgb[0]); ++i) {
        const auto v = lls::bt709_limited(rgb[i]);
        CHECK(v.y == expected[i].y && v.u == expected[i].u && v.v == expected[i].v);
    }
}

static void geometry_and_chroma()
{
    // 3x1 red input, odd dimensions: right/bottom must become limited black.
    const uint8_t red[] = {0,0,255,0, 0,0,255,0, 0,0,255,0, 99,99,99,99};
    const auto out = lls::reference_nv12(red, 3, 1, 16, 6, 4);
    CHECK(out.size() == 36);
    for (unsigned i = 0; i < 24; ++i) CHECK(out[i] == (i < 3 ? 63 : 16));
    CHECK(out[24] == 115 && out[25] == 184); // vertically half red
    CHECK(out[26] == 118 && out[27] == 170); // 3/8 red at padding boundary
    CHECK(out[28] == 128 && out[29] == 128);
    for (unsigned i = 30; i < 36; ++i) CHECK(out[i] == 128);

    // Chroma is centred on even luma x, not halfway between x and x+1.
    std::vector<uint8_t> stripes(4 * 2 * 4, 0);
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 4; x += 2) stripes[(y * 4 + x) * 4 + 2] = 255;
    const auto uv = lls::reference_nv12(stripes.data(), 4, 2, 16, 4, 2);
    CHECK(uv[8] == 109 && uv[9] == 212); // left edge: 3/4 red
    CHECK(uv[10] == 115 && uv[11] == 184); // interior: 1/2 red
    const auto cropped = lls::reference_nv12(red, 3, 1, 16, 2, 2);
    CHECK(cropped[0] == 63 && cropped[1] == 63);
    CHECK(cropped[2] == 16 && cropped[3] == 16);
}

static void output_border_sampling()
{
    // A four-column crop ends in red; the cropped-away desktop is blue.
    // Sampling beyond the output must extend red, not expose blue or add black.
    const uint8_t pixels[] = {0,255,0,255, 0,0,255,255, 0,0,255,255,
                             0,0,255,255, 255,0,0,255, 255,0,0,255};
    for (unsigned sw : {4u, 6u}) {
        for (int x : {3, 4, 5, INT_MAX}) {
            const auto rgb = lls::reference_rgb_at(pixels, sw, 1, sizeof(pixels), 4, 2, x, 0);
            CHECK(rgb.r == 1 && rgb.g == 0 && rgb.b == 0);
        }
        const auto left = lls::reference_rgb_at(pixels, sw, 1, sizeof(pixels), 4, 2, INT_MIN, 0);
        CHECK(left.r == 0 && left.g == 1 && left.b == 0);
    }
    // A narrower desktop leaves black padding. Clamp to the OUTPUT edge, not
    // to the last desktop pixel, even for a tap beyond the output's right edge.
    for (int x : {2, 3, 4, INT_MAX}) {
        const auto rgb = lls::reference_rgb_at(pixels, 2, 1, sizeof(pixels), 4, 2, x, 0);
        CHECK(rgb.r == 0 && rgb.g == 0 && rgb.b == 0);
    }
    const auto below = lls::reference_rgb_at(pixels, 4, 1, sizeof(pixels), 4, 2, 4, 1);
    CHECK(below.r == 0 && below.g == 0 && below.b == 0);
}

static void clock_and_policy()
{
    CHECK(lls::qpc_us(15000001, 15000000) == 1000000);
    CHECK(lls::qpc_us(123456789, 10000000) == 12345678);
    CHECK(lls::qpc_us(UINT64_C(9000000000000000), 10000000) ==
        UINT64_C(900000000000000));
    lls::FramePolicy policy;
    CHECK(policy.may_acquire(0) && !policy.must_repeat(0, true));
    CHECK(lls_frame_policy_deadline(&policy, false) == UINT64_MAX);
    CHECK(lls_frame_policy_deadline(&policy, true) == 0);
    policy.encoded(100000, false);
    CHECK(lls_frame_policy_deadline(&policy, true) == 114666);
    CHECK(lls_frame_policy_deadline(&policy, false) == 117666);
    CHECK(!policy.may_acquire(114665) && policy.may_acquire(114666));
    CHECK(!policy.must_repeat(117665, false) && policy.must_repeat(117666, false));
    CHECK(policy.must_repeat(100001, true));
    /* Static content: repeats are ~0.7 ms late each time (as on the PC), yet
     * 600 of them span exactly 600 periods after the first: 60 fps. */
    uint64_t due = 117666, first = 0, now = 0;
    for (int i = 0; i < 600; ++i) {
        now = due + 700;
        CHECK(policy.must_repeat(now, false) && !policy.must_repeat(due - 1, false));
        if (!i) first = now;
        policy.encoded(now, true);
        due = policy.repeat_due_us;
    }
    CHECK(now - first == 599 * lls::FramePolicy::period_us);
    /* An early repeat (recovery IDR) restarts the grid one period later. */
    policy.encoded(now + 5000, true);
    CHECK(policy.repeat_due_us == now + 5000 + lls::FramePolicy::period_us);
    // A capture that completes just after a repeat remains pending. It must
    // encode at the next acquire gate, even without any further desktop update.
    lls::FramePolicy late;
    late.encoded(100000, false);
    late.encoded(117666, true);
    CHECK(!lls_frame_policy_should_encode(&late, 117667, true, false));
    CHECK(lls_frame_policy_deadline(&late, true) == 132332);
    CHECK(lls_frame_policy_should_encode(&late, 132332, true, false));
    CHECK(lls_frame_policy_should_encode(&late, 117667, true, true));
    CHECK(lls::capture_timeout_ms(114666, 117666) == 2);
    CHECK(lls::capture_timeout_ms(117000, 117666) == 0);
}

static void pattern_motion()
{
    CHECK(lls::pattern_offset(0, 8, 1920) == 0);
    CHECK(lls::pattern_offset(59, 8, 1920) == 472);
    CHECK(lls::pattern_offset(240, 8, 1920) == 0);
    CHECK(lls::pattern_offset(UINT64_MAX, UINT_MAX, 1920) ==
          ((UINT64_MAX % 1920) * (uint64_t(UINT_MAX) % 1920)) % 1920);
    for (unsigned width : {1u, 2u, 3u, 17u, 1920u}) {
        const unsigned height = 7;
        const auto base = lls::test_pattern(width, height);
        for (unsigned offset : {0u, 1u, width - 1, width, width + 1, UINT_MAX}) {
            // Emulate D3D's two copies, checking coverage against an independent
            // per-pixel modulo reference. Odd widths exercise wrap boundaries.
            std::vector<uint8_t> copied(base.size(), 77);
            for (const auto &copy : lls::pattern_copies(width, offset)) {
                CHECK(copy.source_x + copy.width <= width && copy.destination_x + copy.width <= width);
                for (unsigned y = 0; y < height; ++y)
                    std::memcpy(copied.data() + (size_t(y) * width + copy.destination_x) * 4,
                                base.data() + (size_t(y) * width + copy.source_x) * 4, size_t(copy.width) * 4);
            }
            CHECK(copied == lls::scrolled_test_pattern(width, height, offset));
        }
    }
    // Shift happens before crop/padding. The rightmost source pixel wraps
    // into output x=0; absent source rows/columns still become limited black.
    auto shifted = lls::scrolled_test_pattern(3, 1, 1);
    const auto padded = lls::reference_nv12(shifted.data(), 3, 1, 12, 6, 2);
    CHECK(padded[0] == lls::bt709_limited({shifted[2]/255.0, shifted[1]/255.0, shifted[0]/255.0}).y);
    for (unsigned x = 3; x < 12; ++x) CHECK(padded[x] == 16);
    shifted = lls::scrolled_test_pattern(6, 2, 2);
    const auto cropped = lls::reference_nv12(shifted.data(), 6, 2, 24, 4, 2);
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 4; ++x) {
            const auto p = shifted.data() + (y * 6 + x) * 4;
            CHECK(cropped[y * 4 + x] == lls::bt709_limited({p[2]/255.0, p[1]/255.0, p[0]/255.0}).y);
        }
}

static unsigned option(const char *text)
{
    char *end;
    errno = 0;
    const auto value = std::strtoul(text, &end, 10);
    if (errno || text == end || *end || *text == '-' || value > UINT_MAX)
        throw std::invalid_argument("invalid reference dimension/offset");
    return unsigned(value);
}

int main(int argc, char **argv)
{
    color_vectors(); geometry_and_chroma(); output_border_sampling(); clock_and_policy(); pattern_motion();
    if (argc == 5 || argc == 7) {
        // GPU validation oracle: --reference FILE SW SH [--offset X]
        if (std::string(argv[1]) != "--reference") return 2;
        if (argc == 7 && std::string(argv[5]) != "--offset") return 2;
        const unsigned sw = option(argv[3]), sh = option(argv[4]);
        const auto bgra = lls::scrolled_test_pattern(sw, sh, argc == 7 ? option(argv[6]) : 0);
        const auto nv12 = lls::reference_nv12(bgra.data(), sw, sh, size_t(sw) * 4,
                                             1920, 1080);
        FILE *f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        const bool written = std::fwrite(nv12.data(), 1, nv12.size(), f) == nv12.size();
        const bool closed = std::fclose(f) == 0;
        return written && closed ? 0 : 1;
    }
    CHECK(argc == 1);
    std::puts("BT.709, crop/padding, chroma location, QPC, frame policy and pattern motion: OK");
}
