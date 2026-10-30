#include "check.h"
#include <initializer_list>
#include <vector>
#include "../sender/win/llcast/options.h"
#include "../sender/win/precheck.h"
#define main llstream_cli_main
#include "../sender/win/llstream_win.cpp"
#undef main

static Options read(std::initializer_list<const char *> arguments)
{
    std::vector<char *> argv{const_cast<char *>("llstream_win")};
    for (auto arg : arguments) argv.push_back(const_cast<char *>(arg));
    Options o;
    CHECK(parse(int(argv.size()), argv.data(), o));
    return o;
}

static bool accepts(std::initializer_list<const char *> arguments)
{
    try { read(arguments); return true; }
    catch (const std::invalid_argument &) { return false; }
}

static void app_options()
{
    // One receive address: default-route metric wins; active LAN is the fallback.
    const llcast::ReceiveAddressRank ethernet{true, true, 25, 8};
    const llcast::ReceiveAddressRank wifi{true, true, 50, 3};
    const llcast::ReceiveAddressRank virtual_link{false, false, UINT64_MAX, 1};
    const llcast::ReceiveAddressRank lan_only{false, true, UINT64_MAX, 5};
    CHECK(ethernet < wifi && !(wifi < ethernet));
    CHECK(wifi < virtual_link && wifi < lan_only);
    CHECK(lan_only < virtual_link);
    CHECK((llcast::ReceiveAddressRank{false, true, UINT64_MAX, 4} < lan_only));
    CHECK(!(ethernet < ethernet));
    CHECK(llcast::codec_from_selection(0) == 0);
    CHECK(llcast::codec_from_selection(1) == LLS_CODEC_AV1);
    CHECK(llcast::codec_from_selection(2) == LLS_CODEC_HEVC);
    CHECK(llcast::codec_from_selection(3) == LLS_CODEC_H264);
    CHECK(llcast::codec_from_selection(-1) == 0 && llcast::codec_from_selection(4) == 0);
    CHECK(llcast::codec_selection(UINT32_MAX) == 0);
    for (uint8_t codec : {uint8_t(0), uint8_t(LLS_CODEC_AV1), uint8_t(LLS_CODEC_HEVC), uint8_t(LLS_CODEC_H264)})
        CHECK(llcast::codec_from_selection(llcast::codec_selection(codec)) == codec);
    // A forced mismatch is explained before probing the PC's GPU.
    lls::EncoderConfig config;
    config.codec = LLS_CODEC_AV1;
    lls::Problem problem;
    CHECK(!lls::check_encoder(LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264),
        config, problem, lls::EncoderBackend::Nvenc));
    CHECK(problem.headline == L"The receiver can't decode AV1");
    CHECK(problem.advice.find(L"HEVC and H.264") != std::wstring::npos &&
          problem.advice.find(L"Auto") != std::wstring::npos);
    config.codec = LLS_CODEC_HEVC;
    CHECK(!lls::check_encoder(LLS_CODEC_BIT(LLS_CODEC_H264), config, problem, lls::EncoderBackend::MediaFoundation));
    CHECK(problem.headline == L"The receiver can't decode HEVC");
    config.codec = 0;
    CHECK(!lls::check_encoder(LLS_CODEC_BIT(LLS_CODEC_AV1), config, problem, lls::EncoderBackend::Nvenc));
    CHECK(problem.headline == L"Auto has no codec in common with the receiver" &&
          problem.advice.find(L"AV1 explicitly") != std::wstring::npos);
    llcast::Options o;
    const auto parse_app = [&](std::initializer_list<const wchar_t *> arguments) {
        std::vector<const wchar_t *> argv{L"LLCast.exe"};
        argv.insert(argv.end(), arguments.begin(), arguments.end());
        return llcast::parse_options(int(argv.size()), argv.data(), o) == nullptr;
    };
    CHECK(parse_app({}));
    CHECK(!o.profile().listen && !o.profile().connect && !o.profile().capacity);
    CHECK(parse_app({L"--profile", L"47801"}));
    CHECK(o.listen == "47801" && !o.profile().connect);
    CHECK(parse_app({L"--profile-capacity", L"10", L"--profile-to", L"vm:47801"}));
    CHECK(o.connect == "vm:47801" && o.profile().capacity == 10);
    const llcast::Options saved = o;
    CHECK(parse_app({L"--profile", L"[::1]:47801"}));
    CHECK(std::string(saved.profile().connect) == "vm:47801"); // endpoint lifetime independent of argv/o
    CHECK(!parse_app({L"--profile"}));
    CHECK(!parse_app({L"--profile", L""}));
    CHECK(!parse_app({L"--profile", L"--profile-to", L"vm:47801"}));
    CHECK(!parse_app({L"--profile", L"47801", L"--profile-to", L"vm:47801"}));
    CHECK(!parse_app({L"--profile-capacity", L"10"}));
    CHECK(!parse_app({L"--profile", L"47801", L"--profile-capacity", L"1000001"}));
    CHECK(!parse_app({L"--profile", L"47801", L"--profile-capacity", L"-1"}));
    CHECK(!parse_app({L"--profile", L"47801", L"--profile-capacity", L"0"}));
    CHECK(!parse_app({L"--garbage"}));
    CHECK(parse_app({L"--help"}) && o.help);
}

