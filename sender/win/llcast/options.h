#pragma once
extern "C" {
#include "lls_profile.h"
#include "llstream_proto.h"
}
#include <windows.h>
#include <cwchar>
#include <string>

namespace llcast {
// Choose one usable IPv4 address: the least-cost default route, otherwise
// Wi-Fi/Ethernet before other interfaces. Metrics include the interface cost.
struct ReceiveAddressRank {
    bool default_route = false, lan = false;
    uint64_t metric = UINT64_MAX;
    uint32_t index = UINT32_MAX;
    bool operator<(const ReceiveAddressRank &other) const {
        if (default_route != other.default_route) return default_route;
        if (default_route && metric != other.metric) return metric < other.metric;
        if (lan != other.lan) return lan;
        return index < other.index;
    }
};
// UI order differs from wire IDs. Store the wire ID in HKCU\Software\LLCast\Codec.
inline constexpr struct { uint8_t codec; const wchar_t *label; } CODECS[] = {
    {0, L"Auto"}, {LLS_CODEC_AV1, L"AV1"}, {LLS_CODEC_HEVC, L"HEVC"}, {LLS_CODEC_H264, L"H.264"},
};
inline uint8_t codec_from_selection(int selection)
{
    return selection >= 0 && selection < int(sizeof(CODECS) / sizeof(CODECS[0]))
        ? CODECS[selection].codec : 0;
}
inline int codec_selection(uint32_t codec)
{
    for (int i = 0; i < int(sizeof(CODECS) / sizeof(CODECS[0])); ++i)
        if (CODECS[i].codec == codec) return i;
    return 0;
}

// Own endpoint strings across successive runs of the window's pipeline.
struct Options {
    std::string listen, connect;
    uint32_t capacity = 0;
    bool help = false;
    LlsProfileConfig profile() const {
        return {listen.empty() ? nullptr : listen.c_str(),
                connect.empty() ? nullptr : connect.c_str(), capacity};
    }
};

inline const wchar_t *parse_options(int argc, const wchar_t *const *argv, Options &o)
{
    o = {};
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help") { o.help = true; continue; }
        if (arg != L"--profile" && arg != L"--profile-to" && arg != L"--profile-capacity")
            return L"Unknown command-line option.";
        if (++i == argc || !*argv[i] || !std::wcsncmp(argv[i], L"--", 2))
            return L"The option needs a value.";
        if (arg == L"--profile-capacity") {
            uint32_t n = 0;
            for (const wchar_t *p = argv[i]; *p; ++p) {
                if (*p < L'0' || *p > L'9' || n > 100000) return L"Profile capacity must be 1 to 1000000.";
                n = n * 10 + uint32_t(*p - L'0');
            }
            if (!n || n > 1000000) return L"Profile capacity must be 1 to 1000000.";
            o.capacity = n;
        } else {
            if (!o.listen.empty() || !o.connect.empty()) return L"Choose one --profile or --profile-to endpoint.";
            const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (!n) return L"The profile endpoint is not valid text.";
            std::string text(size_t(n), '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), n, nullptr, nullptr);
            text.pop_back();
            (arg == L"--profile" ? o.listen : o.connect) = std::move(text);
        }
    }
    if (o.capacity && o.listen.empty() && o.connect.empty())
        return L"--profile-capacity requires --profile or --profile-to.";
    return nullptr;
}
} // namespace llcast
