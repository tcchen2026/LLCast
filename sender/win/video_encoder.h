#pragma once
// One synchronous frame in flight over NVENC or an asynchronous hardware MFT.
#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include "llstream_proto.h"

namespace lls {
enum class EncoderBackend { Auto, Nvenc, MediaFoundation };
struct EncoderConfig {
    uint32_t width = 1920, height = 1080, fps = 60, bitrate_bps = 30000000;
    uint8_t codec = 0; // zero selects the preferred common codec
};
struct EncodedFrame {
    const uint8_t *data = nullptr; // until next encode or destruction
    size_t size = 0;
    bool key = false;
    int64_t done_qpc = 0; // bitstream available, QPC ticks
    int64_t submit_qpc = 0; // immediately before the encoder input API, QPC ticks
};
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    virtual bool encode(bool key, EncodedFrame &frame, const volatile long &stop) = 0;
    virtual uint8_t codec() const = 0;
    virtual const char *name() const = 0;
};
std::unique_ptr<VideoEncoder> open_encoder(ID3D11Device *device, ID3D11Texture2D *input,
    EncoderConfig config, uint32_t receiver_codecs, EncoderBackend backend = EncoderBackend::Auto);
} // namespace lls
