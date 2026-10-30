#include "scale_math.h"
#include "check.h"
#include <cmath>

static void geometry()
{
    auto check = [](unsigned sw, unsigned sh, unsigned dw, unsigned dh, bool near,
                    unsigned x, unsigned y, unsigned w, unsigned h) {
        const auto r = lls_fit_rect(sw, sh, dw, dh, near, 2);
        CHECK(r.x == x && r.y == y && r.width == w && r.height == h);
        CHECK(r.x + r.width <= dw && r.y + r.height <= dh);
    };
    check(1920, 1080, 1920, 1080, true, 0, 0, 1920, 1080);
    check(1800, 1000, 1920, 1080, true, 60, 40, 1800, 1000);
    check(1728, 972, 1920, 1080, true, 96, 54, 1728, 972); // inclusive 90%
    check(1280, 720, 1920, 1080, true, 0, 0, 1920, 1080);
    check(2048, 1080, 1920, 1080, true, 0, 34, 1920, 1012); // larger never cropped
    check(1024, 768, 1920, 1080, false, 240, 0, 1440, 1080);
    check(1080, 1920, 1920, 1080, false, 657, 0, 606, 1080);
    check(3840, 2160, 1280, 720, false, 0, 0, 1280, 720);
    CHECK(lls_fit_rect(0, 1080, 1920, 1080, true, 2).width == 0);
    CHECK(lls_fit_rect(8192, 1, 1280, 720, false, 1).height == 1);
}

int main()
{
    geometry();
    using lls::ScaleTransfer;
    CHECK(lls::scale_taps(1, 2) == 1);
    CHECK(lls::scale_taps(2, 1) == 1);
    CHECK(lls::scale_taps(3, 1) == 2);
    CHECK(lls::scale_taps(4, 1) == 2);
    CHECK(lls::scale_taps(8, 1) == 4);
    CHECK(lls::scale_taps(100, 1) == 4);
    const std::vector<double> source = {0, .25, .5, 1};
    for (auto transfer : {ScaleTransfer::Gamma, ScaleTransfer::Srgb, ScaleTransfer::Linear})
        CHECK(lls::reference_scale(source, 2, 2, 2, 2, transfer) == source);
    const auto average = lls::reference_scale(source, 2, 2, 1, 1, ScaleTransfer::Gamma);
    CHECK(std::abs(average[0] - .4375) < 1e-12);
    const auto srgb = lls::reference_scale({0, 1, 0, 1}, 2, 2, 1, 1, ScaleTransfer::Srgb);
    CHECK(std::abs(srgb[0] - .7353569830524495) < 1e-12);
    // FP16 filtering preserves linear highlights; only the NV12 pass clips.
    CHECK(lls::reference_scale({0, 2, 0, 2}, 2, 2, 1, 1, ScaleTransfer::Linear)[0] == 1);
    for (unsigned n : {2u, 4u, 8u}) {
        std::vector<double> ramp(n * n);
        double sum = 0;
        for (unsigned i = 0; i < ramp.size(); ++i) sum += ramp[i] = double(i) / ramp.size();
        const auto scaled = lls::reference_scale(ramp, n, n, 1, 1, ScaleTransfer::Gamma);
        CHECK(std::abs(scaled[0] - sum / ramp.size()) < 1e-12);
    }
    const auto bars = lls::reference_scale({1, 1}, 2, 1, 4, 4, ScaleTransfer::Gamma);
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
        CHECK(bars[y * 4 + x] == ((y == 1 || y == 2) ? 1 : 0));
    const auto edge = lls::reference_scale({.3}, 1, 1, 4, 4, ScaleTransfer::Gamma);
    for (double v : edge) CHECK(std::abs(v - .3) < 1e-12);
    puts("scale: fit/10% rule, exact copies, 2/4/8 box averages, transfer functions and black bars verified");
}
