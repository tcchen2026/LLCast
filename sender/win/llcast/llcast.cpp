// LLCast.exe: the window users run on the PC. It checks everything it can
// before streaming and explains any problem in a message; the pipeline is the
// one llstream_win runs. Settings: HKCU\Software\LLCast. Logs (one per
// attempt, newest ten kept): logs beside the EXE, with a LOCALAPPDATA fallback.
#include "pipeline.h"
#include "precheck.h"
extern "C" {
#include "llstream_proto.h"
#include "lls_stream_profile.h"
}
#include "resource.h"
#include "options.h"
#include "receive_window.h"

#include <commctrl.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr UINT WM_APP_STREAMING = WM_APP + 1, WM_APP_DONE = WM_APP + 2;
constexpr UINT_PTR STATUS_TIMER = 1;
constexpr wchar_t SETTINGS_KEY[] = L"Software\\LLCast";

enum class State { Idle, Checking, Streaming, Stopping };

struct App {
    HWND dialog = nullptr;
    State state = State::Idle;
    bool closing = false;
    std::thread worker;
    volatile long stop = 0;
    lls::PipelineStatus status;
    llcast::Options options;
    std::wstring target, mode_label, log_path;
    uint64_t last_frames = 0, last_bytes = 0, last_tick = 0;
} app;

// Sent from the worker with WM_APP_DONE; the dialog deletes it.
struct Outcome {
    bool ok = true;
    lls::Problem problem;
};

std::wstring text_of(HWND dialog, int id)
{
    wchar_t text[256] = L"";
    GetDlgItemTextW(dialog, id, text, 256);
    return text;
}

void set_status(const std::wstring &text) { SetDlgItemTextW(app.dialog, IDC_STATUS, text.c_str()); }

// The status, reception and the doctor's hint included, for a message or a report.
void copy_status(HWND dialog)
{
    const HWND status = GetDlgItem(dialog, IDC_STATUS);
    std::wstring text(size_t(GetWindowTextLengthW(status)) + 1, L'\0');
    text.resize(size_t(GetWindowTextW(status, &text[0], int(text.size()))));
    std::wstring lines;
    for (wchar_t c : text) lines += c == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, c);
    if (!OpenClipboard(dialog)) return;
    EmptyClipboard();
    const size_t bytes = (lines.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void *target = GlobalLock(memory)) {
            std::memcpy(target, lines.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else GlobalFree(memory);
    }
    CloseClipboard();
}

void show_problem(const lls::Problem &problem)
{
    std::wstring detail = problem.detail;
    if (!app.log_path.empty()) detail += (detail.empty() ? L"" : L"\n") + (L"Log: " + app.log_path);
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = app.dialog;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = L"LLCast";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = problem.headline.c_str();
    config.pszContent = problem.advice.c_str();
    config.pszExpandedInformation = detail.empty() ? nullptr : detail.c_str();
    config.pszExpandedControlText = L"Hide details";
    config.pszCollapsedControlText = L"Details";
    if (FAILED(TaskDialogIndirect(&config, nullptr, nullptr, nullptr))) {
        const std::wstring text = problem.advice + (detail.empty() ? L"" : L"\n\n" + detail);
        MessageBoxW(app.dialog, (problem.headline + L"\n\n" + text).c_str(), L"LLCast", MB_OK | MB_ICONWARNING);
    }
}

// Profiles (user decision), in LLS_PROFILE_* order; the default is Medium.
const wchar_t *const PROFILES[] = {L"Low \u00b7 1280\u00d7720 \u00b7 30 fps",
                                   L"Medium \u00b7 1920\u00d71080 \u00b7 60 fps", L"Custom"};

// What the window asks for: a profile and a bandwidth budget (user decision).
struct Choice {
    DWORD profile = LLS_PROFILE_MEDIUM;
    LlsStreamProfile custom{1920, 1080, 60};
    bool auto_budget = true;
    uint32_t budget_bps = 20000000; // typed, when not auto
};

