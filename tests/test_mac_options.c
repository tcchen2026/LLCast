#include "check.h"
#include "../mac/LLCOptions.h"

#define READ(...) LLCParseOptions((int)(sizeof((const char *[]){"LLCast", __VA_ARGS__}) / sizeof(char *)), \
    (const char *[]){"LLCast", __VA_ARGS__}, &o)

int main(void)
{
    LLCOptions o;
    const char *plain[] = {"LLCast"};
    CHECK(!LLCParseOptions(1, plain, &o));
    CHECK(!o.host && o.scale && !o.hd720 && !o.codec && !o.profile.listen && !o.profile.connect);
    CHECK(!READ("--send", "box", "--codec", "h264", "--mode", "720p"));
    CHECK(o.codec == LLS_CODEC_H264 && o.hd720);
    CHECK(!READ("--codec", "av1", "--send", "box") && o.codec == LLS_CODEC_AV1);
    CHECK(!READ("--send", "box", "--codec", "hevc") && o.codec == LLS_CODEC_HEVC);
    CHECK(!READ("--send", "box", "--codec", "auto") && o.codec == 0);
    CHECK(READ("--send", "box", "--codec", "vp9"));
    CHECK(READ("--send", "box", "--codec"));
    CHECK(READ("--codec", "auto"));
    CHECK(!READ("--profile", "47801"));
    CHECK(!o.host && !strcmp(o.profile.listen, "47801"));
    CHECK(!READ("--send", "box", "--profile-to", "vm:47801", "--mode", "720p", "--no-scale"));
    CHECK(o.hd720 && !o.scale && !strcmp(o.host, "box") && !strcmp(o.profile.connect, "vm:47801"));
    CHECK(!READ("--profile-capacity", "10", "--profile", "[::1]:47801", "--send", "box", "--port", "1234"));
    CHECK(o.profile.capacity == 10 && !strcmp(o.port, "1234"));
    CHECK(!READ("-psn_1", "-NSDocumentRevisionsDebugMode", "YES", "--profile", "47801"));
    CHECK(!READ("--selftest"));
    CHECK(!READ("--help"));
    CHECK(READ("--profile"));
    CHECK(READ("--profile", ""));
    CHECK(READ("--profile", "--profile-to", "vm:47801"));
    CHECK(READ("--profile", "47801", "--profile-to", "vm:47801"));
    CHECK(READ("--profile", "47801", "--profile", "47802"));
    CHECK(READ("--profile-capacity", "10"));
    CHECK(READ("--profile", "47801", "--profile-capacity", "1000001"));
    CHECK(READ("--profile", "47801", "--profile-capacity", "-1"));
    CHECK(READ("--profile", "47801", "--profile-capacity", "0"));
    CHECK(READ("--selftest", "--profile", "47801"));
    CHECK(READ("--no-scale"));
    CHECK(READ("--send", "box", "--mode", "bad"));
    CHECK(READ("--garbage"));
    puts("Mac app/CLI profile options: OK");
}
