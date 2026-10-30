#include "dda_capture.h"
#include "win_util.h"
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

using Microsoft::WRL::ComPtr;
namespace lls {
static const char *duplication_error(HRESULT hr)
{
    switch (hr) {
    case E_ACCESSDENIED: return "current desktop is inaccessible (locked/secure desktop?)";
    case E_INVALIDARG: return "wrong adapter/device or output already duplicated";
    case DXGI_ERROR_SESSION_DISCONNECTED: return "session is disconnected";
    case DXGI_ERROR_UNSUPPORTED: return "unsupported desktop mode or capture session";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "too many concurrent desktop duplicators";
    case DXGI_ERROR_ACCESS_LOST: return "desktop/mode changed; duplication must reopen";
    default: return "see HRESULT and device-removal status";
    }
}

static bool console_desktop()
{
    DWORD session = 0, size = 0;
    wchar_t station[256]{};
    const DWORD console = WTSGetActiveConsoleSessionId();
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) ||
        !GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
                                  station, sizeof(station), &size)) {
        std::fprintf(stderr, "Cannot identify process session/window station (error %lu)\n", GetLastError());
        return false;
    }
    std::fprintf(stderr, "Capture session=%lu console=%lu window_station=%ls remote=%d\n",
                 session, console, station, GetSystemMetrics(SM_REMOTESESSION));
    wchar_t thread_name[256] = L"unknown", input_name[256] = L"unavailable";
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                             thread_name, sizeof(thread_name), &size);
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (input) {
        GetUserObjectInformationW(input, UOI_NAME, input_name, sizeof(input_name), &size);
        CloseDesktop(input);
    }
    std::fprintf(stderr, "Capture thread_desktop=%ls input_desktop=%ls\n", thread_name, input_name);
    if (!session || session != console || GetSystemMetrics(SM_REMOTESESSION) ||
        _wcsicmp(station, L"WinSta0") != 0) {
        std::fprintf(stderr, "Run the EXE/batch file on the logged-in physical console desktop\n");
        return false;
    }
    return true;
}

bool DdaCapture::open(int output_index)
{
    release();
    duplication_.Reset(); output_.Reset(); context_.Reset(); device_.Reset();
    if (!console_desktop()) return false;
    ComPtr<IDXGIFactory1> factory;
    if (!hr_ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return false;
    int ordinal = 0;
    for (UINT a = 0; ; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(a, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (!hr_ok(hr, "EnumAdapters1")) return false;
        DXGI_ADAPTER_DESC1 ad{};
        if (!hr_ok(adapter->GetDesc1(&ad), "GetAdapterDesc1")) return false;
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(i, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (!hr_ok(hr, "EnumOutputs")) return false;
            DXGI_OUTPUT_DESC desc{};
            if (!hr_ok(output->GetDesc(&desc), "GetOutputDesc")) return false;
            if (!desc.AttachedToDesktop) continue;
            const bool primary = desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;
            const int current = ordinal++;
            if (output_index < 0 ? !primary : current != output_index) continue;
            std::fprintf(stderr, "Capture output=%d %ls%s adapter=%ls vendor=%04x rect=(%ld,%ld)-(%ld,%ld)\n",
                current, desc.DeviceName, primary ? " primary" : "", ad.Description, ad.VendorId,
                desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
                desc.DesktopCoordinates.right, desc.DesktopCoordinates.bottom);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                std::fprintf(stderr, "Selected output must use a hardware graphics adapter\n");
                return false;
            }
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            if (!hr_ok(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                    levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &context_),
                    "D3D11CreateDevice(capture adapter)") ||
                !hr_ok(output.As(&output_), "IDXGIOutput5")) return false;
            return reopen();
        }
    }
    std::fprintf(stderr, "No attached %s display matches --display %d\n",
                 output_index < 0 ? "primary" : "indexed", output_index);
    return false;
}

