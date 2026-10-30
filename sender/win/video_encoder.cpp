#include "video_encoder.h"
#include "nvenc_encoder.h"
#include "mf_encoder.h"
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>

namespace lls {
std::unique_ptr<VideoEncoder> open_encoder(ID3D11Device *device, ID3D11Texture2D *input,
    EncoderConfig config, uint32_t receiver_codecs, EncoderBackend backend)
{
    if (config.codec > LLS_CODEC_AV1) return nullptr;
    if (config.codec) receiver_codecs &= LLS_CODEC_BIT(config.codec);
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    const bool nvidia = device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) && desc.VendorId == 0x10de;
    if (backend != EncoderBackend::MediaFoundation && nvidia) {
        uint32_t remaining = receiver_codecs &
            (LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264) | LLS_CODEC_BIT(LLS_CODEC_AV1));
        while (const uint8_t codec = lls_codec_pick(config.codec, remaining, receiver_codecs)) {
            auto encoder = std::make_unique<NvencEncoder>();
            EncoderConfig choice = config;
            choice.codec = codec;
            if (encoder->open(device, input, choice)) return encoder;
            remaining &= ~LLS_CODEC_BIT(codec);
        }
    }
    if (backend != EncoderBackend::Nvenc) {
        uint32_t remaining = receiver_codecs & (LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264) | LLS_CODEC_BIT(LLS_CODEC_AV1));
        while (const uint8_t codec = lls_codec_pick(config.codec, remaining, receiver_codecs)) {
            auto encoder = std::make_unique<MfEncoder>();
            EncoderConfig choice = config;
            choice.codec = codec;
            if (encoder->open(device, input, choice)) return encoder;
            remaining &= ~LLS_CODEC_BIT(codec);
        }
    }
    std::fprintf(stderr, "No compatible hardware video encoder on this display's GPU (receiver codecs=0x%x)\n", receiver_codecs);
    return nullptr;
}
} // namespace lls
