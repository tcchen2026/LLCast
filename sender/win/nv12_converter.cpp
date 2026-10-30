#include "nv12_converter.h"
#include "nv12_shaders.h"
#include "scale_shaders.h"
#include "scale_math.h"
#include "win_util.h"
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace lls {
struct ScaleConstants {
    float inverse_source[2], ratio[2], origin[2], fit_size[2];
    int32_t taps[2];
    float padding[2];
};
static_assert(sizeof(ScaleConstants) == 48, "HLSL constant buffer layout");

static bool create_source(ID3D11Device *device, UINT width, UINT height, DXGI_FORMAT format,
                          ComPtr<ID3D11Texture2D> &texture, ComPtr<ID3D11ShaderResourceView> &view,
                          ComPtr<ID3D11RenderTargetView> *target = nullptr)
{
    view.Reset(); texture.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    return hr_ok(device->CreateTexture2D(&desc, nullptr, &texture), "private source copy texture") &&
        hr_ok(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "private source shader view") &&
        (!target || hr_ok(device->CreateRenderTargetView(texture.Get(), nullptr, target->ReleaseAndGetAddressOf()),
                          "private source render target"));
}

bool Nv12Converter::open(ID3D11Device *device, uint32_t width, uint32_t height, bool scale)
{
    output_.Reset(); source_copy_.Reset(); source_view_.Reset();
    source_target_.Reset(); sample_copy_.Reset(); sample_view_.Reset();
    source_views_ = {}; next_source_ = 0;
    scale_shader_.Reset(); scale_srgb_shader_.Reset(); sampler_.Reset(); scale_constants_.Reset();
    luma_.Reset(); chroma_.Reset(); vertex_.Reset(); luma_shader_.Reset(); chroma_shader_.Reset();
    luma_linear_shader_.Reset(); chroma_linear_shader_.Reset();
    width_ = height_ = 0;
    source_width_ = source_height_ = 0;
    scale_ = scale;
    if (!device || !width || !height || (width & 1) || (height & 1) || width > 16384 || height > 16384)
        return false;
    UINT support = 0;
    if (!hr_ok(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support), "CheckFormatSupport(NV12)"))
        return false;
    std::fprintf(stderr, "NV12 format support flags=0x%x; probing texture and R8/R8G8 RTVs\n", support);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.Format = DXGI_FORMAT_NV12;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (!hr_ok(device->CreateTexture2D(&desc, nullptr, &output_), "NV12 render-target texture")) {
        std::fprintf(stderr, "NV12 plane rendering unavailable; a video-processor fallback is needed\n");
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    view.Format = DXGI_FORMAT_R8_UNORM;
    HRESULT hr = device->CreateRenderTargetView(output_.Get(), &view, &luma_);
    if (SUCCEEDED(hr)) {
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        hr = device->CreateRenderTargetView(output_.Get(), &view, &chroma_);
    }
    if (!hr_ok(hr, "NV12 plane RTVs")) {
        std::fprintf(stderr, "NV12 plane rendering unavailable; a video-processor fallback is needed\n");
        return false;
    }
    if (!create_source(device, width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                       source_copy_, source_view_, &source_target_)) return false;

    char width_text[16], height_text[16];
    std::snprintf(width_text, sizeof(width_text), "%u", width);
    std::snprintf(height_text, sizeof(height_text), "%u", height);
    const char *entries[] = {"vertex", "luma", "chroma", "luma", "chroma"};
    for (unsigned i = 0; i < 5; ++i) {
        const D3D_SHADER_MACRO macros[] = {{"OUTPUT_WIDTH", width_text},
            {"OUTPUT_HEIGHT", height_text}, {"INPUT_LINEAR", i >= 3 ? "1" : "0"}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> code, errors;
        hr = D3DCompile(nv12_hlsl, std::strlen(nv12_hlsl), "nv12_shaders.h", macros,
            nullptr, entries[i], i == 0 ? "vs_5_0" : "ps_5_0",
            // Warnings are logged, not fatal: a stricter system compiler must
            // not make the sender unusable.
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors) std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
        if (!hr_ok(hr, "D3DCompile(NV12)")) return false;
        if (i == 0)
            hr = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex_);
        else {
            auto &shader = i == 1 ? luma_shader_ : i == 2 ? chroma_shader_ :
                           i == 3 ? luma_linear_shader_ : chroma_linear_shader_;
            hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                          &shader);
        }
        if (!hr_ok(hr, "Create NV12 shader")) return false;
    }
    if (scale_) {
        for (unsigned i = 0; i < 2; ++i) {
            const D3D_SHADER_MACRO macros[] = {{"REENCODE_SRGB", i ? "1" : "0"}, {nullptr, nullptr}};
            ComPtr<ID3DBlob> code, errors;
            hr = D3DCompile(scale_hlsl, std::strlen(scale_hlsl), "scale_shaders.h", macros,
                nullptr, "scale", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &code, &errors);
            if (errors) std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
            if (!hr_ok(hr, "D3DCompile(scale)")) return false;
            auto &shader = i ? scale_srgb_shader_ : scale_shader_;
            if (!hr_ok(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                 nullptr, &shader), "Create scale shader")) return false;
        }
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(ScaleConstants);
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!hr_ok(device->CreateSamplerState(&sampler, &sampler_), "Scale sampler") ||
            !hr_ok(device->CreateBuffer(&constants, nullptr, &scale_constants_), "Scale constants")) return false;
    }
    width_ = width; height_ = height;
    std::fprintf(stderr, "NV12 %ux%u ready; %s, BT.709 limited, chroma left\n", width, height,
                 scale_ ? "aspect-fit GPU scale, exact copy at equal size" : "top-left GPU copy, no scaling");
    return true;
}

