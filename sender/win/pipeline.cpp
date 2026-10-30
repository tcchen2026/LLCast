// Moved from llstream_win.cpp so that LLCast runs the same pipeline.
#include "pipeline.h"
#include "lls_probe.h"
#include "dda_capture.h"
#include "nv12_converter.h"
#include "color_math.h"
#include "frame_policy.h"
#include "capture_wait.h"
#include "pattern_motion.h"
#include "stage_log.h"
#include "win_util.h"
#include <mmsystem.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <algorithm>

using Microsoft::WRL::ComPtr;
namespace lls {
namespace {
class Timing {
public:
    bool open() {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
            uint64_t(frequency.QuadPart) > UINT64_MAX / 1000000) return false;
        frequency_ = uint64_t(frequency.QuadPart);
        std::fprintf(stderr, "QPC frequency=%llu Hz; all stage times use QPC microseconds\n",
                     static_cast<unsigned long long>(frequency_));
        period_ = timeBeginPeriod(1) == TIMERR_NOERROR;
        priority_ = GetThreadPriority(GetCurrentThread());
        if (!period_ || priority_ == THREAD_PRIORITY_ERROR_RETURN ||
            !SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
            std::fprintf(stderr, "Cannot enable 1 ms timer/pipeline thread priority\n");
            return false;
        }
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) {
            std::fprintf(stderr, "High-resolution timer unavailable (error %lu); Windows 10 1803+ needed\n",
                         GetLastError());
            return false;
        }
        recovery_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return recovery_ != nullptr;
    }
    ~Timing() {
        if (timer_) CloseHandle(timer_);
        if (recovery_) CloseHandle(recovery_);
        if (priority_ != THREAD_PRIORITY_ERROR_RETURN)
            SetThreadPriority(GetCurrentThread(), priority_);
        if (period_) timeEndPeriod(1);
    }
    uint64_t from_qpc(int64_t qpc) const { return qpc > 0 ? qpc_us(uint64_t(qpc), frequency_) : 0; }
    uint64_t now() const {
        LARGE_INTEGER qpc{};
        if (!QueryPerformanceCounter(&qpc)) throw std::runtime_error("QPC failed");
        return from_qpc(qpc.QuadPart);
    }
    static void recover(void *context) { SetEvent(static_cast<Timing *>(context)->recovery_); }
    void wait_until(uint64_t deadline, HANDLE capture = nullptr, bool recovery = true) const {
        const uint64_t at = now();
        if (deadline <= at) return;
        // Existing UI/console stop is a flag, so bound its observation latency.
        const uint64_t delay = std::min<uint64_t>(deadline - at, 100000);
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(delay * 10);
        HANDLE events[3] = {timer_};
        DWORD count = 1;
        if (recovery) events[count++] = recovery_;
        if (capture) events[count++] = capture;
        if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0) ||
            WaitForMultipleObjects(count, events, FALSE, INFINITE) == WAIT_FAILED)
            throw std::runtime_error("pipeline waitable timer failed");
    }
private:
    HANDLE timer_ = nullptr;
    HANDLE recovery_ = nullptr;
    uint64_t frequency_ = 0;
    bool period_ = false;
    int priority_ = THREAD_PRIORITY_ERROR_RETURN;
};

struct FileCloser { void operator()(FILE *f) const { if (f) std::fclose(f); } };
struct SessionCloser { void operator()(LlsSender *s) const { if (s) lls_sender_close(s); } };
struct ProfileCloser { void operator()(LlsProfile *p) const { lls_profile_close(p); } };
using File = std::unique_ptr<FILE, FileCloser>;
using Session = std::unique_ptr<LlsSender, SessionCloser>;
using Profile = std::unique_ptr<LlsProfile, ProfileCloser>;

bool pattern_device(ComPtr<ID3D11Device> &device, ComPtr<ID3D11DeviceContext> &context)
{
    ComPtr<IDXGIFactory1> factory;
    if (!hr_ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return false;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (!adapter || FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        return hr_ok(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context), "pattern D3D11 device");
    }
    std::fprintf(stderr, "No hardware graphics adapter for the synthetic-pattern test\n");
    return false;
}

void fail(PipelineStatus *status, PipelineStep step)
{
    if (status) status->failed = step;
}
} // namespace