uint32_t number_of(HWND dialog, int id) { return GetDlgItemInt(dialog, id, nullptr, FALSE); }

LlsStreamProfile chosen_profile(HWND dialog)
{
    const LRESULT profile = SendDlgItemMessageW(dialog, IDC_PROFILE, CB_GETCURSEL, 0, 0);
    const LlsStreamProfile custom{number_of(dialog, IDC_WIDTH), number_of(dialog, IDC_HEIGHT), number_of(dialog, IDC_FPS)};
    return lls_stream_profile(int(profile), &custom);
}

std::wstring mbps_text(uint32_t bps)
{
    wchar_t text[32];
    std::swprintf(text, 32, bps % 1000000 ? L"%.1f" : L"%.0f", bps / 1e6);
    return text;
}

// Auto shows its value in the box, for the chosen codec (Auto counts as HEVC).
void show_budget(HWND dialog)
{
    const bool automatic = IsDlgButtonChecked(dialog, IDC_BUDGET_AUTO) == BST_CHECKED;
    if (automatic) {
        const LlsStreamProfile profile = chosen_profile(dialog);
        const uint8_t codec = llcast::codec_from_selection(int(SendDlgItemMessageW(dialog, IDC_CODEC, CB_GETCURSEL, 0, 0)));
        SetDlgItemTextW(dialog, IDC_BUDGET, lls_stream_profile_valid(&profile)
            ? mbps_text(lls_auto_budget_bps(&profile, codec)).c_str() : L"");
    }
    EnableWindow(GetDlgItem(dialog, IDC_BUDGET), !automatic && app.state == State::Idle);
}

// Low and Medium show their size; only Custom can be edited.
void show_profile(HWND dialog, const LlsStreamProfile &custom)
{
    const LRESULT profile = SendDlgItemMessageW(dialog, IDC_PROFILE, CB_GETCURSEL, 0, 0);
    const LlsStreamProfile shown = lls_stream_profile(int(profile), &custom);
    SetDlgItemInt(dialog, IDC_WIDTH, shown.width, FALSE);
    SetDlgItemInt(dialog, IDC_HEIGHT, shown.height, FALSE);
    SetDlgItemInt(dialog, IDC_FPS, shown.fps, FALSE);
    for (int id : {IDC_WIDTH, IDC_HEIGHT, IDC_FPS})
        EnableWindow(GetDlgItem(dialog, id), profile == LLS_PROFILE_CUSTOM && app.state == State::Idle);
    show_budget(dialog);
}

// The window's choice, or false with the problem to show.
bool read_choice(HWND dialog, Choice &choice, lls::Problem &problem)
{
    LRESULT profile = SendDlgItemMessageW(dialog, IDC_PROFILE, CB_GETCURSEL, 0, 0);
    choice.profile = profile >= 0 && profile < LLS_PROFILE_COUNT ? DWORD(profile) : DWORD(LLS_PROFILE_MEDIUM);
    if (choice.profile == LLS_PROFILE_CUSTOM) {
        choice.custom = {number_of(dialog, IDC_WIDTH), number_of(dialog, IDC_HEIGHT), number_of(dialog, IDC_FPS)};
        if (!lls_stream_profile_valid(&choice.custom)) {
            problem = {L"The custom profile isn't usable",
                       L"Use an even width and height from 160 to 7680, and a frame rate from 1 to 240 fps.", L""};
            return false;
        }
    }
    choice.auto_budget = IsDlgButtonChecked(dialog, IDC_BUDGET_AUTO) == BST_CHECKED;
    if (!choice.auto_budget) {
        char typed[64];
        GetDlgItemTextA(dialog, IDC_BUDGET, typed, sizeof(typed));
        if (!lls_budget_parse(typed, &choice.budget_bps)) {
            problem = {L"The bandwidth budget isn't usable", L"Type a number of Mbps from 0.5 to 200, or choose Auto.", L""};
            return false;
        }
    }
    return true;
}
// The user's choice of encoder; the default is NVENC when an NVIDIA GPU is present.
const struct { lls::EncoderBackend backend; const wchar_t *label; } ENCODERS[] = {
    {lls::EncoderBackend::Nvenc, L"NVENC (low latency, needs an NVIDIA GPU)"},
    {lls::EncoderBackend::MediaFoundation, L"Media Foundation (any GPU)"},
};

