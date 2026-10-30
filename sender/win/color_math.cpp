#include "color_math.h"
#include "pattern_motion.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lls {
static uint8_t quantize(double value)
{
    return static_cast<uint8_t>(std::floor(std::clamp(value, 0.0, 255.0) + 0.5));
}

Yuv bt709_limited(Rgb rgb)
{
    const double y = 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
    return {quantize(16.0 + 219.0 * y),
            quantize(128.0 + 112.0 * (rgb.b - y) / (1.0 - 0.0722)),
            quantize(128.0 + 112.0 * (rgb.r - y) / (1.0 - 0.2126))};
}

Rgb reference_rgb_at(const uint8_t *bgra, unsigned sw, unsigned sh, size_t stride,
                     unsigned width, unsigned height, int x, unsigned y)
{
    x = std::clamp(x, 0, int(width) - 1);
    if (unsigned(x) >= std::min(sw, width) || y >= std::min(sh, height))
        return {0, 0, 0};
    const uint8_t *p = bgra + size_t(y) * stride + size_t(x) * 4;
    return {p[2] / 255.0, p[1] / 255.0, p[0] / 255.0};
}

std::vector<uint8_t> reference_nv12(const uint8_t *bgra, unsigned sw,
    unsigned sh, size_t stride, unsigned width, unsigned height)
{
    if (!bgra || !sw || !sh || !width || !height || (width & 1) || (height & 1) ||
        stride < size_t(sw) * 4 || width > 16384 || height > 16384)
        throw std::invalid_argument("invalid NV12 reference dimensions");
    std::vector<uint8_t> out(size_t(width) * height * 3 / 2);
    auto pixel = [&](int x, unsigned y) -> Rgb {
        return reference_rgb_at(bgra, sw, sh, stride, width, height, x, y);
    };
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            out[size_t(y) * width + x] = bt709_limited(pixel(int(x), y)).y;
    for (unsigned y = 0; y < height; y += 2) {
        for (unsigned x = 0; x < width; x += 2) {
            Rgb mean{};
            for (unsigned dy = 0; dy < 2; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const Rgb p = pixel(int(x) + dx, y + dy);
                    const double weight = dx ? 0.125 : 0.25;
                    mean.r += p.r * weight;
                    mean.g += p.g * weight;
                    mean.b += p.b * weight;
                }
            }
            const Yuv uv = bt709_limited(mean);
            const size_t pos = size_t(width) * height + size_t(y / 2) * width + x;
            out[pos] = uv.u;
            out[pos + 1] = uv.v;
        }
    }
    return out;
}

std::vector<uint8_t> test_pattern(unsigned width, unsigned height)
{
    if (!width || !height || width > 16384 || height > 16384)
        throw std::invalid_argument("invalid pattern dimensions");
    const uint8_t bars[8][3] = {{255,255,255}, {255,255,0}, {0,255,255},
        {0,255,0}, {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0}};
    std::vector<uint8_t> data(size_t(width) * height * 4);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            uint8_t *p = data.data() + (size_t(y) * width + x) * 4;
            const auto &rgb = bars[size_t(x) * 8 / width];
            p[0] = rgb[2]; p[1] = rgb[1]; p[2] = rgb[0]; p[3] = 255;
            if (y >= height * 2 / 3) {
                const uint8_t grey = static_cast<uint8_t>(size_t(x) * 255 /
                    std::max(width - 1, 1u));
                p[0] = p[1] = p[2] = grey;
            }
            if (y >= height * 5 / 6) {
                p[0] = 0; p[1] = 0; p[2] = (x & 1) ? 0 : 255;
            }
        }
    }
    return data;
}

std::vector<uint8_t> scrolled_test_pattern(unsigned width, unsigned height, unsigned offset)
{
    const auto original = test_pattern(width, height); // Validates dimensions.
    offset %= width;
    if (!offset) return original;
    std::vector<uint8_t> result(original.size());
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const unsigned source_x = (x + width - offset) % width;
            for (unsigned component = 0; component < 4; ++component)
                result[(size_t(y) * width + x) * 4 + component] =
                    original[(size_t(y) * width + source_x) * 4 + component];
        }
    }
    return result;
}
} // namespace lls