void Nv12Converter::clear_source_cache()
{
    source_views_ = {};
    next_source_ = 0;
    sample_view_.Reset();
    sample_copy_.Reset();
    source_width_ = source_height_ = 0;
    source_format_ = DXGI_FORMAT_UNKNOWN;
}

bool Nv12Converter::convert(ID3D11DeviceContext *context, ID3D11Texture2D *source)
{
    if (!context || !source || !width_ || !height_) return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const bool linear = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if ((!linear && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
        desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || !desc.Width || !desc.Height ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        std::fprintf(stderr, "Converter needs single-sample BGRA8/FP16 and an immediate D3D11 context\n");
        return false;
    }
    ComPtr<ID3D11Device> device, source_device, copy_device;
    context->GetDevice(&device); source->GetDevice(&source_device); source_copy_->GetDevice(&copy_device);
    if (device.Get() != source_device.Get() || device.Get() != copy_device.Get()) {
        std::fprintf(stderr, "Capture, converter and encoder must use the same D3D11 device\n");
        return false;
    }
    ID3D11ShaderResourceView *none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (source_width_ != desc.Width || source_height_ != desc.Height || source_format_ != desc.Format)
        clear_source_cache();
    D3D11_TEXTURE2D_DESC copy_desc{};
    source_copy_->GetDesc(&copy_desc);
    const DXGI_FORMAT copy_format = linear ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (copy_desc.Format != copy_format) {
        if (!create_source(device.Get(), width_, height_, copy_format,
                           source_copy_, source_view_, &source_target_)) return false;
        source_width_ = source_height_ = 0;
    }
    const bool changed = source_width_ != desc.Width || source_height_ != desc.Height;
    source_format_ = desc.Format;
    const bool scaling = scale_ && (desc.Width != width_ || desc.Height != height_);
    if (changed && (scaling || desc.Width < width_ || desc.Height < height_)) {
        const FLOAT black[] = {0, 0, 0, 1};
        context->ClearRenderTargetView(source_target_.Get(), black);
    }
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(), nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    if (scaling) {
        ID3D11ShaderResourceView *sample = nullptr;
        if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            for (const auto &entry : source_views_)
                if (entry.texture.Get() == source) sample = entry.view.Get();
            if (!sample) {
                auto &entry = source_views_[next_source_++ % source_views_.size()];
                entry = {};
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = desc.Format;
                view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                view.Texture2D.MipLevels = 1;
                if (!hr_ok(device->CreateShaderResourceView(source, &view, &entry.view), "Scale source SRV"))
                    return false;
                entry.texture = source;
                sample = entry.view.Get();
            }
        } else {
            // Some duplication surfaces cannot be bound as shader inputs.
            // Keep a single reusable full-source copy for that driver path.
            D3D11_TEXTURE2D_DESC previous{};
            if (sample_copy_) sample_copy_->GetDesc(&previous);
            if (previous.Width != desc.Width || previous.Height != desc.Height || previous.Format != desc.Format)
                if (!create_source(device.Get(), desc.Width, desc.Height, desc.Format, sample_copy_, sample_view_))
                    return false;
            const D3D11_BOX whole = {0, 0, 0, desc.Width, desc.Height, 1};
            context->CopySubresourceRegion(sample_copy_.Get(), 0, 0, 0, 0, source, 0, &whole);
            sample = sample_view_.Get();
        }
        const LlsRect fit = lls_fit_rect(desc.Width, desc.Height, width_, height_, false, 1);
        if (changed) {
            const ScaleConstants constants = {{1.f / desc.Width, 1.f / desc.Height},
                {float(desc.Width) / fit.width, float(desc.Height) / fit.height},
                {float(fit.x), float(fit.y)}, {float(fit.width), float(fit.height)},
                {int32_t(scale_taps(desc.Width, fit.width)), int32_t(scale_taps(desc.Height, fit.height))}, {0, 0}};
            context->UpdateSubresource(scale_constants_.Get(), 0, nullptr, &constants, 0, 0);
        }
        ID3D11RenderTargetView *target = source_target_.Get();
        ID3D11SamplerState *sampler = sampler_.Get();
        ID3D11Buffer *constants = scale_constants_.Get();
        const D3D11_VIEWPORT viewport = {float(fit.x), float(fit.y), float(fit.width), float(fit.height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &target, nullptr);
        context->PSSetShaderResources(0, 1, &sample);
        context->PSSetSamplers(0, 1, &sampler);
        context->PSSetConstantBuffers(0, 1, &constants);
        context->PSSetShader(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ?
                             scale_srgb_shader_.Get() : scale_shader_.Get(), nullptr, 0);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->PSSetShaderResources(0, 1, &none);
    } else {
        // Copy compatible BGRA formats as bits: no hidden sRGB conversion.
        const D3D11_BOX region = {0, 0, 0, std::min(desc.Width, width_), std::min(desc.Height, height_), 1};
        context->CopySubresourceRegion(source_copy_.Get(), 0, 0, 0, 0, source, 0, &region);
    }
    source_width_ = desc.Width; source_height_ = desc.Height;
    ID3D11ShaderResourceView *input = source_view_.Get();
    context->PSSetShaderResources(0, 1, &input);
    for (unsigned plane = 0; plane < 2; ++plane) {
        ID3D11RenderTargetView *target = plane ? chroma_.Get() : luma_.Get();
        const D3D11_VIEWPORT viewport = {0, 0, FLOAT(width_ >> plane), FLOAT(height_ >> plane), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &target, nullptr);
        ID3D11PixelShader *shader = linear ? (plane ? chroma_linear_shader_.Get() : luma_linear_shader_.Get()) :
                                            (plane ? chroma_shader_.Get() : luma_shader_.Get());
        context->PSSetShader(shader, nullptr, 0);
        context->Draw(3, 0);
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->PSSetShaderResources(0, 1, &none);
    context->Flush(); // Submit the copy/draws before ReleaseFrame; no CPU fence wait.
    return hr_ok(device->GetDeviceRemovedReason(), "Converter device status");
}
} // namespace lls
