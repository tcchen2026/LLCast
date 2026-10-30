#include "lls_sender.h"
#include "hevc_au.h"
#include "lls_net.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    LlsSenderConfig sender;
    LlsProfileConfig profile;
    const char *input;
    double fps, synthetic_mbps, seconds;
    uint64_t frames;
    unsigned repeat;
} Options;

static volatile sig_atomic_t interrupted;

static void on_signal(int number)
{
    interrupted = number;
}

static bool parse(int argc, char **argv, Options *o)
{
    *o = (Options){.fps = 60, .repeat = 1};
    lls_sender_default_config(&o->sender);
    o->sender.codec = LLS_CODEC_HEVC;
    bool fec_percent = false, fec_min = false;
    if (argc < 2 || argv[1][0] == '-') return false;
    o->sender.host = argv[1];
    for (int i = 2; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--fec-percent")) fec_percent = true;
        if (!strcmp(key, "--fec-min")) fec_min = true;
        if (key[0] != '-') {
            if (o->input) return false;
            o->input = key;
            continue;
        }
        if (!strcmp(key, "--qos")) { o->sender.qos = true; continue; }
        if (++i == argc) return false;
        if (!strcmp(key, "--codec")) {
            if (!strcmp(argv[i], "hevc")) o->sender.codec = LLS_CODEC_HEVC;
            else if (!strcmp(argv[i], "h264")) o->sender.codec = LLS_CODEC_H264;
            else if (!strcmp(argv[i], "av1")) o->sender.codec = LLS_CODEC_AV1;
            else return false;
            continue;
        }
        if (!strcmp(key, "--profile")) { o->profile.listen = argv[i]; continue; }
        if (!strcmp(key, "--profile-to")) { o->profile.connect = argv[i]; continue; }
        char *end = NULL;
        errno = 0;
        double n = strtod(argv[i], &end);
        if (errno || !end || end == argv[i] || *end || !isfinite(n)) return false;
        if (!strcmp(key, "--fps") && n >= 1 && n <= 240) o->fps = n;
        else if (!strcmp(key, "--profile-capacity") && n >= 1 && n <= 1000000 && n == (unsigned)n)
            o->profile.capacity = (uint32_t)n;
        else if (!strcmp(key, "--port") && n >= 1 && n <= 65535 && n == (unsigned)n)
            o->sender.port = (uint16_t)n;
        else if (!strcmp(key, "--synthetic-mbps") && n > 0 && n <= 1000) o->synthetic_mbps = n;
        else if (!strcmp(key, "--pace-mbps") && (n == 0 || n >= 0.01) && n <= 100000)
            o->sender.pace_mbps = n;
        else if (!strcmp(key, "--drop-rate") && n >= 0 && n <= 1) o->sender.drop_rate = n;
        else if (!strcmp(key, "--fec-percent") && n >= 0 && n <= 100 && n == (unsigned)n)
            o->sender.fec_percent = (unsigned)n;
        else if (!strcmp(key, "--fec-min") && n >= 0 && n <= 254 && n == (unsigned)n)
            o->sender.fec_min = (unsigned)n;
        else if (!strcmp(key, "--seconds") && n > 0 && n <= 86400) o->seconds = n;
        else if (!strcmp(key, "--frames") && n >= 1 && n <= 10000000 && n == (uint64_t)n)
            o->frames = (uint64_t)n;
        else if (!strcmp(key, "--repeat") && n >= 1 && n <= 10000 && n == (unsigned)n)
            o->repeat = (unsigned)n;
        else return false;
    }
    return (!fec_min || (fec_percent && o->sender.fec_percent)) && !(o->profile.listen && o->profile.connect) &&
           (!o->profile.capacity || lls_profile_enabled(&o->profile)) &&
           ((o->input != NULL) != (o->synthetic_mbps > 0));
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return NULL; }
    long length;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 ||
        length > 512L * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "Input must be a nonempty file of at most 512 MiB\n");
        fclose(file);
        return NULL;
    }
    uint8_t *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "Cannot read complete input\n");
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    Options o;
    if (!parse(argc, argv, &o)) {
        fprintf(stderr, "Usage: %s HOST [INPUT.hevc | --synthetic-mbps N]\n"
                "  [--port 47800] [--codec hevc|h264|av1] [--fps 60] [--frames N] [--seconds N] [--repeat N]\n"
                "  [--pace-mbps N] [--qos] [--drop-rate 0.01]\n"
                "  [--profile [ADDR:]PORT | --profile-to HOST:PORT] [--profile-capacity N]\n"
                "  [--fec-percent 0..100] [--fec-min 1] (parity adds to replay/synthetic bitrate)\n"
                "Replay defaults to one file's frame count; synthetic defaults to 10 s.\n"
                "Recovery jumps to the next IDR; frame numbers remain consecutive.\n", argv[0]);
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);
    size_t bytes = 0, unit_count = 0, source = 0, synthetic_size = 0;
    uint8_t *data = NULL;
    HevcAccessUnit *units = NULL;
    LlsSender *sender = NULL;
    LlsProfile *profile = NULL;
    int status = 1;
    if (o.input) {
        data = read_file(o.input, &bytes);
        if (!data || !lls_split_access_units(o.sender.codec, data, bytes, &units, &unit_count)) goto done;
        bool have_key = false;
        for (size_t i = 0; i < unit_count; ++i) if (units[i].key) have_key = true;
        if (!have_key) { fprintf(stderr, "Replay needs an IDR carrying its parameter sets\n"); goto done; }
        if (!o.frames && !o.seconds) o.frames = unit_count * o.repeat;
    } else {
        double size = ceil(o.synthetic_mbps * 1000000 / (8 * o.fps));
        if (size < 1 || size > 1024 * 1024) {
            fprintf(stderr, "Synthetic frames including a 4x key burst must fit 4 MiB\n");
            goto done;
        }
        synthetic_size = (size_t)size;
        data = malloc(synthetic_size * 4);
        if (!data) goto done;
        uint32_t random = 0x12345678;
        for (size_t i = 0; i < synthetic_size * 4; ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            data[i] = (uint8_t)random;
        }
        if (!o.frames && !o.seconds) o.seconds = 10;
    }
