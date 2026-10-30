#pragma once
// Synchronous HEVC/H.264/AV1 encoding of one registered D3D11 NV12 texture.
// Loads nvEncodeAPI64.dll at runtime and checks the driver's API version.
// Settings follow docs/PLAN.md: P1, ultra-low-latency tuning, CBR, VBV of
// one frame, infinite GOP, P-frames only, no lookahead, recovery parameter
// sets with every keyframe, BT.709 limited and left chroma.

#include "video_encoder.h"

namespace lls {

class NvencEncoder : public VideoEncoder {
public:
    NvencEncoder() = default;
    NvencEncoder(const NvencEncoder &) = delete;
    NvencEncoder &operator=(const NvencEncoder &) = delete;
    ~NvencEncoder() { close(); }

    bool open(ID3D11Device *device, ID3D11Texture2D *input, const EncoderConfig &config);
    // Encodes the current contents of the input texture and waits for the
    // bitstream. force_idr also emits the codec's recovery parameter sets.
    bool encode(bool force_idr, EncodedFrame &frame, const volatile long &stop) override;
    uint8_t codec() const override;
    const char *name() const override { return "NVENC"; }
    void close();

private:
    struct State;
    State *state_ = nullptr;
};

} // namespace lls