bool nvidia_present()
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 desc{};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10de &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return true;
    }
    return false;
}

DWORD setting(const wchar_t *name, DWORD fallback)
{
    DWORD value = fallback, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value;
}

void load_settings(HWND dialog)
{
    wchar_t host[256] = L"";
    DWORD size = sizeof(host);
    RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Host", RRF_RT_REG_SZ, nullptr, host, &size);
    SetDlgItemTextW(dialog, IDC_HOST, host);
    for (const wchar_t *profile : PROFILES)
        SendDlgItemMessageW(dialog, IDC_PROFILE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile));
    // Before profiles, "Mode" was 0 for 720p and 1 for 1080p.
    const DWORD profile = setting(L"Profile", setting(L"Mode", 1) == 0 ? DWORD(LLS_PROFILE_LOW) : DWORD(LLS_PROFILE_MEDIUM));
    SendDlgItemMessageW(dialog, IDC_PROFILE, CB_SETCURSEL, profile < LLS_PROFILE_COUNT ? profile : DWORD(LLS_PROFILE_MEDIUM), 0);
    for (const auto &encoder : ENCODERS)
        SendDlgItemMessageW(dialog, IDC_ENCODER, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(encoder.label));
    const DWORD encoder = setting(L"Encoder", nvidia_present() ? 0 : 1);
    SendDlgItemMessageW(dialog, IDC_ENCODER, CB_SETCURSEL, encoder < 2 ? encoder : 0, 0);
    for (const auto &codec : llcast::CODECS)
        SendDlgItemMessageW(dialog, IDC_CODEC, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(codec.label));
    SendDlgItemMessageW(dialog, IDC_CODEC, CB_SETCURSEL, llcast::codec_selection(setting(L"Codec", 0)), 0);
    CheckDlgButton(dialog, IDC_NOSCALE, setting(L"NoScale", 0) ? BST_CHECKED : BST_UNCHECKED);
    const bool automatic = setting(L"BudgetAuto", 1) != 0;
    CheckDlgButton(dialog, IDC_BUDGET_AUTO, automatic ? BST_CHECKED : BST_UNCHECKED);
    if (!automatic) SetDlgItemTextW(dialog, IDC_BUDGET, mbps_text(setting(L"BudgetKbps", 20000) * 1000).c_str());
    show_profile(dialog, {setting(L"CustomWidth", 1920), setting(L"CustomHeight", 1080), setting(L"CustomFps", 60)});
}

// "address" or "address:port" ("[v6]:port" for IPv6), as receivers show it; without a
// port, the default. A bare IPv6 address has no port.
void split_target(const std::wstring &text, std::wstring &host, std::wstring &port)
{
    port = std::to_wstring(LLS_DEFAULT_PORT);
    if (!text.empty() && text[0] == L'[') {
        const size_t close = text.find(L']');
        if (close == std::wstring::npos) { host = text; port.clear(); return; }
        host = text.substr(1, close - 1);
        const std::wstring rest = text.substr(close + 1);
        if (!rest.empty()) port = rest[0] == L':' ? rest.substr(1) : L"";
        return;
    }
    const size_t colon = text.find(L':');
    if (colon == std::wstring::npos || colon != text.rfind(L':')) { host = text; return; }
    host = text.substr(0, colon);
    port = text.substr(colon + 1);
}

void save_dword(const wchar_t *name, DWORD value)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, REG_DWORD, &value, sizeof(value));
}

