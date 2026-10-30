#include "windows_receiver.h"
#include "mf_decoder.h"
#include "mf_api.h"
#include <d3d10.h>
#include <dxgi1_6.h>
#include <mferror.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
extern "C" {
#include "lls_geometry.h"
#include "lls_net.h"
}

using Microsoft::WRL::ComPtr;
namespace lls {
namespace {
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct MfScope {
    bool started = mf_api().available && SUCCEEDED(mf_api().MFStartup(MF_VERSION, MFSTARTUP_FULL));
    ~MfScope() { if (started) mf_api().MFShutdown(); }
};
template<class F> struct OnExit {
    F action;
    ~OnExit() { action(); }
};
template<class F> OnExit(F) -> OnExit<F>;
void finish(LlsRxSession *rx, LlsRxFrame *frame, int fate) {
    lls_rx_finish(rx, frame, static_cast<LlsFrameFate>(fate), frame->decoded_ns, 0, 0);
}

// Only this worker calls the swap chain. Vsync never blocks the decode worker.
class Presenter {
    struct Pending { DecodedPicture picture; LlsRxFrame *frame = nullptr; } pending_;
    LlsRxSession *rx_ = nullptr;
    HWND window_ = nullptr;
    bool tearing_ = false, closing_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    UINT width_ = 0, height_ = 0, source_width_ = 0, source_height_ = 0;
    UINT swap_flags_ = 0;

    HRESULT create(HWND window, ID3D11Device *device, bool tearing) {
        window_ = window; device_ = device;
        device->GetImmediateContext(&context_);
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory2> factory;
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&video_));
        if (FAILED(hr) || FAILED(hr = context_.As(&video_context_)) || FAILED(hr = device_.As(&dxgi)) ||
            FAILED(hr = dxgi->GetAdapter(&adapter)) || FAILED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) return hr;
        ComPtr<IDXGIFactory5> newer; BOOL allow = FALSE;
        if (tearing && SUCCEEDED(factory.As(&newer)))
            newer->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow));
        tearing_ = tearing;
        swap_flags_ = allow ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        RECT bounds{}; GetClientRect(window, &bounds);
        width_ = UINT(std::max<LONG>(1, bounds.right)); height_ = UINT(std::max<LONG>(1, bounds.bottom));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width_; desc.Height = height_; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE; desc.Flags = swap_flags_;
        if (FAILED(hr = factory->CreateSwapChainForHwnd(device, window, &desc, nullptr, nullptr, &swap_))) return hr;
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGIDevice1> dxgi1;
        if (SUCCEEDED(dxgi.As(&dxgi1))) dxgi1->SetMaximumFrameLatency(1);
        std::fprintf(stderr, "Receiver display: flip discard, max latency 1, %s%s\n",
            tearing ? "immediate presentation" : "vsync", allow ? ", tearing supported" : "");
        return S_OK;
    }
    HRESULT draw(const DecodedPicture &picture) {
        RECT bounds{}; GetClientRect(window_, &bounds);
        if (!bounds.right || !bounds.bottom || IsIconic(GetAncestor(window_, GA_ROOT))) return S_FALSE;
        const auto width = UINT(bounds.right), height = UINT(bounds.bottom);
        D3D11_TEXTURE2D_DESC texture{}; picture.texture->GetDesc(&texture);
        if (width != width_ || height != height_) {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            HRESULT hr = swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swap_flags_);
            if (FAILED(hr)) return hr;
            width_ = width; height_ = height;
            processor_.Reset(); enumerator_.Reset();
        }
        if (!processor_ || source_width_ != texture.Width || source_height_ != texture.Height) {
            processor_.Reset(); enumerator_.Reset();
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputFrameRate = {60, 1}; content.OutputFrameRate = {60, 1};
            content.InputWidth = texture.Width; content.InputHeight = texture.Height;
            content.OutputWidth = width; content.OutputHeight = height;
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            HRESULT hr = video_->CreateVideoProcessorEnumerator(&content, &enumerator_);
            if (FAILED(hr) || FAILED(hr = video_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) return hr;
            source_width_ = texture.Width; source_height_ = texture.Height;
            video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{}, output{};
            input.YCbCr_Matrix = 1; // BT.709, limited -> RGB full
            input.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            output.RGB_Range = 0; output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input);
            video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output);
        }
        ComPtr<ID3D11Texture2D> back;
        ComPtr<ID3D11VideoProcessorOutputView> output;
        ComPtr<ID3D11VideoProcessorInputView> input;
        HRESULT hr = swap_->GetBuffer(0, IID_PPV_ARGS(&back));
        if (FAILED(hr)) return hr;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
        out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
        in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        in_desc.Texture2D.ArraySlice = picture.subresource / texture.MipLevels;
        in_desc.Texture2D.MipSlice = picture.subresource % texture.MipLevels;
        if (FAILED(hr = video_->CreateVideoProcessorOutputView(back.Get(), enumerator_.Get(), &out_desc, &output)) ||
            FAILED(hr = video_->CreateVideoProcessorInputView(picture.texture.Get(), enumerator_.Get(), &in_desc, &input))) return hr;
        const LlsRect fitted = lls_fit_rect(UINT(picture.source.right - picture.source.left),
            UINT(picture.source.bottom - picture.source.top), width, height, true, 1);
        const RECT target{LONG(fitted.x), LONG(fitted.y), LONG(fitted.x + fitted.width), LONG(fitted.y + fitted.height)};
        const RECT full{0, 0, LONG(width), LONG(height)};
        D3D11_VIDEO_COLOR black{}; black.RGBA.A = 1;
        video_context_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &black);
        video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &full);
        video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &picture.source);
        video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &target);
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = input.Get();
        if (FAILED(hr = video_context_->VideoProcessorBlt(processor_.Get(), output.Get(), 0, 1, &stream))) return hr;
        return swap_->Present(tearing_ ? 0 : 1, tearing_ && swap_flags_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }
    void run() {
        ComScope com;
        // Keeping the previous sample through its replacement also keeps the
        // decoder pool from recycling its surface while presentation uses it.
        DecodedPicture current;
        for (;;) {
            Pending next;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return closing_ || pending_.frame; });
                if (closing_) break;
                next = std::move(pending_); pending_ = {};
            }
            if (next.frame->epoch != lls_rx_epoch(rx_)) {
                finish(rx_, next.frame, LLS_RX_DISCARDED); continue;
            }
            next.frame->ready_ns = int64_t(lls_net_now_ns());
            const HRESULT hr = draw(next.picture);
            if (hr == S_OK) {
                current = std::move(next.picture);
                ++presented; last_present_ns = lls_net_now_ns();
                // Present returning is submission, not a measured LCD flip.
                // Report successful delivery with decoded_us and NO flip time.
                finish(rx_, next.frame, LLS_FATE_DELIVERED);
            } else {
                finish(rx_, next.frame, hr == S_FALSE || hr == DXGI_STATUS_OCCLUDED ? int(LLS_FATE_REPLACED) : int(LLS_RX_DISCARDED));
                if (FAILED(hr)) { error = hr; lls_rx_stop(rx_); break; }
            }
        }
    }
