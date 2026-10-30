#include "precheck.h"
extern "C" {
#include "lls_probe.h"
}
#include "win_util.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cwchar>
#include <cwctype>

using Microsoft::WRL::ComPtr;
namespace lls {
namespace {
Problem problem_of(const wchar_t *headline, const wchar_t *advice, std::wstring detail = {})
{
    return {headline, advice, std::move(detail)};
}

std::wstring widen(const char *text)
{
    std::wstring out;
    for (; *text; ++text) out += wchar_t(static_cast<unsigned char>(*text));
    return out;
}

std::wstring codec_list(uint32_t codecs)
{
    std::wstring names;
    const uint8_t order[] = {LLS_CODEC_HEVC, LLS_CODEC_AV1, LLS_CODEC_H264};
    for (size_t i = 0; i < sizeof(order); ++i) {
        if (!(codecs & LLS_CODEC_BIT(order[i]))) continue;
        bool more = false;
        for (size_t j = i + 1; j < sizeof(order); ++j) more |= (codecs & LLS_CODEC_BIT(order[j])) != 0;
        if (!names.empty()) names += more ? L", " : L" and ";
        names += widen(lls_codec_name(order[i]));
    }
    return names.empty() ? L"no supported video codec" : names;
}

std::wstring hresult(const wchar_t *operation, HRESULT hr)
{
    wchar_t text[96];
    std::swprintf(text, 96, L"%ls failed: HRESULT 0x%08lx", operation, static_cast<unsigned long>(hr));
    return text;
}

std::wstring trim(const std::wstring &text)
{
    size_t start = 0, end = text.size();
    while (start < end && std::iswspace(text[start])) ++start;
    while (end > start && std::iswspace(text[end - 1])) --end;
    return text.substr(start, end - start);
}

struct Net {
    bool ok = lls_net_init();
    ~Net() { if (ok) lls_net_cleanup(); }
};
} // namespace

bool check_address(const std::wstring &host_text, const std::wstring &port_text,
                   std::string &host_out, uint16_t &port_out, Problem &problem)
{
    const std::wstring host = trim(host_text), port = trim(port_text);
    if (host.empty()) {
        problem = problem_of(L"Enter the receiver's address",
            L"Type the address the receiver shows while it waits, for example 192.168.1.50.");
        return false;
    }
    for (wchar_t c : host)
        if (c > 0x7e || c <= 0x20) {
            problem = problem_of(L"The address has characters that can't be in an address",
                L"Type the address the receiver shows while it waits, for example 192.168.1.50.");
            return false;
        }
    unsigned long value = 0;
    bool digits = !port.empty() && port.size() <= 5;
    for (wchar_t c : port) digits = digits && c >= L'0' && c <= L'9';
    if (digits) value = std::wcstoul(port.c_str(), nullptr, 10);
    if (!digits || value < 1 || value > 65535) {
        problem = problem_of(L"The port must be a number from 1 to 65535",
            L"Use 47800 unless the receiver shows a different port.");
        return false;
    }
    host_out.assign(host.begin(), host.end());
    port_out = uint16_t(value);
    Net net;
    LlsAddress address;
    if (!net.ok || !lls_net_address(host_out.c_str(), port_out, AF_UNSPEC, &address)) {
        problem = problem_of(L"LLCast can't find the receiver",
            (L"No device called \"" + host + L"\" was found on the network. "
             L"Type the numeric address the receiver shows, for example 192.168.1.50.").c_str());
        return false;
    }
    return true;
}

bool check_desktop(Problem &problem)
{
    DWORD session = 0;
    if (GetSystemMetrics(SM_REMOTESESSION) ||
        (ProcessIdToSessionId(GetCurrentProcessId(), &session) &&
         session != WTSGetActiveConsoleSessionId())) {
        problem = problem_of(L"LLCast is running in a Remote Desktop session",
            L"Windows only lets apps capture the screen of the PC's own desktop. "
            L"Start LLCast on the PC itself, signed in at its monitor.");
        return false;
    }
    wchar_t name[64] = L"";
    DWORD size = 0;
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    const bool named = input && GetUserObjectInformationW(input, UOI_NAME, name, sizeof(name), &size);
    if (input) CloseDesktop(input);
    if (!named || _wcsicmp(name, L"Default") != 0) {
        problem = problem_of(L"The PC is locked or showing a security prompt",
            L"Unlock the PC or answer the prompt, then press Start again.",
            named ? L"Input desktop: " + std::wstring(name) : L"OpenInputDesktop failed");
        return false;
    }
    return true;
}

bool check_display(Problem &problem)
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        problem = problem_of(L"Windows graphics aren't available",
            L"Update the graphics driver and restart the PC.", hresult(L"CreateDXGIFactory1", hr));
        return false;
    }
    bool hardware = false;
    for (UINT a = 0; ; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{};
        if (!adapter || FAILED(adapter->GetDesc1(&ad))) continue;
        hardware = hardware || !(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (!output || FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop ||
                desc.DesktopCoordinates.left != 0 || desc.DesktopCoordinates.top != 0) continue;
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                problem = problem_of(L"The main display needs a hardware graphics driver",
                    L"Install the graphics driver from your PC or graphics-card manufacturer, then restart the PC.");
                return false;
            }
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc1{};
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc1))) {
                problem = problem_of(L"This version of Windows is too old",
                    L"LLCast needs Windows 10 version 1803 or later.", L"IDXGIOutput6 unavailable");
                return false;
            }
            if (desc1.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc1.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                problem = problem_of(L"The main display is rotated",
                    L"Set its orientation to Landscape in Settings > System > Display, then press Start again.");
                return false;
            }
            if (desc1.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
                problem = problem_of(L"HDR is on for the main display",
                    L"LLCast streams SDR only. Turn off \"Use HDR\" in Settings > System > Display, "
                    L"then press Start again.");
                return false;
            }
            return true;
        }
    }
    problem = hardware
        ? problem_of(L"No main display was found", L"Connect a monitor and make it the main display in Windows Settings.")
        : problem_of(L"No hardware graphics card was found", L"Install the graphics driver from your PC or graphics-card manufacturer.");
    return false;
}

