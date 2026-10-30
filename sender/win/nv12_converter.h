#pragma once
// GPU conversion of a BGRA8 or SDR linear scRGB FP16 desktop into NV12:
// BT.709 limited range, chroma sited left (HEVC chroma_loc_type 0).
// Scale the whole source into a private aspect-fit image, or copy its top-left
// region when scaling is disabled. Equal sizes always copy exactly. NV12
// conversion reads only the private image; release capture after convert().

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <array>

namespace lls {

class Nv12Converter {
public:
    // Fails if the device cannot render to NV12 plane views; that case needs a
    // video-processor fallback, so report it rather than degrading silently.
    bool open(ID3D11Device *device, uint32_t width, uint32_t height, bool scale = true);
    // Queues the conversion; no CPU wait. FP16 is SDR linear scRGB (white=1).
    bool convert(ID3D11DeviceContext *context, ID3D11Texture2D *source);
    // Drop retained desktop surfaces on duplication loss/reopen. Private
    // converted NV12 stays valid for repeats while capture is unavailable.
    void clear_source_cache();
    // NV12, BIND_RENDER_TARGET; registered once with the encoder.
    ID3D11Texture2D *output() const { return output_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_copy_;          // width x height, matching source format
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_view_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> source_target_;
    struct SourceView {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    };
    std::array<SourceView, 4> source_views_{}; // bounded; retaining the SRV protects texture identity
    unsigned next_source_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sample_copy_; // for sources without SHADER_RESOURCE
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sample_view_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> scale_shader_, scale_srgb_shader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> scale_constants_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> luma_, chroma_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> luma_shader_, chroma_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> luma_linear_shader_, chroma_linear_shader_;
    uint32_t width_ = 0, height_ = 0;
    uint32_t source_width_ = 0, source_height_ = 0;
    DXGI_FORMAT source_format_ = DXGI_FORMAT_UNKNOWN;
    bool scale_ = true;
};

} // namespace lls
