/* Real mode/import/scheduling logic, fake DRM metadata and ioctls. No DRM device. */
#include "../src/drm_display.c"
#include "../src/scanout_buffer.c"
#include <stdarg.h>
#include "check.h"

typedef DisplayFrame Picture;
static DrmDisplay *display;
static uint32_t next_fb = 1, live[1024], handles[1024];
static unsigned imports, removes, closed;
static uint64_t controller_active;

drmModeCrtc *drmModeGetCrtc(int fd, uint32_t id)
{
    (void)fd; CHECK(id == 68);
    drmModeCrtc *crtc = calloc(1, sizeof(*crtc)); CHECK(crtc);
    crtc->mode_valid = 1;
    crtc->mode.hdisplay = 1200; crtc->mode.vdisplay = 1920;
    crtc->mode.clock = 250000;
    return crtc;
}
void drmModeFreeCrtc(drmModeCrtc *crtc) { free(crtc); }
drmModeObjectProperties *drmModeObjectGetProperties(int fd, uint32_t id, uint32_t type)
{
    (void)fd; CHECK(id == 68 && type == DRM_MODE_OBJECT_CRTC);
    drmModeObjectProperties *props = calloc(1, sizeof(*props)); CHECK(props);
    props->props = calloc(1, sizeof(*props->props)); CHECK(props->props);
    props->prop_values = calloc(1, sizeof(*props->prop_values)); CHECK(props->prop_values);
    props->count_props = 1; props->props[0] = 99;
    props->prop_values[0] = controller_active;
    return props;
}
void drmModeFreeObjectProperties(drmModeObjectProperties *props)
{ if (props) { free(props->props); free(props->prop_values); free(props); } }
drmModePropertyRes *drmModeGetProperty(int fd, uint32_t id)
{
    (void)fd; CHECK(id == 99);
    drmModePropertyRes *prop = calloc(1, sizeof(*prop)); CHECK(prop);
    strcpy(prop->name, "ACTIVE"); prop->prop_id = id;
    return prop;
}
void drmModeFreeProperty(drmModePropertyRes *prop) { free(prop); }

static void mode_state(void)
{
    drmModeModeInfo preferred = {.hdisplay=1920,.vdisplay=1080,.type=DRM_MODE_TYPE_PREFERRED};
    drmModeConnector connector = {.count_modes=1,.modes=&preferred};
    for (unsigned active=0; active<2; ++active) {
        controller_active = active;
        DrmDisplay d = {.crtc=68};
        CHECK(choose_mode(&d,&connector));
        CHECK(d.mode.hdisplay==1200 && d.mode.vdisplay==1920 && d.mode.clock==250000);
        CHECK(d.keep_mode==(bool)active); // inactive CRTC keeps its mode but needs activation
    }
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{ (void)fd; *handle = (uint32_t)prime_fd; ++imports; return 0; }
int drmModeAddFB2(int fd, uint32_t w, uint32_t h, uint32_t format,
                  const uint32_t hnd[4], const uint32_t pitch[4], const uint32_t off[4],
                  uint32_t *id, uint32_t flags)
{
    (void)fd; (void)w; (void)h; (void)pitch; (void)off; (void)flags;
    CHECK(format == DRM_FORMAT_NV12 && next_fb < 1024);
    *id = next_fb++;
    live[*id] = handles[*id] = hnd[0];
    return 0;
}
int drmModeRmFB(int fd, uint32_t id)
{
    (void)fd;
    CHECK(id && id < next_fb && live[id]);
    CHECK(id != display->scanning.framebuffer && id != display->flight.framebuffer);
    live[id] = 0; ++removes;
    return 0;
}
int ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    CHECK(request == DRM_IOCTL_GEM_CLOSE);
    va_list args; va_start(args, request);
    uint32_t handle = va_arg(args, struct drm_gem_close *)->handle;
    va_end(args);
    for (uint32_t i = 1; i < next_fb; ++i) CHECK(live[i] != handle);
    ++closed;
    return 0;
}

#include "scanout_pixels_checks.h"

