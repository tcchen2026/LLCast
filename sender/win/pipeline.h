#pragma once
// The Windows video pipeline shared by llstream_win (test CLI) and LLCast
// (the window): capture or pattern -> NV12 -> NVENC -> optional UDP session
// and HEVC file, on the calling thread until `stop` is set.

extern "C" {
#include "lls_net.h" // Winsock must precede Windows/D3D headers.
#include "lls_sender.h"
}
#include "video_encoder.h"

#include <atomic>
#include <mutex>
#include <cstdint>
#include <string>

namespace lls {

// LLCast's FEC (user decision): parity of 10% of each frame's data, paid from the budget.
constexpr unsigned APP_FEC_PERCENT = 10;

struct PipelineOptions {
    LlsSenderConfig sender{};      // sender.host == nullptr: no network
    EncoderConfig encoder;         // the profile's size and rate; bitrate is the video + FEC budget
    EncoderBackend backend = EncoderBackend::Auto;
    uint32_t receiver_codecs = 0;   // prechecked capabilities; zero probes at start
    // true (default): the whole display scaled to the encoded size, aspect
    // ratio kept, black bars where the shapes differ. false ("no scale"): the
    // top-left region at full size, black where the display is smaller.
    bool scale = true;
    int display = -1;              // -1: primary
    unsigned pattern_width = 1920, pattern_height = 1080;
    unsigned pattern_motion = 0;
    uint64_t frames = 0;           // 0: no limit
    double seconds = 0;            // 0: no limit
    bool pattern = false;
    LlsProfileConfig profile{};    // no endpoint: no diagnostic service or rings
    std::string hevc;              // empty: no HEVC file
};

// The step that failed, so a front end can explain it in plain words.
enum class PipelineStep { None, Timing, Capture, Convert, Encode, Network, Codec, Files, Running };

struct PipelineStatus {
    std::atomic<uint64_t> frames{0}, bytes{0}, keys{0};
    std::atomic<uint32_t> rtt_us{UINT32_MAX}; // UINT32_MAX: no PONG in the last 5 s
    std::atomic<PipelineStep> failed{PipelineStep::None};
    std::atomic<const char *> capture_pause{nullptr}; // static reason text, nullptr while active
    std::atomic<uint8_t> codec{0};
    std::atomic<const char *> encoder_name{""}; // static backend label
    std::mutex quality_lock;
    LlsQuality quality{}; // level 0: no receiver STATS yet, or none lately
};

// Returns 0 on a clean stop. Diagnostics go to stderr and stdout.
int run_pipeline(const PipelineOptions &options, const volatile long &stop,
                 PipelineStatus *status = nullptr);

} // namespace lls
