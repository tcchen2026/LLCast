#include "scale_math.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lls {
unsigned scale_taps(unsigned source, unsigned fitted)
{
    if (!source || !fitted) return 1;
    return unsigned(std::clamp((uint64_t(source) + 2ull * fitted - 1) / (2ull * fitted), 1ull, 4ull));
}

static double decode(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
static double encode(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

std::vector<double> reference_scale(const std::vector<double> &source, unsigned sw,
    unsigned sh, unsigned dw, unsigned dh, ScaleTransfer transfer)
{
    if (!sw || !sh || !dw || !dh || source.size() != size_t(sw) * sh)
        throw std::invalid_argument("Invalid reference scale dimensions");
    if (sw == dw && sh == dh) return source; // Exact copy, including _SRGB.
    const LlsRect fit = lls_fit_rect(sw, sh, dw, dh, false, 1);
    const unsigned nx = scale_taps(sw, fit.width), ny = scale_taps(sh, fit.height);
    const double rx = double(sw) / fit.width, ry = double(sh) / fit.height;
    auto at = [&](int x, int y) {
        const double v = source[size_t(std::clamp(y, 0, int(sh) - 1)) * sw +
                                std::clamp(x, 0, int(sw) - 1)];
        return transfer == ScaleTransfer::Srgb ? decode(v) : v;
    };
    auto bilinear = [&](double x, double y) {
        x -= .5; y -= .5;
        int ix = int(std::floor(x)), iy = int(std::floor(y));
        double fx = x - ix, fy = y - iy;
        return (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
               (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
    };
    std::vector<double> output(size_t(dw) * dh, 0);
    for (unsigned y = 0; y < fit.height; ++y) for (unsigned x = 0; x < fit.width; ++x) {
        double sum = 0;
        for (unsigned j = 0; j < ny; ++j) for (unsigned i = 0; i < nx; ++i)
            sum += bilinear((x + (i + .5) / nx) * rx, (y + (j + .5) / ny) * ry);
        double v = sum / (nx * ny);
        output[size_t(y + fit.y) * dw + x + fit.x] = transfer == ScaleTransfer::Srgb ? encode(v) : v;
    }
    return output;
}
} // namespace lls
