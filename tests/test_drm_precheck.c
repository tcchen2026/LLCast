/* Read-only preflight against KMS fixtures. Any mutating ioctl would fail to link. */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fake_open(const char *path, int flags, ...);
#define open fake_open
#include "../src/drm_precheck.c"
#undef open
#include "check.h"

static bool card_ok;
static uint32_t connector_id = 333, encoder_id = 84, plane_id = 57;
static uint32_t crtc_id = 85, format = DRM_FORMAT_NV12;
static drmModeModeInfo mode;
static drmModeRes resources;
static drmModeConnector connector;
static drmModeEncoder encoder;
static drmModeCrtc crtc;
static drmModePlane plane;
static drmModePlaneRes planes;
static uint32_t property_ids[] = {1, 2};
static uint64_t property_values[] = {DRM_PLANE_TYPE_PRIMARY, 0};
static uint64_t range[] = {0, 12};
static drmModeObjectProperties properties;
static drmModePropertyRes type_prop, color_prop;

static int fake_open(const char *path, int flags, ...)
{
    CHECK(flags == (O_RDWR | O_CLOEXEC));
    return card_ok && !strcmp(path, "/dev/dri/card0") ? open("/dev/null", O_RDONLY) : -1;
}
int drmSetClientCap(int fd, uint64_t cap, uint64_t value)
{ CHECK(fd >= 0 && cap == DRM_CLIENT_CAP_ATOMIC && value == 1); return 0; }
int drmGetCap(int fd, uint64_t cap, uint64_t *value)
{ CHECK(fd >= 0 && cap == DRM_CAP_TIMESTAMP_MONOTONIC); *value = 1; return 0; }
drmModeResPtr drmModeGetResources(int fd) { (void)fd; return &resources; }
void drmModeFreeResources(drmModeResPtr p) { CHECK(p == &resources); }
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t id)
{ (void)fd; CHECK(id == connector_id); return &connector; }
void drmModeFreeConnector(drmModeConnectorPtr p) { CHECK(p == &connector); }
drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t id)
{ (void)fd; CHECK(id == encoder_id); return &encoder; }
void drmModeFreeEncoder(drmModeEncoderPtr p) { CHECK(p == &encoder); }
drmModePlaneResPtr drmModeGetPlaneResources(int fd) { (void)fd; return &planes; }
void drmModeFreePlaneResources(drmModePlaneResPtr p) { CHECK(p == &planes); }
drmModePlanePtr drmModeGetPlane(int fd, uint32_t id)
{ (void)fd; CHECK(id == plane_id); return &plane; }
void drmModeFreePlane(drmModePlanePtr p) { CHECK(p == &plane); }
drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t id, uint32_t type)
{ (void)fd; CHECK(id == plane_id && type == DRM_MODE_OBJECT_PLANE); return &properties; }
void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr p) { CHECK(p == &properties); }
drmModePropertyPtr drmModeGetProperty(int fd, uint32_t id)
{ (void)fd; return id == 1 ? &type_prop : &color_prop; }
void drmModeFreeProperty(drmModePropertyPtr p) { CHECK(p == &type_prop || p == &color_prop); }
drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t id) { (void)fd; CHECK(id == crtc_id); return &crtc; }
void drmModeFreeCrtc(drmModeCrtcPtr p) { CHECK(!p || p == &crtc); }