void save_settings(const std::wstring &typed, const Choice &choice, DWORD encoder, DWORD codec, bool no_scale)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Host", REG_SZ, typed.c_str(),
                    DWORD((typed.size() + 1) * sizeof(wchar_t)));
    save_dword(L"Profile", choice.profile);
    if (choice.profile == LLS_PROFILE_CUSTOM) {
        save_dword(L"CustomWidth", choice.custom.width);
        save_dword(L"CustomHeight", choice.custom.height);
        save_dword(L"CustomFps", choice.custom.fps);
    }
    save_dword(L"BudgetAuto", choice.auto_budget);
    if (!choice.auto_budget) save_dword(L"BudgetKbps", choice.budget_bps / 1000);
    save_dword(L"NoScale", no_scale);
    save_dword(L"Encoder", encoder);
    save_dword(L"Codec", codec);
}

// Points stdout/stderr at a new log for this stream and prunes old logs.
std::wstring open_log()
{
    wchar_t base[MAX_PATH] = {};
    std::vector<std::wstring> candidates;
    DWORD n = GetModuleFileNameW(nullptr, base, MAX_PATH);
    if (n && n < MAX_PATH) {
        const std::wstring exe(base, n);
        const auto slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) candidates.push_back(exe.substr(0, slash) + L"\\logs");
    }
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n && n < MAX_PATH) {
        const std::wstring local = std::wstring(base) + L"\\LLCast";
        CreateDirectoryW(local.c_str(), nullptr);
        candidates.push_back(local + L"\\logs");
    }
    wchar_t name[64];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::wcsftime(name, 64, L"llcast-%Y%m%d-%H%M%S", &local);
    const std::wstring filename = std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".log";
    std::wstring dir, path;
    for (const auto &candidate : candidates) {
        CreateDirectoryW(candidate.c_str(), nullptr);
        const std::wstring file = candidate + L"\\" + filename;
        FILE *probe = _wfopen(file.c_str(), L"wb");
        if (!probe) continue;
        std::fclose(probe);
        dir = candidate; path = file;
        break;
    }
    if (path.empty()) return {};
    std::vector<std::wstring> logs;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((dir + L"\\llcast-*.log").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do logs.push_back(found.cFileName); while (FindNextFileW(search, &found));
        FindClose(search);
    }
    std::sort(logs.begin(), logs.end()); // names are timestamps
    for (size_t i = 0; i + 10 < logs.size(); ++i)
        if (dir + L"\\" + logs[i] != path) DeleteFileW((dir + L"\\" + logs[i]).c_str());
    if (!_wfreopen(path.c_str(), L"a", stdout) || !_wfreopen(path.c_str(), L"a", stderr)) return {};
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return path;
}

// The last lines of the log, for the problem details.
std::wstring log_tail()
{
    FILE *log = app.log_path.empty() ? nullptr : _wfopen(app.log_path.c_str(), L"rb");
    if (!log) return {};
    std::string text;
    // The last 1200 bytes, or all of a shorter log.
    if (!std::fseek(log, -1200, SEEK_END) || !std::fseek(log, 0, SEEK_SET)) {
        char buffer[1200];
        text.assign(buffer, std::fread(buffer, 1, sizeof(buffer), log));
    }
    std::fclose(log);
    const size_t first = text.find('\n');
    if (text.size() == 1200 && first != std::string::npos) text.erase(0, first + 1);
    std::wstring out;
    for (char c : text)
        if (c != '\r') out += wchar_t(static_cast<unsigned char>(c));
    while (!out.empty() && out.back() == L'\n') out.pop_back();
    return out;
}