int run_pipeline(const PipelineOptions &o, const volatile long &stop, PipelineStatus *status)
{
    EncoderConfig encoder_config = o.encoder;
    unsigned fec_percent = o.sender.host ? o.sender.fec_percent : 0;
    encoder_config.bitrate_bps = uint32_t(uint64_t(o.encoder.bitrate_bps) * 100 / (100 + fec_percent));
    std::printf("Video/parity budget_bps=%u encoder_bps=%u fec_percent=%u fec_min=%u; "
                "packet headers and parity rounding add overhead\n",
                o.encoder.bitrate_bps, encoder_config.bitrate_bps, fec_percent, o.sender.fec_min);
    Timing timing;
    if (!timing.open()) { fail(status, PipelineStep::Timing); return 1; }
    uint32_t peer_codecs = o.receiver_codecs;
    if (!o.sender.host) peer_codecs = LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264) | LLS_CODEC_BIT(LLS_CODEC_AV1);
    else if (!peer_codecs) {
        std::fprintf(stderr, "Waiting for the receiver's codec capabilities at %s:%u\n", o.sender.host, o.sender.port);
        while (!stop) {
            uint32_t rtt = 0; int error = 0;
            LlsProbeResult result = lls_probe_receiver(o.sender.host, o.sender.port, &rtt, &peer_codecs, &error);
            if (result == LLS_PROBE_ANSWERED) break;
            if (result == LLS_PROBE_FAILED) { fail(status, PipelineStep::Network); return 1; }
        }
        if (stop) return 0;
    }
    DdaCapture capture;
    struct CaptureRelease {
        DdaCapture &capture;
        ~CaptureRelease() { capture.release(); }
    } capture_release{capture};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> pattern, moving_pattern;
    if (o.pattern) {
        if (!pattern_device(device, context)) { fail(status, PipelineStep::Capture); return 1; }
        const auto pixels = test_pattern(o.pattern_width, o.pattern_height);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = o.pattern_width; desc.Height = o.pattern_height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data = {pixels.data(), o.pattern_width * 4, 0};
        if (!hr_ok(device->CreateTexture2D(&desc, &data, &pattern), "pattern texture")) return 1;
        if (o.pattern_motion) {
            desc.Usage = D3D11_USAGE_DEFAULT;
            if (!hr_ok(device->CreateTexture2D(&desc, nullptr, &moving_pattern), "moving pattern texture")) return 1;
        }
        std::printf("Pattern source=%ux%u motion=%u px/frame right; "
                    "offset=(frame*motion)%%width, frame zero unshifted\n",
                    o.pattern_width, o.pattern_height, o.pattern_motion);
    } else {
        if (!capture.open(o.display)) { fail(status, PipelineStep::Capture); return 1; }
        device = capture.device(); context = capture.context();
    }
    CaptureWait<DdaCapture, CapturedFrame> acquisition(capture);
    if (!o.pattern && !acquisition.open()) { fail(status, PipelineStep::Capture); return 1; }
    Nv12Converter converter;
    if (!converter.open(device.Get(), encoder_config.width, encoder_config.height, o.scale)) {
        fail(status, PipelineStep::Convert); return 1;
    }
    auto encoder = open_encoder(device.Get(), converter.output(), encoder_config, peer_codecs, o.backend);
    if (!encoder) {
        fail(status, PipelineStep::Encode);
        return 1;
    }
    if (status) {
        status->codec = encoder->codec();
        status->encoder_name = std::string(encoder->name()) == "NVENC" ? "NVENC" : "Media Foundation";
    }
    std::fprintf(stderr, "Selected encoder: %s, codec=%s\n", encoder->name(), lls_codec_name(encoder->codec()));
    File hevc;
    if (!o.hevc.empty()) {
        hevc.reset(std::fopen(o.hevc.c_str(), "wb"));
        if (!hevc) { fail(status, PipelineStep::Files); throw std::runtime_error("cannot open HEVC file"); }
        std::fprintf(stderr, "%s dump enabled; file I/O can affect frame timing\n", lls_codec_name(encoder->codec()));
    }
    LlsSenderConfig sender_config = o.sender;
    sender_config.codec = encoder->codec();
    sender_config.budget_bps = o.encoder.bitrate_bps;
    sender_config.fps = o.encoder.fps;
    Profile profile(lls_sender_profile_open(&o.profile, "llstream_win", "QPC_us"));
    if (lls_profile_enabled(&o.profile) && !profile) { fail(status, PipelineStep::Files); return 1; }
    sender_config.profile = profile.get();
    sender_config.stage_records = bool(profile);
    sender_config.on_recover = Timing::recover;
    sender_config.recover_context = &timing;
    Session sender(o.sender.host ? lls_sender_open(&sender_config) : nullptr);
    if (o.sender.host && !sender) { fail(status, PipelineStep::Network); return 1; }
    StageLog stages;
    std::fprintf(stderr, "Starting video pipeline; profile %s\n", profile ? "enabled" : "off");
    FramePolicy policy(o.encoder.fps);
    uint64_t content_time = 0, retry_at = 0, lost_count = 0, repeated_count = 0, retry_count = 0;
    uint64_t capture_grid = UINT64_MAX, capture_log_at = 0;
    Stage prepared; // converted update survives a repeat until its encode gate
    int64_t content_qpc = 0;
    const uint64_t start = timing.now();
    const uint64_t end = o.seconds ? start + uint64_t(o.seconds * 1000000) : UINT64_MAX;
    auto convert_pattern = [&](Stage &row) {
        LARGE_INTEGER stamp{};
        if (!QueryPerformanceCounter(&stamp)) return false;
        content_qpc = stamp.QuadPart;
        content_time = timing.from_qpc(content_qpc);
        ID3D11Texture2D *source = pattern.Get();
        if (moving_pattern) {
            const unsigned offset = pattern_offset(stages.count(), o.pattern_motion, o.pattern_width);
            for (const auto &copy : pattern_copies(o.pattern_width, offset)) {
                if (!copy.width) continue;
                const D3D11_BOX box = {copy.source_x, 0, 0, copy.source_x + copy.width, o.pattern_height, 1};
                context->CopySubresourceRegion(moving_pattern.Get(), 0, copy.destination_x, 0, 0,
                                               pattern.Get(), 0, &box);
            }
            source = moving_pattern.Get();
        }
        if (!converter.convert(context.Get(), source)) return false;
        row.converted = timing.now();
        return true;
    };
    bool lost = false, ok = true;
    uint64_t clock_at = 0;
    while (!stop &&
           timing.now() < end && (!o.frames || stages.count() < o.frames)) {
        const uint64_t now = timing.now();
        if (lost && now >= retry_at) {
            if (!hr_ok(device->GetDeviceRemovedReason(), "Capture device status")) {
                ok = false; break;
            }
            ++retry_count;
            const bool log = now >= capture_log_at;
            converter.clear_source_cache();
            lost = !capture.reopen(log);
            if (log) capture_log_at = now + 180000000;
            if (status) status->capture_pause = lost ? capture.pause_reason() : nullptr;
            if (!lost)
                std::fprintf(stderr, "Capture restored after %llu attempts; duplication_losses=%llu\n",
                    static_cast<unsigned long long>(retry_count), static_cast<unsigned long long>(lost_count));
            retry_at = now + 250000;
        }
        if (o.pattern && !policy.have_image) {
            std::fprintf(stderr, "Converting first pattern frame\n");
            if (!convert_pattern(prepared)) { ok = false; break; }
        } else if (!o.pattern) {
            CapturedFrame frame;
            DdaCapture::Result result{};
            uint64_t generation = 0;
            const uint64_t acquired = timing.now();
            if (acquisition.take(frame, result, generation)) {
                if (result == DdaCapture::Result::Frame) {
                    // DDA may deliver this update only once. It is newer than
                    // a repeat encoded while the worker was still acquiring.
                    prepared.acquired = timing.from_qpc(acquisition.completed_qpc());
                    prepared.accumulated = frame.accumulated;
                    content_qpc = frame.present_qpc;
                    content_time = timing.from_qpc(content_qpc);
                    const bool converted = converter.convert(context.Get(), frame.texture);
                    prepared.converted = timing.now();
                    capture.release();
                    if (!converted) { ok = false; break; }
                } else if (result == DdaCapture::Result::Lost) {
                    lost = true; retry_at = acquired; ++lost_count;
                    retry_count = 0;
                    capture_log_at = 0;
                    if (status) status->capture_pause = "desktop or display mode changed";
                    std::fprintf(stderr, "Capture interrupted; repeating the last image while reopening\n");
                } else if (result == DdaCapture::Result::Error) {
                    ok = false; break;
                }
            }
        }
        const bool urgent = sender && lls_sender_recover_now(sender.get());
        if (!lls_frame_policy_should_encode(&policy, timing.now(), prepared.converted != 0, urgent)) {
            uint64_t deadline = std::min(end, lls_frame_policy_deadline(&policy, prepared.converted != 0));
            if (lost) deadline = std::min(deadline, retry_at);
            else if (!o.pattern && !prepared.converted && !acquisition.pending() &&
                     (!policy.have_image || capture_grid != policy.repeat_due_us)) {
                if (policy.may_acquire(timing.now())) {
                    const uint64_t current = timing.now();
                    const unsigned wait = capture_timeout_ms(current, deadline);
                    if (!acquisition.start(wait, stages.count())) { ok = false; break; }
                    capture_grid = policy.repeat_due_us;
                } else deadline = std::min(deadline, lls_frame_policy_deadline(&policy, true));
            }
            timing.wait_until(deadline, acquisition.event());
            continue;
        }
        Stage row = prepared;
        prepared = {};
        row.frame = uint32_t(stages.count());
        // Decide to encode first, then update the moving pattern. A deadline
        // or RECOVER arriving between the two checks must not repeat an old
        // offset under a new frame number.
        if (o.pattern_motion && !row.converted && !convert_pattern(row)) { ok = false; break; }
        row.repeated = row.converted == 0;
        repeated_count += row.repeated;
        row.present = content_time;
        row.present_qpc = content_qpc;
        row.encode_start = timing.now();
        // Synthetic motion changes pixels on the fixed repeat grid. Keep its
        // cadence anchored even though this frame's profile repeated flag is false.
        policy.encoded(row.encode_start, row.repeated || (o.pattern && policy.have_image));
        EncodedFrame encoded;
        if (!stages.count()) std::fprintf(stderr, "Encoding first frame\n");
        if (!encoder->encode((sender && lls_sender_need_keyframe(sender.get())) || !stages.count(), encoded, stop)) {
            ok = false; break;
        }
        if (!stages.count()) std::fprintf(stderr, "First encoded frame: %zu bytes\n", encoded.size);
        row.locked = timing.from_qpc(encoded.done_qpc);
        row.encode_start = timing.from_qpc(encoded.submit_qpc);
        row.bytes = encoded.size;
        row.key = encoded.key;
        const LlsSenderFrame frame = {encoded.data, encoded.size, encoded.key, row.present, row.locked};
        if (sender) {
            row.sent = lls_sender_send(sender.get(), &frame, &row.frame);
            row.send_return = timing.now();
            if (!stages.count()) std::fprintf(stderr, "First frame send: %s\n", row.sent ? "ok" : "failed");
            if (!row.sent) {
                int64_t offset; uint32_t rtt;
                if (lls_sender_clock(sender.get(), &offset, &rtt) &&
                    !(lls_sender_peer_codecs(sender.get()) & LLS_CODEC_BIT(sender_config.codec)))
                    fail(status, PipelineStep::Codec);
                ok = false;
            }
        }
        if (hevc) {
            if (std::fwrite(encoded.data, 1, encoded.size, hevc.get()) != encoded.size) ok = false;
            row.file_write_return = timing.now();
        }
        if (profile) {
            const LlsSenderStages detail = {row.accumulated, row.present_qpc, row.acquired, row.converted,
                row.encode_start, row.send_return, row.file_write_return, row.repeated};
            if (sender) {
                if (row.sent && !lls_sender_set_stages(sender.get(), row.frame, &detail)) ok = false;
            } else lls_sender_profile_local(profile.get(), row.frame, &frame, &detail);
        }
        stages.append(row);
        if (status) {
            ++status->frames;
            if (sender) status->bytes = lls_sender_sent_bytes(sender.get());
            status->keys += encoded.key;
            if (sender && row.send_return >= clock_at) {
                int64_t offset = 0;
                uint32_t rtt = 0;
                status->rtt_us = lls_sender_clock(sender.get(), &offset, &rtt) ? rtt : UINT32_MAX;
                LlsQuality quality{};
                if (!lls_sender_quality(sender.get(), &quality)) quality = LlsQuality{};
                std::lock_guard<std::mutex> lock(status->quality_lock);
                status->quality = quality;
                clock_at = row.send_return + 250000;
            }
        }
        if (!ok) break;
    }
    std::fprintf(stderr, "Stopping pipeline: waiting for capture worker\n");
    acquisition.close();
    std::fprintf(stderr, "Stopping pipeline: releasing captured frame\n");
    capture.release();
    std::fprintf(stderr, "Stopping pipeline: closing encoder\n");
    encoder.reset();
    if (!stages.count()) { std::fprintf(stderr, "No video frames produced\n"); ok = false; }
    if (hevc && std::fclose(hevc.release())) ok = false;
    // Allow final feedback to arrive before the session writes reports.
    if (sender) {
        std::fprintf(stderr, "Stopping pipeline: closing sender session\n");
        const uint64_t drain = timing.now() + 100000;
        timing.wait_until(drain, nullptr, false);
        if (!lls_sender_close(sender.release())) ok = false;
    }
    stages.close();
    std::printf("Pipeline frames=%llu repeated=%llu duplication_losses=%llu\n",
        static_cast<unsigned long long>(stages.count()),
        static_cast<unsigned long long>(repeated_count), static_cast<unsigned long long>(lost_count));
    if (!ok && status && status->failed == PipelineStep::None) fail(status, PipelineStep::Running);
    return ok ? 0 : 1;
}
} // namespace lls
