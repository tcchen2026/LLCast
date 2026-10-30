#pragma once
#include "lls_geometry.h"
#include <vector>

namespace lls {
enum class ScaleTransfer { Gamma, Srgb, Linear };
unsigned scale_taps(unsigned source, unsigned fitted);
// Scalar reference of the GPU filter, pixel centres at n+0.5. Channels are
// independent; Srgb decodes before interpolation then re-encodes on output.
std::vector<double> reference_scale(const std::vector<double> &source, unsigned sw,
    unsigned sh, unsigned dw, unsigned dh, ScaleTransfer transfer);
} // namespace lls