static unsigned released[8], fates[8];
static void release_picture(void *owner) { ++released[(uintptr_t)owner]; }
static Slot slot(unsigned index)
{
    DisplayFrame *frame = calloc(1, sizeof(*frame)); CHECK(frame);
    frame->owner = (void *)(uintptr_t)index; frame->release = release_picture;
    return (Slot){.frame=frame,.index=index};
}
static void frame_fate(void *context, int64_t index, DrmFrameFate fate, int64_t flip, uint32_t sequence)
{
    (void)context; (void)flip; (void)sequence;
    CHECK(index > 0 && index < 8 && !fates[index]);
    fates[index] = (unsigned)fate+1;
}
static void scheduling(void)
{
    DrmDisplay d={.options={.on_frame=frame_fate}};
    CHECK(!pthread_mutex_init(&d.lock,NULL));
    d.scanning=slot(1); d.flight=slot(2); d.ready=slot(3);
    CHECK(next_work(&d)==DISPLAY_PREPARE && d.prepared.index==3);
    CHECK(!released[1] && !released[2]); // prepare while a flip is pending
    d.ready=slot(4);
    CHECK(next_work(&d)==DISPLAY_PREPARE && d.prepared.index==4);
    CHECK(released[3]==1 && fates[3]==DRM_FRAME_REPLACED+1);
    CHECK(next_work(&d)==DISPLAY_WAIT); // prepared, but flight 2 still owns its buffer
    on_flip(0,10,1,0,&d);
    CHECK(released[1]==1 && !released[2] && d.scanning.index==2);
    d.ready=slot(5);
    CHECK(next_work(&d)==DISPLAY_COMMIT && d.prepared.index==4 && d.ready.index==5);
    // Complete the commit; a new raw frame must not delay the prepared one.
    d.flight=d.prepared; d.prepared=(Slot){0};
    CHECK(next_work(&d)==DISPLAY_PREPARE && d.prepared.index==5);
    on_flip(0,11,1,11111,&d);
    CHECK(released[2]==1 && d.scanning.index==4);
    // EBUSY retains prepared ownership, then permits replacement by a newer picture.
    d.retry=true; CHECK(next_work(&d)==DISPLAY_COMMIT && d.prepared.index==5);
    d.ready=slot(6);
    CHECK(next_work(&d)==DISPLAY_PREPARE && d.prepared.index==6);
    CHECK(released[5]==1 && fates[5]==DRM_FRAME_REPLACED+1);
    d.retry=false; CHECK(next_work(&d)==DISPLAY_COMMIT);
    d.flight=d.prepared; d.prepared=(Slot){0};
    on_flip(0,12,1,22222,&d);
    d.stopping=true; CHECK(next_work(&d)==DISPLAY_STOP);
    display_frame_free(&d.scanning.frame);
    for(unsigned i=1;i<=6;++i) CHECK(released[i]==1);
    CHECK(fates[2]==DRM_FRAME_SHOWN+1 && fates[4]==DRM_FRAME_SHOWN+1 && fates[6]==DRM_FRAME_SHOWN+1);
    pthread_mutex_destroy(&d.lock);
}

int main(void)
{
    scanout_pixel_checks(); scheduling(); mode_state();
    DrmDisplay d = {0}; display = &d;
    FILE *files[300]; Picture frames[300];
    for (unsigned i = 0; i < 300; ++i) {
        files[i] = tmpfile(); CHECK(files[i]);
        frames[i] = (Picture){.fd = fileno(files[i]), .width = 1920, .height = 1080,
            .stride = 1920, .rows = 1088, .bytes = 1920*1088*3/2, .format = DRM_FORMAT_NV12};
        Framebuffer *fb = import_frame(&d, &frames[i]);
        CHECK(fb && d.buffer_count <= MAX_FRAMEBUFFERS);
        if (!i) d.scanning.framebuffer = fb->fb;
        if (i == 1) d.flight.framebuffer = fb->fb;
        CHECK(import_frame(&d, &frames[i]) == fb); // cache hit, no new import
    }
    CHECK(imports == 300 && removes == 300 - MAX_FRAMEBUFFERS);
    CHECK(live[d.scanning.framebuffer] && live[d.flight.framebuffer]);
    /* A new layout can share a GEM handle with an old, still-scanning FB. */
    frames[0].width = 1280; frames[0].height = 720;
    Framebuffer *alias = import_frame(&d, &frames[0]);
    CHECK(alias && alias->handle == handles[d.scanning.framebuffer]);
    CHECK(release_import(&d, (size_t)(alias - d.buffers)));
    CHECK(live[d.scanning.framebuffer]);
    d.scanning.framebuffer = d.flight.framebuffer = 0;
    for (size_t i = 0; i < d.buffer_count; ++i) CHECK(release_import(&d, i));
    CHECK(removes == imports && closed == 300);
    // A full cache of stale imports must not evict the new eight-buffer
    // working set repeatedly. Simulate scanout advancing, with no flight FB.
    d = (DrmDisplay){0};
    for (unsigned i = 0; i < MAX_FRAMEBUFFERS; ++i) {
        frames[i].width = 1920; frames[i].height = 1080;
        Framebuffer *fb = import_frame(&d, &frames[i]); CHECK(fb);
        d.scanning.framebuffer = fb->fb;
    }
    const unsigned before = imports;
    for (unsigned i = 0; i < 600; ++i) {
        Framebuffer *fb = import_frame(&d, &frames[MAX_FRAMEBUFFERS + i % 8]); CHECK(fb);
        d.scanning.framebuffer = fb->fb;
    }
    CHECK(imports == before + 8);
    frames[136].width = frames[137].width = 1280;
    frames[136].height = frames[137].height = 720;
    Framebuffer *changed = import_frame(&d, &frames[136]); CHECK(changed);
    unsigned kept = 0;
    for (size_t i = 0; i < d.buffer_count; ++i) {
        const Framebuffer *fb = &d.buffers[i];
        if (fb->fb) {
            ++kept;
            CHECK(fb->width == 1280 || fb->fb == d.scanning.framebuffer);
        }
    }
    CHECK(kept == 2); // the new geometry and the protected old scanout
    d.scanning.framebuffer = changed->fb;
    CHECK(import_frame(&d, &frames[137]));
    for (size_t i = 0; i < d.buffer_count; ++i)
        CHECK(!d.buffers[i].fb || d.buffers[i].width == 1280);
    d.scanning.framebuffer = 0;
    for (size_t i = 0; i < d.buffer_count; ++i) CHECK(release_import(&d, i));
    CHECK(removes == imports);
    for (unsigned i = 0; i < 300; ++i) fclose(files[i]);
    puts("DRM cache: LRU retains 8 buffers across 600 frames; geometry flush and protected/shared handles verified");
}
