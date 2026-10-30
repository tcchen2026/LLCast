#pragma once
// Portable reference for the shader: gamma-coded RGB -> BT.709 limited NV12.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lls {
struct Rgb { double r, g, b; }; // normalized, already gamma-coded
struct Yuv { uint8_t y, u, v; };
Yuv bt709_limited(Rgb rgb);
// Border sampler used by the reference conversion. Valid nonempty dimensions
// and BGRA storage are required. Extend output left/right edges; keep missing
// desktop pixels black, including padding at the output's right edge.
Rgb reference_rgb_at(const uint8_t *bgra, unsigned source_width,
    unsigned source_height, size_t stride, unsigned width, unsigned height,
    int x, unsigned y);
// Output dimensions must be positive/even. Crop top-left; black-pad missing RGB.
// Chroma location 0: horizontally left, vertically centred, [1 2 1]/4 x [1 1]/2.
std::vector<uint8_t> reference_nv12(const uint8_t *bgra, unsigned source_width,
    unsigned source_height, size_t stride, unsigned width, unsigned height);
// Full-intensity bars, greys and single-pixel edges for later GPU/pixel checks.
std::vector<uint8_t> test_pattern(unsigned width, unsigned height);
} // namespace lls