bool DdaCapture::reopen(bool log)
{
    release();
    duplication_.Reset();
    if (!device_ || !output_) { pause_reason_ = "display or graphics device unavailable"; return false; }
    auto checked = [this, log](HRESULT hr, const char *operation) {
        if (FAILED(hr)) pause_reason_ = duplication_error(hr);
        return log ? hr_ok(hr, operation) : SUCCEEDED(hr);
    };
    if (!checked(device_->GetDeviceRemovedReason(), "Capture device status")) return false;
    ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 output_desc{};
    if (!checked(output_.As(&output6), "IDXGIOutput6 (Windows 10 1803+)") ||
        !checked(output6->GetDesc1(&output_desc), "GetOutputDesc1")) return false;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (log && EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
        std::fprintf(stderr, "Desktop mode=%lux%lu %lu Hz %lu bpp; output bits_per_color=%u colorspace=%d D3D_level=0x%x\n",
            mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency, mode.dmBitsPerPel,
            output_desc.BitsPerColor, int(output_desc.ColorSpace), unsigned(device_->GetFeatureLevel()));
    if (!output_desc.AttachedToDesktop || output_desc.Rotation != DXGI_MODE_ROTATION_IDENTITY ||
        output_desc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        pause_reason_ = !output_desc.AttachedToDesktop ? "display disconnected" :
            output_desc.Rotation != DXGI_MODE_ROTATION_IDENTITY ? "rotated display unsupported" : "HDR enabled; SDR required";
        if (log) std::fprintf(stderr, "Capture requires an attached, unrotated SDR output: attached=%d rotation=%d colorspace=%d\n",
                     output_desc.AttachedToDesktop, int(output_desc.Rotation), int(output_desc.ColorSpace));
        return false;
    }
    // Advanced Color can use linear FP16 composition on an SDR monitor too.
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
    HRESULT hr = output_->DuplicateOutput1(device_.Get(), 0, 2, formats, &duplication_);
    const char *api = "DuplicateOutput1";
    if (hr == DXGI_ERROR_UNSUPPORTED || hr == E_NOTIMPL) {
        if (log) std::fprintf(stderr, "DuplicateOutput1 returned 0x%08lx; trying standard DuplicateOutput\n",
                     static_cast<unsigned long>(hr));
        duplication_.Reset();
        api = "DuplicateOutput";
        hr = output_->DuplicateOutput(device_.Get(), &duplication_);
    }
    if (FAILED(hr)) {
        pause_reason_ = duplication_error(hr);
        if (log) std::fprintf(stderr, "%s failed: 0x%08lx: %s\n",
                     api, static_cast<unsigned long>(hr), duplication_error(hr));
        return false;
    }
    DXGI_OUTDUPL_DESC desc{};
    duplication_->GetDesc(&desc);
    if ((desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         desc.ModeDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
        pause_reason_ = "unsupported desktop format or rotation";
        if (log) std::fprintf(stderr, "Unexpected DDA format/rotation: %d/%d\n", int(desc.ModeDesc.Format), int(desc.Rotation));
        duplication_.Reset();
        return false;
    }
    if (log) std::fprintf(stderr, "DDA ready via %s: %ux%u refresh=%u/%u Hz format=%d\n", api,
        desc.ModeDesc.Width, desc.ModeDesc.Height,
        desc.ModeDesc.RefreshRate.Numerator, desc.ModeDesc.RefreshRate.Denominator,
        int(desc.ModeDesc.Format));
    pause_reason_ = nullptr;
    return true;
}

DdaCapture::Result DdaCapture::acquire(unsigned timeout_ms, CapturedFrame &frame)
{
    frame = {};
    if (holding_) {
        std::fprintf(stderr, "DDA acquire called before releasing the previous image\n");
        return Result::Error;
    }
    if (!duplication_) return Result::Lost;
    const ULONGLONG start = GetTickCount64();
    unsigned wait = timeout_ms;
    for (;;) {
        ComPtr<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO info{};
        const HRESULT hr = duplication_->AcquireNextFrame(wait, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::Timeout;
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            duplication_.Reset();
            return Result::Lost;
        }
        if (!hr_ok(hr, "AcquireNextFrame")) return Result::Error;
        holding_ = true;
        if (info.LastPresentTime.QuadPart > 0) {
            if (!resource || !hr_ok(resource.As(&frame_), "DDA texture interface")) {
                release();
                return Result::Error;
            }
            D3D11_TEXTURE2D_DESC desc{};
            frame_->GetDesc(&desc);
            if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                std::fprintf(stderr, "Unsupported DDA texture format=%d\n", int(desc.Format));
                release();
                return Result::Error;
            }
            frame.texture = frame_.Get();
            frame.width = desc.Width; frame.height = desc.Height;
            frame.present_qpc = info.LastPresentTime.QuadPart;
            frame.accumulated = info.AccumulatedFrames;
            return Result::Frame;
        }
        // Pointer-only update: do not assign a new content timestamp to old pixels.
        release();
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= timeout_ms) return Result::Timeout;
        wait = timeout_ms - unsigned(elapsed);
    }
}

void DdaCapture::release()
{
    if (holding_ && duplication_) {
        const HRESULT hr = duplication_->ReleaseFrame();
        if (FAILED(hr) && hr != DXGI_ERROR_ACCESS_LOST) hr_ok(hr, "ReleaseFrame");
    }
    holding_ = false;
    frame_.Reset();
}
} // namespace lls
