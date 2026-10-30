#pragma once
// Desktop Duplication capture of one SDR output, BGRA8 or linear scRGB FP16.
// The process must
// run on the logged-in console desktop: from a service, SSH or RDP session,
// DuplicateOutput fails or captures the wrong display.

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdint>

namespace lls {

struct CapturedFrame {
    ID3D11Texture2D *texture = nullptr; // valid until DdaCapture::release()
    uint32_t width = 0, height = 0;     // desktop size, before any crop
    int64_t present_qpc = 0;            // DXGI_OUTDUPL_FRAME_INFO::LastPresentTime
    uint32_t accumulated = 0;           // desktop updates folded into this image
};

class DdaCapture {
public:
    enum class Result { Frame, Timeout, Lost, Error };

    // Creates the D3D11 device (BGRA support, video support) on the adapter
    // that owns the chosen output. Encoder and converter share this device.
    // output_index < 0 selects the primary display: the output whose desktop
    // coordinates start at (0, 0).
    bool open(int output_index = -1);
    ID3D11Device *device() const { return device_.Get(); }
    ID3D11DeviceContext *context() const { return context_.Get(); }

    // Waits up to timeout_ms for a desktop image update. Pointer-only updates
    // are not frames. Lost means the duplication was invalidated (mode change,
    // secure desktop, fullscreen switch); call reopen() and continue.
    Result acquire(unsigned timeout_ms, CapturedFrame &frame);
    // Call once the GPU work reading frame.texture has been submitted.
    void release();
    bool reopen(bool log = true);
    const char *pause_reason() const { return pause_reason_; } // static text, no ownership

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutput5> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frame_; // the acquired image, until release()
    bool holding_ = false;
    const char *pause_reason_ = nullptr;
};

} // namespace lls
