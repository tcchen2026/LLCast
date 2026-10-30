#include "waiting_frame.h"
#include "waiting_strings.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <drm_fourcc.h>
#include <stdio.h>
#include <string.h>


enum { WIDTH = 1920, HEIGHT = 1080, STRIDE = 1920, ROWS = 1088 };

/* Small original block alphabet. Each byte is a five-pixel row. */
static const unsigned char letters[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {31,4,4,4,4,4,31}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
};
static const unsigned char digits[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
};

static void label(unsigned char *y, const char *text, unsigned top,
                  unsigned scale, unsigned char level)
{
    size_t length = strlen(text);
    if (!length || length > WIDTH / (6 * scale) || top + 7 * scale > HEIGHT) return;
    unsigned left = (WIDTH - ((unsigned)length * 6 - 1) * scale) / 2;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        const unsigned char *glyph = c >= 'A' && c <= 'Z' ? letters[c - 'A'] :
                                      c >= '0' && c <= '9' ? digits[c - '0'] : NULL;
        for (unsigned row = 0; row < 7; ++row) {
            unsigned bits = glyph ? glyph[row] : c == '.' && row == 6 ? 4 : 0;
            for (unsigned col = 0; col < 5; ++col) {
                if (!(bits & (16u >> col))) continue;
                for (unsigned dy = 0; dy < scale; ++dy)
                    memset(y + (top + row * scale + dy) * STRIDE + left + col * scale,
                           level, scale);
            }
        }
        left += 6 * scale;
    }
}

static void address(char *text, size_t size, unsigned port)
{
    char ip[INET_ADDRSTRLEN] = "";
    struct ifaddrs *addresses = NULL;
    if (!getifaddrs(&addresses)) {
        for (struct ifaddrs *a = addresses; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
            const struct sockaddr_in *in = (const struct sockaddr_in *)a->ifa_addr;
            if ((ntohl(in->sin_addr.s_addr) >> 24) == 127 || !in->sin_addr.s_addr) continue;
            if (!ip[0] || !strcmp(a->ifa_name, "wlan0"))
                inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
            if (!strcmp(a->ifa_name, "wlan0")) break;
        }
        freeifaddrs(addresses);
    }
    snprintf(text, size, "%s%sUDP %u", ip, ip[0] ? "   " : "", port);
}

DisplayFrame *lls_waiting_frame(unsigned port, bool back_key, double refresh_hz)
{
    const size_t size = (size_t)STRIDE * ROWS * 3 / 2;
    unsigned char *pixels = malloc(size);
    DisplayFrame *frame = calloc(1, sizeof(*frame));
    if (!pixels || !frame) { free(pixels); free(frame); return NULL; }
    memset(pixels, 28, (size_t)STRIDE * ROWS);
    memset(pixels + (size_t)STRIDE * ROWS, 128, size - (size_t)STRIDE * ROWS);
    label(pixels, LLCAST_WAITING_TITLE, 330, 12, 235);
    label(pixels, LLCAST_WAITING_STATUS, 490, 8, 220);
    char destination[64];
    address(destination, sizeof(destination), port);
    label(pixels, destination, 640, 5, 160);
    label(pixels, LLCAST_WAITING_START, 750, 4, 140);
    if (back_key) label(pixels, LLCAST_WAITING_STOP, 840, 4, 180);
    if (refresh_hz > 0 && (refresh_hz < 59.5 || refresh_hz > 60.5)) {
        char refresh[96];
        snprintf(refresh, sizeof(refresh), LLCAST_WAITING_REFRESH, (int)(refresh_hz + 0.5));
        label(pixels, refresh, 930, 4, 200);
    }
    *frame = (DisplayFrame){.fd = -1, .width = WIDTH, .height = HEIGHT,
        .format = DRM_FORMAT_YUV420, .owner = pixels, .release = free,
        .planes = {pixels, pixels + STRIDE * ROWS, pixels + STRIDE * ROWS * 5 / 4},
        .pitches = {STRIDE, STRIDE / 2, STRIDE / 2}};
    fprintf(stderr, "Waiting screen ready: %s\n", destination);
    return frame;
}