bool check_encoder(uint32_t codecs, const EncoderConfig &config, Problem &problem, EncoderBackend backend)
{
    if (config.codec > LLS_CODEC_AV1) {
        problem = problem_of(L"Unknown video codec", L"Choose Auto, AV1, HEVC or H.264 under Codec.");
        return false;
    }
    const std::wstring receiver = L"The receiver decodes " + codec_list(codecs) + L".";
    if (!config.codec && !lls_codec_choose(codecs, codecs)) {
        problem = problem_of(L"Auto has no codec in common with the receiver",
            (receiver + L" Auto uses HEVC or H.264. Choose AV1 explicitly if this PC can encode it, "
             L"or enable HEVC or H.264 on the receiver.").c_str());
        return false;
    }
    if (config.codec && !(codecs & LLS_CODEC_BIT(config.codec))) {
        problem = {L"The receiver can't decode " + widen(lls_codec_name(config.codec)),
            receiver + L" Choose Auto or one of those codecs.", {}};
        return false;
    }
    if (config.codec) {
        const std::wstring codec = widen(lls_codec_name(config.codec));
        problem = {L"This display's hardware encoder can't encode " + codec,
            L"Choose Auto or another codec, or update the graphics driver. " + receiver, {}};
        if (backend == EncoderBackend::Nvenc) {
            problem.headline = L"NVENC can't encode " + codec + L" on this display";
            problem.advice = L"Choose Media Foundation under Encoder, or choose Auto or another codec. " + receiver +
                L" Update the NVIDIA driver if this GPU supports " + codec + L" encoding.";
        } else if (backend == EncoderBackend::MediaFoundation) {
            problem.headline = L"Media Foundation couldn't start " + codec + L" hardware encoding for this display";
            problem.advice += L" On an NVIDIA GPU, choose NVENC under Encoder. On Windows N, install the Media Feature Pack.";
        }
    } else if (backend == EncoderBackend::Nvenc) {
        problem = problem_of(L"NVENC needs an NVIDIA graphics card",
            L"Choose Media Foundation under Encoder, or update the NVIDIA driver.");
    } else if (backend == EncoderBackend::MediaFoundation) {
        problem = problem_of(L"Media Foundation has no hardware encoder for this display",
            L"Update the graphics driver. On Windows N, install the Media Feature Pack. On an NVIDIA GPU, choose NVENC.");
    } else {
        problem = problem_of(L"No compatible hardware video encoder is available",
            L"Install the latest graphics driver. LLCast needs NVENC or a Media Foundation hardware encoder "
            L"on the main display's GPU, and a codec the receiver can decode. On Windows N, install the Media Feature Pack "
            L"to enable Media Foundation. Close game recorders and other streams, then try again.");
    }
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    for (UINT a = 0; ; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC1 ad{};
        if (FAILED(adapter->GetDesc1(&ad)) || (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        bool primary = false;
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (!output) break;
            DXGI_OUTPUT_DESC desc{};
            primary |= SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop &&
                !desc.DesktopCoordinates.left && !desc.DesktopCoordinates.top;
        }
        if (!primary) continue;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context))) break;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = config.width; desc.Height = config.height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_NV12; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) break;
        auto encoder = open_encoder(device.Get(), texture.Get(), config, codecs, backend);
        if (encoder) {
            std::fprintf(stderr, "Encoder check: %s (%s)\n", encoder->name(), lls_codec_name(encoder->codec()));
            return true;
        }
        break;
    }
    return false;
}

