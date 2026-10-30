#pragma once
#include "video_encoder.h"
#include <string>

namespace lls {
class MfEncoder : public VideoEncoder {
public:
    MfEncoder();
    MfEncoder(const MfEncoder &) = delete;
    MfEncoder &operator=(const MfEncoder &) = delete;
    ~MfEncoder();
    bool open(ID3D11Device *device, ID3D11Texture2D *input, const EncoderConfig &config);
    bool encode(bool key, EncodedFrame &frame, const volatile long &stop) override;
    uint8_t codec() const override;
    const char *name() const override;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace lls
