#include "win_probe.h"
#include "dda_capture.h"
#include "nv12_converter.h"
#include "nvenc_encoder.h"
#include "win_util.h"
#include <dxgi1_6.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace lls {
class ApiProbe {
public:
    ~ApiProbe() { if (library_) FreeLibrary(library_); }
    bool open() {
        library_ = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) {
            std::printf("NVENC: nvEncodeAPI64.dll unavailable, error=%lu\n", GetLastError());
            return false;
        }
        using GetVersion = NVENCSTATUS (NVENCAPI *)(uint32_t *);
        using Create = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
        const auto version = dll_function<GetVersion>(library_, "NvEncodeAPIGetMaxSupportedVersion");
        const auto create = dll_function<Create>(library_, "NvEncodeAPICreateInstance");
        uint32_t supported = 0;
        if (!version || !create || version(&supported) != NV_ENC_SUCCESS) return false;
        std::printf("NVENC max API=%u.%u required=%u.%u driver-baseline=570.0\n",
                    supported >> 4, supported & 15, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        if (supported < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) return false;
        api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        return create(&api_) == NV_ENC_SUCCESS;
    }
    bool caps(ID3D11Device *device) {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
        params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        params.apiVersion = NVENCAPI_VERSION;
        params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        params.device = device;
        void *session = nullptr;
        const NVENCSTATUS status = api_.nvEncOpenEncodeSessionEx(&params, &session);
        std::printf("  NVENC session status=%d\n", int(status));
        if (status != NV_ENC_SUCCESS) return false;
        // A session guard also handles allocation failures in the probe vectors.
        struct SessionGuard {
            NV_ENCODE_API_FUNCTION_LIST &api; void *session;
            ~SessionGuard() { api.nvEncDestroyEncoder(session); }
        } guard{api_, session};
        uint32_t count = 0;
        if (api_.nvEncGetEncodeGUIDCount(session, &count) != NV_ENC_SUCCESS || !count) return false;
        std::vector<GUID> guids(count);
        if (api_.nvEncGetEncodeGUIDs(session, guids.data(), count, &count) != NV_ENC_SUCCESS) return false;
        const bool hevc = std::any_of(guids.begin(), guids.end(), [](const GUID &id) {
            return IsEqualGUID(id, NV_ENC_CODEC_HEVC_GUID);
        });
        std::printf("  HEVC supported=%s\n", hevc ? "yes" : "no");
        if (!hevc) return false;
        const NV_ENC_CAPS ids[] = {NV_ENC_CAPS_WIDTH_MAX, NV_ENC_CAPS_HEIGHT_MAX,
            NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES, NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE,
            NV_ENC_CAPS_NUM_MAX_BFRAMES};
        const char *names[] = {"max_width", "max_height", "ratecontrol_mask", "custom_vbv", "max_bframes"};
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
            NV_ENC_CAPS_PARAM caps{};
            caps.version = NV_ENC_CAPS_PARAM_VER;
            caps.capsToQuery = ids[i];
            int value = 0;
            const NVENCSTATUS result = api_.nvEncGetEncodeCaps(session, NV_ENC_CODEC_HEVC_GUID, &caps, &value);
            std::printf("  HEVC %s=%d status=%d\n", names[i], value, int(result));
        }
        if (api_.nvEncGetInputFormatCount(session, NV_ENC_CODEC_HEVC_GUID, &count) != NV_ENC_SUCCESS || !count)
            return false;
        std::vector<NV_ENC_BUFFER_FORMAT> formats(count);
        if (api_.nvEncGetInputFormats(session, NV_ENC_CODEC_HEVC_GUID, formats.data(), count, &count) != NV_ENC_SUCCESS)
            return false;
        const bool nv12 = std::find(formats.begin(), formats.end(), NV_ENC_BUFFER_FORMAT_NV12) != formats.end();
        std::printf("  HEVC NV12 input=%s\n", nv12 ? "yes" : "no");
        return nv12;
    }
private:
    HMODULE library_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
};

