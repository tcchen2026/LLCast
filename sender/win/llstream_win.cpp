// Test CLI for the Windows pipeline; LLCast.exe is the window for users.
// One pipeline thread; the shared C sender owns network feedback and recovery.
#include "pipeline.h"
#include "win_probe.h"
extern "C" {
#include "lls_stream_profile.h"
}

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
volatile long stopping = 0;
LONG WINAPI on_exception(EXCEPTION_POINTERS *exception)
{
    const auto *record = exception->ExceptionRecord;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "unknown";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCSTR>(record->ExceptionAddress), &module);
    if (module) GetModuleFileNameA(module, name, sizeof(name));
    std::fprintf(stderr, "Unhandled Windows exception 0x%08lx at %p (%s+0x%llx)\n",
        record->ExceptionCode, record->ExceptionAddress, name,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record->ExceptionAddress) -
                                       reinterpret_cast<uintptr_t>(module)));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        std::fprintf(stderr, "Access operation=%llu address=0x%llx\n",
            static_cast<unsigned long long>(record->ExceptionInformation[0]),
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    return EXCEPTION_EXECUTE_HANDLER;
}

BOOL WINAPI on_console(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&stopping, 1);
    return TRUE;
}

struct Options : lls::PipelineOptions {
    bool info = false;
};

void usage()
{
    std::puts("llstream_win --info [--display N]\n"
        "llstream_win --output FILE.hevc --seconds N [--profile 47801]\n"
        "llstream_win --host BOX [--port 47800] [--display N] [--mode 720p|1080p]\n"
        "  [--size WxH] [--fps N] [--no-scale] [--bitrate-mbps N] (video + FEC budget, 0.5 to 200)\n"
        "  [--encoder auto|nvenc|mf] [--codec auto|av1|hevc|h264] (hardware only)\n"
        "  [--seconds N] [--frames N] [--pace-mbps N] [--no-qos]\n"
        "  [--profile [ADDR:]PORT | --profile-to HOST:PORT] [--profile-capacity N]\n"
        "  [--fec-percent 0..100] [--fec-min 1] [--drop-rate 0.01] (test loss)\n"
        "  [--output capture.hevc] [--pattern [--pattern-width W --pattern-height H]]\n"
        "  [--pattern-motion N] (implies --pattern; scroll right N pixels per encoded frame)\n"
        "Profiles as in LLCast.exe: 720p is Low (1280x720 at 30 fps), 1080p is Medium (1920x1080\n"
        "at 60 fps, the default); --size/--fps make a custom one. Without --bitrate-mbps, the\n"
        "Auto budget for the profile and codec.\n"
        "Aspect-fit scales the whole desktop with black bars; --no-scale crops top-left.\n"
        "Run the EXE/batch file on the logged-in console desktop, with HDR disabled.\n"
        "Default display is primary; --display N indexes attached DXGI outputs.\n"
        "--output requires --seconds or --frames; without --host it writes locally.\n"
        "--pattern substitutes test pixels for desktop capture; --hevc aliases --output.\n"
        "Profiles are off by default, use bounded memory and write no files. Ctrl-C stops.");
}

double number(const char *text, double low, double high)
{
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (errno || end == text || *end || !std::isfinite(value) || value < low || value > high)
        throw std::invalid_argument(std::string("invalid numeric option: ") + text);
    return value;
}

uint64_t integer(const char *text, uint64_t low, uint64_t high)
{
    const double value = number(text, double(low), double(high));
    if (value != std::floor(value)) throw std::invalid_argument("integer option has fraction");
    return uint64_t(value);
}

