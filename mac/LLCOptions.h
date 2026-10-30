#ifndef LLC_OPTIONS_H
#define LLC_OPTIONS_H
#include "lls_profile.h"
#include "llstream_proto.h"
#include <string.h>

/* argv lives for the process lifetime. No saved preferences or network I/O. */
typedef struct {
    const char *host, *port;
    bool hd720, scale, selftest, help;
    uint8_t codec; // 0 Auto; otherwise LLS_CODEC_*
    LlsProfileConfig profile;
} LLCOptions;

static inline const char *LLCParseOptions(int argc, const char *const *argv, LLCOptions *o)
{
    *o = (LLCOptions){.port = "47800", .scale = true};
    bool send_option = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strncmp(arg, "-psn_", 5)) continue; // Launch Services
        if (!strcmp(arg, "-NSDocumentRevisionsDebugMode") && i + 1 < argc) { ++i; continue; }
        if (!strcmp(arg, "--help")) { o->help = true; continue; }
        if (!strcmp(arg, "--selftest")) { o->selftest = true; continue; }
        if (!strcmp(arg, "--no-scale")) { o->scale = false; send_option = true; continue; }
        if (strcmp(arg, "--send") && strcmp(arg, "--port") && strcmp(arg, "--mode") && strcmp(arg, "--codec") &&
            strcmp(arg, "--profile") && strcmp(arg, "--profile-to") && strcmp(arg, "--profile-capacity"))
            return "Unknown command-line option.";
        if (++i == argc || !*argv[i] || !strncmp(argv[i], "--", 2)) return "The option needs a value.";
        const char *value = argv[i];
        if (!strcmp(arg, "--send")) {
            if (o->host) return "Use --send once.";
            o->host = value;
        } else if (!strcmp(arg, "--port")) { o->port = value; send_option = true; }
        else if (!strcmp(arg, "--mode")) {
            if (strcmp(value, "720p") && strcmp(value, "1080p")) return "Mode must be 720p or 1080p.";
            o->hd720 = !strcmp(value, "720p"); send_option = true;
        } else if (!strcmp(arg, "--codec")) {
            if (!strcmp(value, "auto")) o->codec = 0;
            else if (!strcmp(value, "av1")) o->codec = LLS_CODEC_AV1;
            else if (!strcmp(value, "hevc")) o->codec = LLS_CODEC_HEVC;
            else if (!strcmp(value, "h264")) o->codec = LLS_CODEC_H264;
            else return "Codec must be auto, av1, hevc or h264.";
            send_option = true;
        } else if (!strcmp(arg, "--profile-capacity")) {
            uint32_t n = 0;
            for (const char *p = value; *p; ++p) {
                if (*p < '0' || *p > '9' || n > 100000) return "Profile capacity must be 1 to 1000000.";
                n = n * 10 + (uint32_t)(*p - '0');
            }
            if (!n || n > 1000000) return "Profile capacity must be 1 to 1000000.";
            o->profile.capacity = n;
        } else {
            if (o->profile.listen || o->profile.connect) return "Choose one --profile or --profile-to endpoint.";
            if (!strcmp(arg, "--profile")) o->profile.listen = value;
            else o->profile.connect = value;
        }
    }
    const bool profiling = o->profile.listen || o->profile.connect;
    if (o->profile.capacity && !profiling) return "--profile-capacity requires --profile or --profile-to.";
    if (send_option && !o->host) return "--port, --mode, --codec and --no-scale require --send.";
    if (o->selftest && (o->host || profiling || send_option)) return "--selftest runs alone.";
    return NULL;
}
#endif
