#pragma once
// Synthetic pattern motion only; capture/conversion geometry is unchanged.
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace lls {
struct PatternCopy { unsigned source_x, destination_x, width; };

// Frame zero is unshifted. Positive motion scrolls right, wrapping within the
// full source width BEFORE the converter's top-left crop/black padding.
inline unsigned pattern_offset(uint64_t frame, unsigned pixels_per_frame, unsigned width)
{
    if (!width) throw std::invalid_argument("zero pattern width");
    return unsigned((frame % width) * (pixels_per_frame % width) % width);
}

inline std::array<PatternCopy, 2> pattern_copies(unsigned width, unsigned offset)
{
    if (!width) throw std::invalid_argument("zero pattern width");
    offset %= width;
    return {{{0, offset, width - offset}, {width - offset, 0, offset}}};
}

// Independent CPU oracle for pixel checks; not called per frame by the sender.
std::vector<uint8_t> scrolled_test_pattern(unsigned width, unsigned height, unsigned offset);
} // namespace lls
