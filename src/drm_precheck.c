#define _POSIX_C_SOURCE 200809L
#include "drm_precheck.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static bool plane_property(int fd, uint32_t plane, const char *name, uint64_t *value)
{
    bool found = false;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!props) return false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && !strcmp(p->name, name)) {
            *value = props->prop_values[i];
            found = strcmp(name, "COLOR_SPACE") ||
                ((p->flags & DRM_MODE_PROP_RANGE) && p->count_values >= 2 &&
                 p->values[0] <= 3 && p->values[1] >= 3);
        }
        drmModeFreeProperty(p);
        if (found) break;
    }
    drmModeFreeObjectProperties(props);
    return found;
}

static bool compatible_plane(int fd, uint32_t crtc_mask)
{
    bool found = false;
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) return false;
    for (uint32_t i = 0; i < planes->count_planes && !found; ++i) {
        drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
        if (!p) continue;
        uint64_t type = UINT64_MAX;
        if ((p->possible_crtcs & crtc_mask) && plane_property(fd, p->plane_id, "type", &type) &&
            type == DRM_PLANE_TYPE_PRIMARY) {
            for (uint32_t j = 0; j < p->count_formats; ++j)
                if ((p->formats[j] == DRM_FORMAT_NV12 || p->formats[j] == DRM_FORMAT_YUYV)) found = true;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(planes);
    return found;
}

static double refresh_rate(const drmModeModeInfo *m)
{
    return m->htotal && m->vtotal ? 1000.0 * m->clock / m->htotal / m->vtotal : 0;
}

/* The mode drm_display_open will use: the CRTC's current one, else the preferred, else the
 * first progressive one. Any mode is fine (user decision); the app warns when it isn't 60 Hz. */
static bool describe_mode(int fd, const drmModeConnector *c, uint32_t crtc_id, char *text, size_t size)
{
    drmModeCrtc *crtc = crtc_id ? drmModeGetCrtc(fd, crtc_id) : NULL;
    bool found = crtc && crtc->mode_valid && crtc->mode.hdisplay;
    if (found) snprintf(text, size, "%ux%u@%.2f", crtc->mode.hdisplay, crtc->mode.vdisplay, refresh_rate(&crtc->mode));
    drmModeFreeCrtc(crtc);
    for (int pass = 0; pass < 2 && !found; ++pass)
        for (int j = 0; j < c->count_modes && !found; ++j) {
            const drmModeModeInfo *m = &c->modes[j];
            if ((m->flags & DRM_MODE_FLAG_INTERLACE) || (pass == 0 && !(m->type & DRM_MODE_TYPE_PREFERRED))) continue;
            snprintf(text, size, "%ux%u@%.2f (CRTC off; will be set)", m->hdisplay, m->vdisplay, refresh_rate(m));
            found = true;
        }
    return found;
}

int lls_drm_check(FILE *output, char *card, size_t capacity)
{
    bool best[4] = {false};
    char best_path[64] = "unavailable", best_mode[64] = "none";
    for (unsigned n = 0; n < 16; ++n) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%u", n);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        /* Per-fd query capability only. This does not take DRM master. */
        uint64_t monotonic = 0;
        if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) ||
            drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &monotonic) || !monotonic) { close(fd); continue; }
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) { close(fd); continue; }
        bool have[4] = {true, false, false, false};
        char mode_text[64] = "none";
        for (int i = 0; i < res->count_connectors && !have[2]; ++i) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (!c) continue;
            bool hdmi = c->connection == DRM_MODE_CONNECTED &&
                c->connector_type != DRM_MODE_CONNECTOR_WRITEBACK;
            if (hdmi) have[1] = true;
            uint32_t mask = 0, current = 0;
            for (int j = 0; hdmi && j < c->count_encoders; ++j) {
                drmModeEncoder *e = drmModeGetEncoder(fd, c->encoders[j]);
                if (e) {
                    mask |= e->possible_crtcs;
                    if (e->crtc_id) current = e->crtc_id;
                }
                drmModeFreeEncoder(e);
            }
            if (hdmi && describe_mode(fd, c, current, mode_text, sizeof(mode_text))) {
                have[2] = true;
                /* Match drm_display_open's first physical display mode/current-CRTC choice. */
                uint32_t selected = 0;
                for (int j = 0; j < res->count_crtcs && j < 32; ++j)
                    if (res->crtcs[j] == current) selected = 1u << j;
                if (!selected) selected = mask & (0u - mask);
                if (compatible_plane(fd, selected)) have[3] = true;
            }
            drmModeFreeConnector(c);
        }
        unsigned score = 0, old_score = 0;
        for (unsigned i = 0; i < 4; ++i) { score += have[i]; old_score += best[i]; }
        if (score > old_score) {
            memcpy(best, have, sizeof(best));
            snprintf(best_path, sizeof(best_path), "%s", path);
            snprintf(best_mode, sizeof(best_mode), "%s", mode_text);
        }
        drmModeFreeResources(res);
        close(fd);
        if (best[3]) break;
    }
    const char *names[] = {"card", "display", "mode", "plane"};
    for (unsigned i = 0; i < 4; ++i)
        fprintf(output, "CHECK\t%s\t%s\t%s\n", names[i], best[i] ? "PASS" : "FAIL", i == 2 ? best_mode : best_path);
    if (best[3] && card && capacity) snprintf(card, capacity, "%s", best_path);
    return best[3] ? 0 : 1;
}