bool parse(int argc, char **argv, Options &o)
{
    lls_sender_default_config(&o.sender);
    o.sender.qos = true;
    bool network_option = false, fec_percent = false, fec_min = false, pattern_size = false;
    LlsStreamProfile profile = lls_stream_profile(LLS_PROFILE_MEDIUM, nullptr);
    uint32_t bitrate = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") { usage(); return false; }
        if (arg == "--info") { o.info = true; continue; }
        if (arg == "--pattern") { o.pattern = true; continue; }
        if (arg == "--no-qos") { o.sender.qos = false; continue; }
        if (arg == "--no-scale") { o.scale = false; continue; }
        if (++i == argc) throw std::invalid_argument("option needs a value: " + arg);
        const char *value = argv[i];
        if (arg == "--pace-mbps" || arg == "--drop-rate" || arg == "--fec-percent" || arg == "--fec-min")
            network_option = true;
        if (arg == "--fec-percent") fec_percent = true;
        if (arg == "--fec-min") fec_min = true;
        if (arg == "--pattern-width" || arg == "--pattern-height") pattern_size = true;
        if (arg == "--host") o.sender.host = value;
        else if (arg == "--port") o.sender.port = uint16_t(integer(value, 1, 65535));
        else if (arg == "--display") {
            const double index = number(value, -1, 255);
            if (index != std::floor(index)) throw std::invalid_argument("display index must be an integer");
            o.display = int(index);
        }
        else if (arg == "--mode") {
            // The window's profiles; 720p is Low (720p30), 1080p is Medium (1080p60).
            if (!std::strcmp(value, "720p")) profile = lls_stream_profile(LLS_PROFILE_LOW, nullptr);
            else if (!std::strcmp(value, "1080p")) profile = lls_stream_profile(LLS_PROFILE_MEDIUM, nullptr);
            else throw std::invalid_argument("--mode must be 720p or 1080p");
        }
        else if (arg == "--size") {
            unsigned w = 0, h = 0; char end = 0;
            if (std::sscanf(value, "%ux%u%c", &w, &h, &end) != 2) throw std::invalid_argument("--size must be WIDTHxHEIGHT");
            profile.width = w; profile.height = h;
        }
        else if (arg == "--fps") profile.fps = uint32_t(integer(value, 1, LLS_PROFILE_MAX_FPS));
        else if (arg == "--bitrate-mbps") bitrate = uint32_t(number(value, LLS_BUDGET_MIN_MBPS, LLS_BUDGET_MAX_MBPS) * 1000000);
        else if (arg == "--encoder") {
            if (!std::strcmp(value, "auto")) o.backend = lls::EncoderBackend::Auto;
            else if (!std::strcmp(value, "nvenc")) o.backend = lls::EncoderBackend::Nvenc;
            else if (!std::strcmp(value, "mf")) o.backend = lls::EncoderBackend::MediaFoundation;
            else throw std::invalid_argument("--encoder must be auto, nvenc or mf");
        }
        else if (arg == "--codec") {
            if (!std::strcmp(value, "auto")) o.encoder.codec = 0;
            else if (!std::strcmp(value, "hevc")) o.encoder.codec = LLS_CODEC_HEVC;
            else if (!std::strcmp(value, "h264")) o.encoder.codec = LLS_CODEC_H264;
            else if (!std::strcmp(value, "av1")) o.encoder.codec = LLS_CODEC_AV1;
            else throw std::invalid_argument("--codec must be auto, av1, hevc or h264");
        }
        else if (arg == "--pace-mbps") o.sender.pace_mbps = number(value, 0, 1000);
        else if (arg == "--fec-percent") o.sender.fec_percent = unsigned(integer(value, 0, 100));
        else if (arg == "--fec-min") o.sender.fec_min = unsigned(integer(value, 0, 254));
        else if (arg == "--drop-rate") o.sender.drop_rate = number(value, 0, 1);
        else if (arg == "--seconds") o.seconds = number(value, 0.001, 86400);
        else if (arg == "--frames") o.frames = integer(value, 1, UINT32_MAX);
        else if (arg == "--profile") o.profile.listen = value;
        else if (arg == "--profile-to") o.profile.connect = value;
        else if (arg == "--profile-capacity") o.profile.capacity = uint32_t(integer(value, 1, 1000000));
        else if (arg == "--output" || arg == "--hevc") o.hevc = value;
        else if (arg == "--pattern-width") o.pattern_width = unsigned(integer(value, 1, 8192));
        else if (arg == "--pattern-height") o.pattern_height = unsigned(integer(value, 1, 8192));
        else if (arg == "--pattern-motion") {
            o.pattern_motion = unsigned(integer(value, 0, UINT32_MAX));
            o.pattern = true;
        }
        else throw std::invalid_argument("unknown option: " + arg);
    }
    if (!lls_stream_profile_valid(&profile))
        throw std::invalid_argument("--size needs even sides from 160 to 7680, and --fps 1 to 240");
    // No --bitrate-mbps: the window's Auto budget for this profile and codec (Auto counts as HEVC).
    o.encoder = {profile.width, profile.height, profile.fps,
                 bitrate ? bitrate : lls_auto_budget_bps(&profile, o.encoder.codec), o.encoder.codec};
    if (fec_min && (!fec_percent || !o.sender.fec_percent))
        throw std::invalid_argument("--fec-min requires --fec-percent greater than zero");
    if (network_option && !o.sender.host)
        throw std::invalid_argument("--pace-mbps, --drop-rate and --fec-* require --host");
    if (pattern_size && !o.pattern)
        throw std::invalid_argument("--pattern-width/height require --pattern or --pattern-motion");
    if (!o.hevc.empty() && !o.seconds && !o.frames)
        throw std::invalid_argument("--output requires --seconds or --frames");
    if (o.info && (o.sender.host || !o.hevc.empty() || o.pattern))
        throw std::invalid_argument("--info is a separate mode; do not combine with --host, --output or --pattern");
    if (!o.info && !o.sender.host && o.hevc.empty())
        throw std::invalid_argument("provide --info, --output FILE.hevc, or --host IP");
    if (o.profile.listen && o.profile.connect)
        throw std::invalid_argument("choose --profile or --profile-to");
    if (o.profile.capacity && !lls_profile_enabled(&o.profile))
        throw std::invalid_argument("--profile-capacity requires --profile or --profile-to");
    if (o.info && lls_profile_enabled(&o.profile))
        throw std::invalid_argument("--info does not stream profile data");
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(on_exception);
    try {
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
            GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) != DPI_AWARENESS_PER_MONITOR_AWARE)
            throw std::runtime_error("Cannot enable physical-pixel desktop coordinates");
        Options options;
        if (!parse(argc, argv, options)) return 0;
        if (options.info) return lls::print_windows_info(options.display) ? 0 : 1;
        SetConsoleCtrlHandler(on_console, TRUE);
        const int result = lls::run_pipeline(options, stopping);
        SetConsoleCtrlHandler(on_console, FALSE);
        return result;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "llstream_win: %s\n", e.what());
        return 1;
    }
}