void work(std::wstring typed, Choice choice, DWORD encoder_index, uint8_t codec, bool no_scale)
{
    app.log_path = open_log(); // includes failures during precheck
    while (!typed.empty() && iswspace(typed.back())) typed.pop_back();
    while (!typed.empty() && iswspace(typed.front())) typed.erase(0, 1);
    std::wstring host_text, port_text;
    split_target(typed, host_text, port_text);
    const lls::EncoderBackend backend = ENCODERS[encoder_index].backend;
    auto *outcome = new Outcome;
    std::string host;
    uint16_t port = 0;
    uint32_t rtt = 0, codecs = 0;
    const LlsStreamProfile profile = lls_stream_profile(int(choice.profile), &choice.custom);
    lls::EncoderConfig config{profile.width, profile.height, profile.fps, choice.budget_bps, codec};
    lls::Problem &problem = outcome->problem;
    outcome->ok = lls::check_address(host_text, port_text, host, port, problem) &&
        lls::check_desktop(problem) && lls::check_display(problem) &&
        lls::check_receiver(host, port, rtt, codecs, problem);
    // Auto budgets for the codec Auto will pick with this receiver.
    if (choice.auto_budget)
        config.bitrate_bps = lls_auto_budget_bps(&profile, codec ? codec :
            lls_codec_choose(LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264), codecs));
    outcome->ok = outcome->ok && lls::check_encoder(codecs, config, problem, backend);
    if (outcome->ok) {
        save_settings(typed, choice, encoder_index, codec, no_scale);
        lls::PipelineOptions options;
        lls_sender_default_config(&options.sender);
        options.sender.host = host.c_str();
        options.sender.port = port;
        options.sender.qos = true;
        options.sender.fec_percent = lls::APP_FEC_PERCENT; // inside the budget: the encoder gets the rest
        options.receiver_codecs = codecs;
        options.encoder = config;
        options.scale = !no_scale;
        options.backend = backend;
        options.profile = app.options.profile();
        std::fprintf(stderr, "LLCast: streaming %ux%u at %u fps, %u bit/s (%s), %s to %s port %u; "
                     "receiver answered in %u us\n", config.width, config.height, config.fps, config.bitrate_bps,
                     choice.auto_budget ? "auto" : "typed",
                     no_scale ? "top-left unscaled" : "whole display scaled", host.c_str(), unsigned(port), rtt);
        PostMessageW(app.dialog, WM_APP_STREAMING, 0, 0);
        int result = 1;
        try {
            result = lls::run_pipeline(options, app.stop, &app.status);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "LLCast: %s\n", e.what());
        }
        const lls::PipelineStep step = app.status.failed;
        // Stopping before the first frame is not a failure.
        if (result && !(app.stop && step == lls::PipelineStep::Running)) {
            outcome->ok = false;
            problem = lls::explain(step);
            problem.detail = log_tail();
        }
        std::fprintf(stderr, "LLCast: pipeline exit %d\n", result);
    }
    if (!outcome->ok && problem.detail.empty()) problem.detail = log_tail();
    PostMessageW(app.dialog, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(outcome));
}

void set_editing(bool enabled)
{
    for (int id : {IDC_HOST, IDC_PROFILE, IDC_ENCODER, IDC_CODEC, IDC_BUDGET_AUTO, IDC_NOSCALE})
        EnableWindow(GetDlgItem(app.dialog, id), enabled);
    // Custom size and a typed budget follow their own switches.
    for (int id : {IDC_WIDTH, IDC_HEIGHT, IDC_FPS, IDC_BUDGET}) EnableWindow(GetDlgItem(app.dialog, id), FALSE);
    if (enabled) {
        const bool custom = SendDlgItemMessageW(app.dialog, IDC_PROFILE, CB_GETCURSEL, 0, 0) == LLS_PROFILE_CUSTOM;
        for (int id : {IDC_WIDTH, IDC_HEIGHT, IDC_FPS}) EnableWindow(GetDlgItem(app.dialog, id), custom);
        show_budget(app.dialog);
    }
}