int main()
{
    app_options();
    CHECK(accepts({"--host", "127.0.0.1"}));
    CHECK(accepts({"--host", "127.0.0.1", "--profile", "47801"}));
    CHECK(accepts({"--host", "127.0.0.1", "--profile-to", "127.0.0.1:47801", "--profile-capacity", "10"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--profile", "47801", "--profile-to", "127.0.0.1:47802"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--profile-capacity", "10"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--csv", "rx.csv"}));
    auto defaults = read({"--host", "127.0.0.1"});
    CHECK(defaults.encoder.width == 1920 && defaults.encoder.height == 1080 &&
          defaults.encoder.bitrate_bps == 20000000 && defaults.encoder.fps == 60 && defaults.scale);
    auto hd = read({"--host", "127.0.0.1", "--mode", "720p", "--no-scale"});
    CHECK(hd.encoder.width == 1280 && hd.encoder.height == 720 &&
          hd.encoder.bitrate_bps == 4500000 && hd.encoder.fps == 30 && !hd.scale);
    auto avc30 = read({"--host", "127.0.0.1", "--size", "1920x1080", "--fps", "30", "--codec", "h264"});
    CHECK(avc30.encoder.width == 1920 && avc30.encoder.height == 1080 && avc30.encoder.fps == 30 &&
          avc30.encoder.bitrate_bps == 15000000);
    auto override = read({"--host", "127.0.0.1", "--bitrate-mbps", "6", "--mode", "720p"});
    CHECK(override.encoder.width == 1280 && override.encoder.bitrate_bps == 6000000);
    CHECK(!accepts({"--host", "127.0.0.1", "--mode", "2160p"}));
    CHECK(accepts({"--host", "127.0.0.1", "--bitrate-mbps", "31"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--bitrate-mbps", "201"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--output", "test.hevc"}));
    CHECK(accepts({"--host", "127.0.0.1", "--output", "test.hevc", "--frames", "1"}));
    CHECK(accepts({"--output", "test.hevc", "--seconds", "1"}));
    auto mf = read({"--output", "test.h264", "--seconds", "1", "--encoder", "mf", "--codec", "h264"});
    CHECK(mf.backend == lls::EncoderBackend::MediaFoundation && mf.encoder.codec == LLS_CODEC_H264);
    CHECK(read({"--host", "127.0.0.1", "--codec", "av1", "--mode", "720p"}).encoder.codec == LLS_CODEC_AV1);
    CHECK(read({"--host", "127.0.0.1", "--codec", "auto"}).encoder.codec == 0);
    auto nv264 = read({"--host", "127.0.0.1", "--encoder", "nvenc", "--codec", "h264", "--mode", "720p"});
    CHECK(nv264.backend == lls::EncoderBackend::Nvenc && nv264.encoder.codec == LLS_CODEC_H264);
    CHECK(read({"--host", "127.0.0.1", "--encoder", "nvenc"}).backend == lls::EncoderBackend::Nvenc);
    CHECK(!accepts({"--host", "127.0.0.1", "--encoder", "software"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--codec", "vp9"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--fec-min", "1"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--fec-percent", "0", "--fec-min", "1"}));
    CHECK(accepts({"--host", "127.0.0.1", "--fec-min", "1", "--fec-percent", "20"}));
    for (const char *arg : {"--fec-min", "--fec-percent", "--drop-rate", "--pace-mbps"})
        CHECK(!accepts({"--output", "test.hevc", "--seconds", "1", arg, "0"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--pattern-width", "1280"}));
    CHECK(!accepts({"--host", "127.0.0.1", "--pattern-height", "720"}));
    CHECK(accepts({"--host", "127.0.0.1", "--pattern-width", "1280", "--pattern"}));
    CHECK(accepts({"--host", "127.0.0.1", "--pattern-height", "720", "--pattern-motion", "8"}));
    puts("Windows options: diagnostic bounds and meaningful combinations verified");
}