public:
    std::atomic<uint64_t> presented{0}, last_present_ns{0};
    std::atomic<HRESULT> error{S_OK};
    HRESULT start(HWND window, ID3D11Device *device, LlsRxSession *rx, bool tearing) {
        rx_ = rx;
        const HRESULT hr = create(window, device, tearing);
        if (SUCCEEDED(hr)) thread_ = std::thread(&Presenter::run, this);
        return hr;
    }
    void submit(DecodedPicture picture, LlsRxFrame *frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.frame) finish(rx_, pending_.frame, LLS_FATE_REPLACED);
        pending_ = {std::move(picture), frame};
        wake_.notify_one();
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex_); closing_ = true; wake_.notify_one(); }
        if (thread_.joinable()) thread_.join();
        if (pending_.frame) { finish(rx_, pending_.frame, LLS_RX_DISCARDED); pending_ = {}; }
    }
    ~Presenter() { stop(); }
};
}

struct WindowsReceiver::State {
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<bool> finished{false};
    ReceiveStatus status;
    LlsRxSession *rx = nullptr;
    Presenter *presenter = nullptr; // protected by mutex, owned on worker
    std::string profile_listen, profile_connect;
    uint32_t profile_capacity = 0;

    void fail(const wchar_t *headline, const wchar_t *advice, HRESULT hr) {
        wchar_t code[48]; std::swprintf(code, 48, L"HRESULT 0x%08lx", static_cast<unsigned long>(hr));
        std::fprintf(stderr, "Receiver: %ls; %ls\n", headline, code);
        std::lock_guard<std::mutex> lock(mutex);
        status.failed = true; status.problem = {headline, advice, code};
    }
    void run(HWND window, bool tearing) {
        ComScope com; MfScope mf;
        if (FAILED(com.result) || !mf.started) {
            fail(L"Windows media support is unavailable", L"On Windows N, install the Media Feature Pack in Optional features, then restart Windows.", E_NOINTERFACE); return;
        }
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr)) { fail(L"This PC cannot show hardware-decoded video", L"Update the graphics driver and run LLCast on the local desktop.", hr); return; }
        ComPtr<ID3D10Multithread> protection;
        if (FAILED(hr = context.As(&protection))) { fail(L"The graphics driver cannot share video surfaces", L"Update the graphics driver.", hr); return; }
        protection->SetMultithreadProtected(TRUE);
        UINT reset_token = 0; ComPtr<IMFDXGIDeviceManager> manager;
        if (FAILED(hr = mf_api().MFCreateDXGIDeviceManager(&reset_token, &manager)) ||
            FAILED(hr = manager->ResetDevice(device.Get(), reset_token))) {
            fail(L"Windows cannot start hardware video decoding", L"Update the graphics driver.", hr); return;
        }
        uint32_t codecs = 0;
        const bool hevc_installed = MfDecoder::installed(LLS_CODEC_HEVC);
        const bool hevc_gpu = MfDecoder::gpu_supports(device.Get(), LLS_CODEC_HEVC);
        HRESULT hevc_error = S_OK;
        for (uint8_t codec : {uint8_t(LLS_CODEC_H264), uint8_t(LLS_CODEC_HEVC)}) {
            MfDecoder probe;
            const bool gpu = codec == LLS_CODEC_HEVC ? hevc_gpu : MfDecoder::gpu_supports(device.Get(), codec);
            const HRESULT result = gpu ? probe.open(manager.Get(), codec) : MF_E_UNSUPPORTED_D3D_TYPE;
            if (codec == LLS_CODEC_HEVC) hevc_error = result;
            if (SUCCEEDED(result)) codecs |= LLS_CODEC_BIT(codec);
            else std::fprintf(stderr, "Receiver %s probe: GPU support=%u HRESULT 0x%08lx\n",
                lls_codec_name(codec), unsigned(gpu), (unsigned long)result);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            status.codecs_checked = true; status.codecs = codecs;
            status.hevc_installed = hevc_installed; status.hevc_gpu = hevc_gpu;
            status.hevc_error = hevc_error;
        }
        if (!codecs) {
            fail(L"No usable hardware video decoder", L"LLCast needs hardware H.264 or HEVC decoding. Update the graphics driver. For HEVC, install HEVC Video Extensions; on Windows N, install the Media Feature Pack.", MF_E_TOPO_CODEC_NOT_FOUND); return;
        }
        const LlsProfileConfig profiling{profile_listen.empty() ? nullptr : profile_listen.c_str(),
            profile_connect.empty() ? nullptr : profile_connect.c_str(), profile_capacity};
        std::unique_ptr<LlsProfile, decltype(&lls_profile_close)> profile_owner(
            lls_rx_profile_open(&profiling, "LLCast_Windows_rx"), lls_profile_close);
        LlsProfile *profile = profile_owner.get();
        if (lls_profile_enabled(&profiling) && !profile) { fail(L"The profiling service could not start", L"Check the profile address and port.", E_FAIL); return; }
        LlsRxConfig config; lls_rx_default_config(&config);
        config.codecs = codecs; config.profile = profile;
        LlsRxSession *session = nullptr; int net_error = 0;
        for (unsigned port = LLS_DEFAULT_PORT; port < LLS_DEFAULT_PORT + 10; ++port) {
            config.port = uint16_t(port); session = lls_rx_open(&config, &net_error);
            if (session || net_error != WSAEADDRINUSE) break;
        }
        if (!session) {
            fail(L"The receiver could not open its network port", L"Close another receiver using this port, then try Receive again.", HRESULT_FROM_WIN32(net_error));
            return;
        }
        std::unique_ptr<LlsRxSession, decltype(&lls_rx_close)> session_owner(session, lls_rx_close);
        MfDecoder decoder;
        Presenter display;
        if (FAILED(hr = display.start(window, device.Get(), session, tearing))) {
            fail(L"This PC cannot display the stream", L"Update the graphics driver and run LLCast on the local desktop.", hr);
            return;
        }
        OnExit cleanup{[&] {
            lls_rx_stop(session); display.stop();
            std::lock_guard<std::mutex> lock(mutex);
            rx = nullptr; presenter = nullptr;
            status.presented = display.presented; status.last_present_ns = display.last_present_ns;
            SetThreadExecutionState(ES_CONTINUOUS);
        }};
        {
            std::lock_guard<std::mutex> lock(mutex);
            rx = session; presenter = &display;
            status.ready = true; status.port = config.port; status.codecs = codecs; status.hevc_installed = hevc_installed;
            if (stopping) lls_rx_stop(rx);
        }
        std::fprintf(stderr, "Receiver ready: UDP %u, hardware codecs 0x%x, HEVC component %s\n", config.port, codecs, hevc_installed ? "available" : "missing");
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
        uint8_t current_codec = 0;
        std::array<LlsRxFrame *, 16> pending{};
        auto retire = [&](int fate) {
            for (auto &frame : pending) if (frame) { finish(session, frame, fate); frame = nullptr; }
        };
        unsigned failures = 0;
        std::vector<DecodedPicture> pictures;
        pictures.reserve(pending.size());
        while (!stopping) {
            bool reset = false;
            LlsRxFrame *frame = lls_rx_next(session, -1, &reset);
            if (!frame) break;
            if (stopping || frame->epoch != lls_rx_epoch(session)) { finish(session, frame, LLS_RX_DISCARDED); continue; }
            if (reset || current_codec != frame->codec) {
                decoder.close(); retire(LLS_RX_DISCARDED);
                current_codec = frame->codec;
                hr = decoder.open(manager.Get(), current_codec);
                if (FAILED(hr)) {
                    finish(session, frame, LLS_FATE_DECODE_ERROR);
                    fail(L"The stream's hardware decoder could not start", L"Choose H.264 on the sender, or update the graphics driver and HEVC Video Extensions.", hr); break;
                }
                std::lock_guard<std::mutex> lock(mutex); status.codec = current_codec;
            }
            const uint32_t session_id = frame->session, number = frame->frame;
            auto slot = std::find(pending.begin(), pending.end(), nullptr);
            if (slot == pending.end()) {
                finish(session, frame, LLS_FATE_DECODE_ERROR); retire(LLS_FATE_DECODE_ERROR);
                decoder.close(); current_codec = 0; lls_rx_resync(session, session_id, number); continue;
            }
            *slot = frame;
            frame->decode_put_ns = int64_t(lls_net_now_ns());
            pictures.clear();
            hr = decoder.push(frame->data, frame->size, frame->key, int64_t(frame->ordinal), pictures);
            lls_rx_release_data(frame);
            for (auto &picture : pictures) {
                auto found = std::find_if(pending.begin(), pending.end(), [&](LlsRxFrame *f) { return f && int64_t(f->ordinal) == picture.token; });
                if (found == pending.end()) { hr = E_UNEXPECTED; break; }
                LlsRxFrame *decoded = *found; *found = nullptr;
                decoded->decoded_ns = int64_t(lls_net_now_ns());
                if (decoded->epoch != lls_rx_epoch(session)) finish(session, decoded, LLS_RX_DISCARDED);
                else display.submit(std::move(picture), decoded);
            }
            pictures.clear(); // release any rejected surfaces before a decoder reset
            if (FAILED(hr)) {
                hr_ok(hr, "Receiver decode");
                retire(LLS_FATE_DECODE_ERROR); decoder.close(); current_codec = 0;
                lls_rx_resync(session, session_id, number);
                if (hr == MF_E_UNSUPPORTED_D3D_TYPE || hr == E_NOINTERFACE || ++failures >= 3) {
                    fail(L"This stream cannot be decoded in hardware", L"Choose Low or H.264 on the sender. If it still fails, update the graphics driver.", hr); break;
                }
            } else failures = 0;
        }
        LlsRxStatus network{}; lls_rx_status(session, &network);
        if (network.failed && !stopping) fail(L"The receiver's network socket stopped", L"Check the network connection, then try Receive again.", E_FAIL);
        lls_rx_stop(session);
        display.stop(); // all display callbacks and decoder surfaces precede teardown
        decoder.close(); retire(LLS_RX_DISCARDED);
        if (FAILED(display.error)) fail(L"The graphics device stopped displaying video", L"Update the graphics driver, then try Receive again.", display.error);
    }
};