void start()
{
    if (app.worker.joinable()) app.worker.join();
    app.stop = 0;
    app.status.frames = app.status.bytes = app.status.keys = 0;
    app.status.rtt_us = UINT32_MAX;
    app.status.failed = lls::PipelineStep::None;
    app.status.capture_pause = nullptr;
    app.status.codec = 0;
    app.status.encoder_name = "";
    app.log_path.clear();
    const std::wstring host = text_of(app.dialog, IDC_HOST);
    Choice choice;
    lls::Problem problem;
    if (!read_choice(app.dialog, choice, problem)) { show_problem(problem); return; }
    LRESULT encoder = SendDlgItemMessageW(app.dialog, IDC_ENCODER, CB_GETCURSEL, 0, 0);
    if (encoder < 0 || encoder > 1) encoder = 0;
    const uint8_t codec = llcast::codec_from_selection(int(SendDlgItemMessageW(app.dialog, IDC_CODEC, CB_GETCURSEL, 0, 0)));
    const bool no_scale = IsDlgButtonChecked(app.dialog, IDC_NOSCALE) == BST_CHECKED;
    app.target = host;
    const LlsStreamProfile profile = lls_stream_profile(int(choice.profile), &choice.custom);
    app.mode_label = std::to_wstring(profile.width) + L"\u00d7" + std::to_wstring(profile.height) + L" \u00b7 " +
        std::to_wstring(profile.fps) + L" fps \u00b7 " + (choice.auto_budget ? L"auto budget" :
        mbps_text(choice.budget_bps) + L" Mbps");
    app.state = State::Checking;
    set_editing(false);
    EnableWindow(GetDlgItem(app.dialog, IDC_START), FALSE);
    set_status(L"Checking this PC and the receiver…");
    app.worker = std::thread(work, host, choice, DWORD(encoder), codec, no_scale);
}

void stop()
{
    InterlockedExchange(&app.stop, 1);
    app.state = State::Stopping;
    EnableWindow(GetDlgItem(app.dialog, IDC_START), FALSE);
    set_status(L"Stopping…");
}

void update_status()
{
    const uint64_t tick = GetTickCount64(), frames = app.status.frames, bytes = app.status.bytes;
    const double seconds = double(tick - app.last_tick) / 1000;
    if (seconds <= 0) return;
    const uint32_t rtt = app.status.rtt_us;
    wchar_t text[256];
    std::swprintf(text, 256, L"Streaming %ls · %hs · %hs to %ls · %.0f fps · %.1f Mbps\n",
        app.mode_label.c_str(), lls_codec_name(app.status.codec), app.status.encoder_name.load(), app.target.c_str(), double(frames - app.last_frames) / seconds,
        double(bytes - app.last_bytes) * 8 / seconds / 1e6);
    std::wstring status = text;
    LlsQuality quality;
    {
        std::lock_guard<std::mutex> lock(app.status.quality_lock);
        quality = app.status.quality;
    }
    if (rtt == UINT32_MAX) status += L"The receiver isn't answering.";
    else if (quality.level) {
        // The receiver's view of the last 5 s; its round trip is part of the latency.
        char line[512];
        wchar_t wide[512];
        lls_quality_describe(&quality, line, sizeof(line));
        if (MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, 512) > 0) status += wide;
    } else status += L"The receiver answers in " + std::to_wstring((rtt + 500) / 1000) + L" ms.";
    if (const char *reason = app.status.capture_pause.load()) {
        std::wstring paused = L"Capture paused (";
        for (; *reason; ++reason) paused += wchar_t(static_cast<unsigned char>(*reason));
        status = paused + L"). Repeating the last image.\n" + status;
    }
    set_status(status);
    app.last_frames = frames; app.last_bytes = bytes; app.last_tick = tick;
}

INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG: {
        app.dialog = dialog;
        app.closing = false;
        const HICON big = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_LLCAST), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
        const HICON small = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_LLCAST), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        SendDlgItemMessageW(dialog, IDC_HOST, EM_SETCUEBANNER, TRUE,
                            reinterpret_cast<LPARAM>(L"for example 192.168.1.50"));
        SendDlgItemMessageW(dialog, IDC_HOST, EM_LIMITTEXT, 253, 0);
        load_settings(dialog);
        const bool empty = text_of(dialog, IDC_HOST).empty();
        SetFocus(GetDlgItem(dialog, empty ? IDC_HOST : IDC_START));
        return FALSE; // focus set here
    }
    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID(reinterpret_cast<HWND>(lparam)) == IDC_HINT) {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_CODEC:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const DWORD codec = llcast::codec_from_selection(int(SendDlgItemMessageW(dialog, IDC_CODEC, CB_GETCURSEL, 0, 0)));
                RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Codec", REG_DWORD, &codec, sizeof(codec));
                show_budget(dialog);
            }
            return TRUE;
        case IDC_PROFILE:
            if (HIWORD(wparam) == CBN_SELCHANGE) show_profile(dialog, {setting(L"CustomWidth", 1920),
                setting(L"CustomHeight", 1080), setting(L"CustomFps", 60)});
            return TRUE;
        case IDC_WIDTH: case IDC_HEIGHT: case IDC_FPS:
            if (HIWORD(wparam) == EN_CHANGE) show_budget(dialog);
            return TRUE;
        case IDC_BUDGET_AUTO:
            if (HIWORD(wparam) == BN_CLICKED) show_budget(dialog);
            return TRUE;
        case IDC_COPY:
            copy_status(dialog);
            return TRUE;
        case IDC_START:
            if (app.state == State::Idle) start();
            else if (app.state == State::Streaming) stop();
            return TRUE;
        case IDCANCEL:
            if (app.state == State::Idle) { EndDialog(dialog, 0); return TRUE; }
            app.closing = true;
            if (app.state == State::Streaming) stop();
            return TRUE;
        }
        return FALSE;
    case WM_APP_STREAMING:
        if (app.state != State::Checking) return TRUE;
        app.state = State::Streaming;
        app.last_frames = app.last_bytes = 0;
        app.last_tick = GetTickCount64();
        SetDlgItemTextW(dialog, IDC_START, L"&Stop");
        EnableWindow(GetDlgItem(dialog, IDC_START), TRUE);
        set_status(L"Starting the stream to " + app.target + L"…");
        SetTimer(dialog, STATUS_TIMER, 1000, nullptr);
        if (app.closing) stop();
        return TRUE;
    case WM_TIMER:
        if (wparam == STATUS_TIMER && app.state == State::Streaming) update_status();
        return TRUE;
    case WM_APP_DONE: {
        Outcome *outcome = reinterpret_cast<Outcome *>(lparam);
        KillTimer(dialog, STATUS_TIMER);
        if (app.worker.joinable()) app.worker.join();
        app.state = State::Idle;
        SetDlgItemTextW(dialog, IDC_START, L"&Start");
        EnableWindow(GetDlgItem(dialog, IDC_START), TRUE);
        set_editing(true);
        set_status(outcome->ok ? L"Stopped." : outcome->problem.headline + L".");
        if (!outcome->ok && !app.closing) show_problem(outcome->problem);
        delete outcome;
        if (app.closing) EndDialog(dialog, 0);
        return TRUE;
    }
    }
    return FALSE;
}