#ifdef _WIN32
    const char *clock = "QPC_us";
#elif defined(__APPLE__)
    const char *clock = "UPTIME_RAW_us";
#else
    const char *clock = "MONOTONIC_us";
#endif
    profile = lls_sender_profile_open(&o.profile, "llstream_send", clock);
    if (lls_profile_enabled(&o.profile) && !profile) goto done;
    o.sender.profile = profile;
    sender = lls_sender_open(&o.sender);
    if (!sender) goto done;
    uint64_t started = lls_net_now_us(), sent = 0, jumps = 0;
    status = 0;
    while (!interrupted && (!o.frames || sent < o.frames)) {
        uint64_t scheduled = started + (uint64_t)(sent * (1000000.0 / o.fps));
        if (o.seconds && scheduled - started >= o.seconds * 1000000) break;
        if (!lls_net_sleep_until_us(scheduled)) { status = 1; break; }
        if (interrupted || (o.seconds && lls_net_now_us() - started >= o.seconds * 1000000)) break;
        bool need_key = lls_sender_need_keyframe(sender);
        LlsSenderFrame frame = {.capture_us = scheduled, .encoded_us = lls_net_now_us()};
        if (o.input) {
            if (need_key && !units[source].key) {
                do { source = (source + 1) % unit_count; } while (!units[source].key);
                ++jumps;
            }
            frame.data = data + units[source].offset;
            frame.size = units[source].size;
            frame.key = units[source].key;
            source = (source + 1) % unit_count;
        } else {
            frame.key = need_key;
            frame.data = data;
            frame.size = synthetic_size * (frame.key ? 4 : 1);
        }
        uint32_t number;
        if (!lls_sender_send(sender, &frame, &number)) { status = 1; break; }
        ++sent;
    }
    printf("Replay/probe sent=%" PRIu64 " recovery_jumps=%" PRIu64 "\n", sent, jumps);
    /* Let the last frame report arrive; the feedback thread continues servicing PING. */
    if (!lls_net_sleep_until_us(lls_net_now_us() + 200000)) status = 1;
done:
    if (!lls_sender_close(sender)) status = 1;
    lls_profile_close(profile);
    free(units);
    free(data);
    return interrupted ? 128 + interrupted : status;
}