WindowsReceiver::WindowsReceiver() : state_(new State) {}
WindowsReceiver::~WindowsReceiver() { stop(); }
void WindowsReceiver::start(HWND video, bool tearing, const LlsProfileConfig &profile)
{
    stop(); state_.reset(new State);
    state_->profile_listen = profile.listen ? profile.listen : "";
    state_->profile_connect = profile.connect ? profile.connect : "";
    state_->profile_capacity = profile.capacity;
    State *state = state_.get();
    state->worker = std::thread([state, video, tearing] {
        // Allocation/driver exceptions must not escape std::thread.
        try { state->run(video, tearing); }
        catch (const std::exception &e) {
            std::fprintf(stderr, "Receiver exception: %s\n", e.what());
            state->fail(L"The receiver could not continue", L"Close and reopen Receive; see the log for details.", E_FAIL);
        }
        state->finished = true;
    });
}
ReceiveStatus WindowsReceiver::status()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    ReceiveStatus result = state_->status;
    result.finished = state_->finished;
    if (state_->rx) lls_rx_status(state_->rx, &result.network);
    if (state_->presenter) { result.presented = state_->presenter->presented; result.last_present_ns = state_->presenter->last_present_ns; }
    return result;
}
void WindowsReceiver::request_stop()
{
    state_->stopping = true;
    { std::lock_guard<std::mutex> lock(state_->mutex); if (state_->rx) lls_rx_stop(state_->rx); }
}
void WindowsReceiver::stop()
{
    request_stop();
    if (state_->worker.joinable()) state_->worker.join();
}
} // namespace lls