bool check_receiver(const std::string &host, uint16_t port, uint32_t &rtt_us, uint32_t &codecs, Problem &problem)
{
    const std::wstring where = widen(host.c_str()) + L", port " + std::to_wstring(port);
    int error = 0;
    switch (lls_probe_receiver(host.c_str(), port, &rtt_us, &codecs, &error)) {
    case LLS_PROBE_ANSWERED:
        if (!(codecs & (LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264) | LLS_CODEC_BIT(LLS_CODEC_AV1)))) {
            problem = problem_of(L"No shared video codec", L"This receiver needs HEVC, AV1 or H.264 hardware decoding.");
            return false;
        }
        return true;
    case LLS_PROBE_UNREACHABLE:
        problem = problem_of(L"This PC can't reach the receiver",
            (L"Nothing on this PC's network answers at " + where + L". Check the address the receiver shows, "
             L"and that the PC and the receiver are on the same network.").c_str(),
            L"socket error " + std::to_wstring(error));
        return false;
    case LLS_PROBE_REFUSED:
        problem = problem_of(L"The device is on, but LLCast isn't receiving",
            (L"The device at " + where + L" answered, but LLCast isn't running on it. Open LLCast "
             L"there and press Start; it then shows its address.").c_str());
        return false;
    case LLS_PROBE_FAILED:
        problem = problem_of(L"LLCast can't open a network connection",
            L"Check that this PC is connected to the network.", L"socket error " + std::to_wstring(error));
        return false;
    case LLS_PROBE_SILENT:
        break;
    }
    problem = problem_of(L"The receiver doesn't answer",
        (L"Nothing answered at " + where + L". Check that the receiver shows the LLCast waiting screen, "
         L"that the address and port match, and that the PC and the receiver are on the same "
         L"network.").c_str());
    return false;
}

Problem explain(PipelineStep step)
{
    switch (step) {
    case PipelineStep::Timing:
        return problem_of(L"Windows can't provide the precise timing LLCast needs",
            L"LLCast needs Windows 10 version 1803 or later.");
    case PipelineStep::Capture:
        return problem_of(L"LLCast can't capture the screen",
            L"Another app may be capturing the screen or showing protected video, or the display "
            L"changed while starting. Close screen recorders and press Start again.");
    case PipelineStep::Convert:
        return problem_of(L"The graphics card can't prepare the picture",
            L"Install the latest graphics driver, then press Start again.");
    case PipelineStep::Encode:
        return problem_of(L"The hardware video encoder won't start",
            L"Other apps may be using it, for example game recording or another stream. Close them, "
            L"or install the latest graphics driver, then press Start again.");
    case PipelineStep::Network:
        return problem_of(L"LLCast can't open a network connection to the receiver",
            L"Check the receiver's address and this PC's network connection.");
    case PipelineStep::Codec:
        return problem_of(L"The receiver no longer supports this stream's codec",
            L"Start a new stream so LLCast can choose a codec both devices support.");
    case PipelineStep::Files:
        return problem_of(L"LLCast can't write its log",
            L"Check that the disk isn't full.");
    case PipelineStep::Running:
    case PipelineStep::None:
        break;
    }
    return problem_of(L"Streaming stopped unexpectedly", L"Press Start to try again.");
}

} // namespace lls