LONG WINAPI on_exception(EXCEPTION_POINTERS *exception)
{
    std::fprintf(stderr, "Unhandled Windows exception 0x%08lx at %p\n",
                 exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

// The same first step as the Mac: two role cards, then only that role's UI.
void draw_card(const DRAWITEMSTRUCT &draw)
{
    const bool down = draw.itemState & ODS_SELECTED, focus = draw.itemState & ODS_FOCUS;
    HDC dc = draw.hDC;
    RECT r = draw.rcItem;
    const COLORREF fill = GetSysColor(down ? COLOR_3DSHADOW : COLOR_3DLIGHT);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focus ? 2 : 1, GetSysColor(focus ? COLOR_HIGHLIGHT : COLOR_3DSHADOW));
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
    RoundRect(dc, r.left + 2, r.top + 2, r.right - 2, r.bottom - 2, 16, 16);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN icon_pen = CreatePen(PS_SOLID, 2, GetSysColor(COLOR_BTNTEXT));
    SelectObject(dc, icon_pen);
    const int cx = (r.left + r.right) / 2, cy = r.top + (r.bottom - r.top) / 3;
    const int unit = std::max(12, int((r.bottom - r.top) / 8));
    RoundRect(dc, cx - unit * 2, cy - unit, cx + unit * 2, cy + unit, 4, 4);
    MoveToEx(dc, cx, cy + unit, nullptr); LineTo(dc, cx, cy + unit + 6);
    MoveToEx(dc, cx - unit, cy + unit + 6, nullptr); LineTo(dc, cx + unit, cy + unit + 6);
    const bool send = draw.CtlID == IDC_SEND;
    const int tip = cy + (send ? -unit / 2 : unit / 2), tail = cy + (send ? unit / 2 : -unit / 2);
    MoveToEx(dc, cx, tail, nullptr); LineTo(dc, cx, tip);
    MoveToEx(dc, cx - 5, tip + (send ? 5 : -5), nullptr); LineTo(dc, cx, tip); LineTo(dc, cx + 5, tip + (send ? 5 : -5));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    r.top = r.top + (r.bottom - r.top) * 2 / 3;
    DrawTextW(dc, send ? L"Send" : L"Receive", -1, &r, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, old_pen); SelectObject(dc, old_brush);
    DeleteObject(icon_pen); DeleteObject(pen); DeleteObject(brush);
}

INT_PTR CALLBACK chooser_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG:
        SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LLCAST))));
        SendDlgItemMessageW(dialog, IDC_RECEIVE_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Normal"));
        SendDlgItemMessageW(dialog, IDC_RECEIVE_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Lowest latency"));
        SendDlgItemMessageW(dialog, IDC_RECEIVE_MODE, CB_SETCURSEL, 0, 0);
        SetFocus(GetDlgItem(dialog, IDC_SEND));
        return FALSE;
    case WM_DRAWITEM:
        if (wparam == IDC_SEND || wparam == IDC_RECEIVE) { draw_card(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam)); return TRUE; }
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_SEND:
            ShowWindow(dialog, SW_HIDE);
            DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MAIN), dialog, dialog_proc, 0);
            app.dialog = nullptr;
            ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog); SetFocus(GetDlgItem(dialog, IDC_SEND));
            return TRUE;
        case IDC_RECEIVE: {
            const bool tearing = SendDlgItemMessageW(dialog, IDC_RECEIVE_MODE, CB_GETCURSEL, 0, 0) == 1;
            app.log_path = open_log();
            ShowWindow(dialog, SW_HIDE);
            llcast::receive_window(dialog, tearing, app.options.profile());
            ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog); SetFocus(GetDlgItem(dialog, IDC_RECEIVE));
            return TRUE;
        }
        case IDCANCEL: EndDialog(dialog, 0); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(on_exception);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *error = argv ? llcast::parse_options(argc, argv, app.options) : L"Cannot read command line.";
    if (argv) LocalFree(argv);
    if (error || app.options.help) {
        std::wstring message = error ? std::wstring(error) + L"\n\n" : L"";
        message += L"LLCast.exe [--profile [ADDR:]PORT | --profile-to HOST:PORT] [--profile-capacity N]\n\n"
                   L"Profiling is off by default and starts with the stream. A bare listen port uses loopback.";
        MessageBoxW(nullptr, message.c_str(), L"LLCast", MB_OK | (error ? MB_ICONWARNING : MB_ICONINFORMATION));
        return error ? 2 : 0;
    }
    // The manifest also asks for this; the crop needs physical pixels.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\LLCast.App");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"#32770", L"LLCast")) {
            HWND role = GetLastActivePopup(other);
            ShowWindow(role, SW_RESTORE);
            SetForegroundWindow(role);
        }
        return 0;
    }
    const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CHOOSER), nullptr, chooser_proc, 0);
    if (app.worker.joinable()) app.worker.join();
    if (single) CloseHandle(single);
    return result == -1 ? 1 : 0;
}