bool print_windows_info(int display_index)
{
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    std::printf("LLStream diagnostics: process_session=%lu console_session=%lu remote=%d\n",
                session, WTSGetActiveConsoleSessionId(), GetSystemMetrics(SM_REMOTESESSION));
    ApiProbe api;
    const bool api_ok = api.open();
    ComPtr<IDXGIFactory1> factory;
    if (!hr_ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1(info)")) return false;
    int ordinal = 0;
    for (UINT a = 0; ; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT next = factory->EnumAdapters1(a, &adapter);
        if (next == DXGI_ERROR_NOT_FOUND) break;
        if (!hr_ok(next, "EnumAdapters1(info)")) return false;
        DXGI_ADAPTER_DESC1 ad{};
        if (!hr_ok(adapter->GetDesc1(&ad), "GetAdapterDesc1(info)")) return false;
        std::printf("Adapter %u: %ls vendor=%04x device=%04x VRAM_MiB=%llu software=%d\n", a,
            ad.Description, ad.VendorId, ad.DeviceId,
            static_cast<unsigned long long>(ad.DedicatedVideoMemory / (1024 * 1024)),
            !!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const HRESULT created = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context);
        std::printf("  D3D11 device status=0x%08lx\n", static_cast<unsigned long>(created));
        if (device && ad.VendorId == 0x10de) {
            if (api_ok) api.caps(device.Get());
            else std::printf("  HEVC caps unavailable: required NVENC API not available\n");
            Nv12Converter converter;
            const bool rtv = converter.open(device.Get(), 1920, 1080);
            std::printf("  NV12 texture + R8/R8G8 RTV + shaders=%s\n", rtv ? "supported" : "unavailable");
        }
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIOutput> output;
            const HRESULT hr = adapter->EnumOutputs(i, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (!hr_ok(hr, "EnumOutputs(info)")) return false;
            DXGI_OUTPUT_DESC desc{};
            if (!hr_ok(output->GetDesc(&desc), "GetOutputDesc(info)")) return false;
            const int index = desc.AttachedToDesktop ? ordinal++ : -1;
            const bool primary = desc.AttachedToDesktop &&
                desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            const bool have_mode = EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
            std::printf("  Output %d: %ls attached=%d primary=%d rect=(%ld,%ld)-(%ld,%ld) rotation=%d desktop_Hz=%lu\n",
                index, desc.DeviceName, desc.AttachedToDesktop, primary,
                desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
                desc.DesktopCoordinates.right, desc.DesktopCoordinates.bottom,
                int(desc.Rotation), have_mode ? mode.dmDisplayFrequency : 0ul);
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 extra{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&extra)))
                std::printf("    colorspace=%d bits_per_color=%u (SDR requires colorspace=%d)\n",
                    int(extra.ColorSpace), extra.BitsPerColor, int(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
            if (!device || !desc.AttachedToDesktop) continue;
            ComPtr<IDXGIOutput5> output5;
            if (FAILED(output.As(&output5))) continue;
            ComPtr<IDXGIOutputDuplication> duplicate;
            const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
            const HRESULT available = output5->DuplicateOutput1(device.Get(), 0, 1, &format, &duplicate);
            std::printf("    DDA BGRA8 availability=0x%08lx\n", static_cast<unsigned long>(available));
            if (duplicate) {
                DXGI_OUTDUPL_DESC d{};
                duplicate->GetDesc(&d);
                std::printf("    DDA mode=%ux%u refresh=%u/%u Hz\n", d.ModeDesc.Width,
                    d.ModeDesc.Height, d.ModeDesc.RefreshRate.Numerator, d.ModeDesc.RefreshRate.Denominator);
            }
        }
    }
    std::puts("Selected display: checking console access, SDR DDA, NV12 and encoder initialization");
    DdaCapture capture;
    Nv12Converter converter;
    std::unique_ptr<VideoEncoder> encoder;
    const bool ready = capture.open(display_index) && converter.open(capture.device(), 1920, 1080) &&
        bool(encoder = open_encoder(capture.device(), converter.output(), EncoderConfig{},
            LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264) | LLS_CODEC_BIT(LLS_CODEC_AV1)));
    std::printf("Selected pipeline initialization=%s (no frames captured or encoded)\n", ready ? "ready" : "FAILED");
    return ready;
}
} // namespace lls