static void defaults(void)
{
    card_ok = true;
    mode = (drmModeModeInfo){.clock = 148500, .htotal = 2200, .vtotal = 1125,
                            .hdisplay = 1920, .vdisplay = 1080};
    resources = (drmModeRes){.count_connectors = 1, .connectors = &connector_id,
                             .count_crtcs = 1, .crtcs = &crtc_id};
    connector = (drmModeConnector){.connector_id = connector_id, .connection = DRM_MODE_CONNECTED,
        .connector_type = DRM_MODE_CONNECTOR_HDMIA, .count_modes = 1, .modes = &mode,
        .count_encoders = 1, .encoders = &encoder_id};
    encoder = (drmModeEncoder){.possible_crtcs = 1, .crtc_id = crtc_id};
    crtc = (drmModeCrtc){.crtc_id = crtc_id, .mode_valid = 1, .mode = mode}; /* Android's current mode */
    format = DRM_FORMAT_NV12;
    plane = (drmModePlane){.plane_id = plane_id, .possible_crtcs = 1, .count_formats = 1, .formats = &format};
    planes = (drmModePlaneRes){.count_planes = 1, .planes = &plane_id};
    properties = (drmModeObjectProperties){.count_props = 2, .props = property_ids, .prop_values = property_values};
    type_prop = (drmModePropertyRes){.prop_id = 1};
    strcpy(type_prop.name, "type");
    color_prop = (drmModePropertyRes){.prop_id = 2, .flags = DRM_MODE_PROP_RANGE, .count_values = 2, .values = range};
    strcpy(color_prop.name, "COLOR_SPACE");
    property_values[0] = DRM_PLANE_TYPE_PRIMARY;
    range[0] = 0; range[1] = 12;
}

static void check_result(int first_failure, const char *mode_detail)
{
    FILE *file = tmpfile(); CHECK(file);
    char card[64] = "unchanged", line[256];
    CHECK(lls_drm_check(file, card, sizeof(card)) == (first_failure < 4));
    CHECK(!strcmp(card, first_failure == 4 ? "/dev/dri/card0" : "unchanged"));
    rewind(file);
    const char *keys[] = {"card", "display", "mode", "plane"};
    for (int i = 0; i < 4; ++i) {
        CHECK(fgets(line, sizeof(line), file));
        char expected[128];
        snprintf(expected, sizeof(expected), "CHECK\t%s\t%s\t", keys[i], i < first_failure ? "PASS" : "FAIL");
        CHECK(!strncmp(line, expected, strlen(expected)));
        if (i == 2 && mode_detail) CHECK(!strcmp(line + strlen(expected), mode_detail));
    }
    CHECK(!fgets(line, sizeof(line), file));
    fclose(file);
}

int main(void)
{
    defaults(); check_result(4, "1920x1080@60.00\n");
    defaults(); card_ok = false; check_result(0, NULL);
    defaults(); connector.connection = DRM_MODE_DISCONNECTED; check_result(1, NULL);
    defaults(); connector.connector_type = DRM_MODE_CONNECTOR_eDP; check_result(4, NULL);
    defaults(); connector.connector_type = DRM_MODE_CONNECTOR_DSI; format = DRM_FORMAT_YUYV;
    properties.count_props = 1; check_result(4, NULL);
    defaults(); connector.connector_type = DRM_MODE_CONNECTOR_WRITEBACK; check_result(1, NULL);
    /* Any mode Android uses is kept (user decision); the app warns when it isn't 60 Hz. */
    defaults(); crtc.mode.hdisplay = 3840; crtc.mode.vdisplay = 2160; crtc.mode.clock = 594000;
    crtc.mode.htotal = 5280; crtc.mode.vtotal = 2250; check_result(4, "3840x2160@50.00\n");
    defaults(); crtc.mode_valid = 0; mode.type = DRM_MODE_TYPE_PREFERRED; mode.clock = 185625;
    check_result(4, "1920x1080@75.00 (CRTC off; will be set)\n");
    defaults(); crtc.mode_valid = 0; mode.flags = DRM_MODE_FLAG_INTERLACE; check_result(2, "none\n");
    defaults(); plane.possible_crtcs = 2; check_result(3, NULL);
    defaults(); format = DRM_FORMAT_XRGB8888; check_result(3, NULL);
    defaults(); property_values[0] = DRM_PLANE_TYPE_CURSOR; check_result(3, NULL);
    defaults(); strcpy(color_prop.name, "other"); check_result(4, NULL);
    defaults(); range[1] = 2; check_result(4, NULL);
    puts("DRM prechecks passed");
    return 0;
}
