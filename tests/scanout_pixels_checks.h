/* Shared by the host cache test and a short ARM64 on-device pixel check. */
#include "check.h"
static void scanout_pixel_checks(void)
{
    const uint8_t y[] = {16, 32, 48, 64, 80, 96, 112, 128};
    const uint8_t u[] = {90, 110}, v[] = {140, 160};
    DisplayFrame s = {.fd = -1, .width = 4, .height = 2, .format = DRM_FORMAT_YUV420,
        .planes = {y,u,v}, .pitches = {4,2,2}};
    uint8_t out[16];
    CHECK(scanout_pixels(out, sizeof(out), 4, DRM_FORMAT_YUYV, 2, 4, &s));
    const uint8_t rotated[] = {80,90,16,140, 96,90,32,140, 112,110,48,160, 128,110,64,160};
    CHECK(!memcmp(out, rotated, sizeof(out))); // clockwise, never UV swapped
    CHECK(scanout_pixels(out, sizeof(out), 8, DRM_FORMAT_YUYV, 4, 2, &s));
    const uint8_t same[] = {16,90,32,140, 48,110,64,160, 80,90,96,140, 112,110,128,160};
    CHECK(!memcmp(out, same, sizeof(out))); // same orientation: unchanged
    uint8_t nv12[12];
    CHECK(scanout_pixels(nv12, sizeof(nv12), 2, DRM_FORMAT_NV12, 2, 4, &s));
    const uint8_t nv[] = {80,16,96,32,112,48,128,64,90,140,110,160};
    CHECK(!memcmp(nv12, nv, sizeof(nv)));
    const uint8_t interleaved[] = {90,140,110,160};
    DisplayFrame hardware = s;
    hardware.format = DRM_FORMAT_NV12; hardware.planes[1] = interleaved; hardware.pitches[1] = 4;
    CHECK(scanout_pixels(out, sizeof(out), 4, DRM_FORMAT_YUYV, 2, 4, &hardware));
    CHECK(!memcmp(out, rotated, sizeof(out)));
    CHECK(!scanout_pixels(out, 15, 4, DRM_FORMAT_YUYV, 2, 4, &s));
    uint8_t fit[80]; memset(fit, 0xa5, sizeof(fit));
    CHECK(scanout_pixels(fit+4, 64, 16, DRM_FORMAT_YUYV, 8, 4, &s));
    CHECK(fit[3] == 0xa5 && fit[68] == 0xa5); // scaled output cannot overrun
    s.width = 2; s.height = 4; s.pitches[0] = 2; s.pitches[1] = s.pitches[2] = 1;
    CHECK(scanout_pixels(out, sizeof(out), 8, DRM_FORMAT_YUYV, 4, 2, &s)); // portrait sender -> landscape device
    CHECK(out[0] == 112 && out[2] == 80 && out[4] == 48 && out[6] == 16);

    /* Differential checks: the NV12 input path uses the generic coordinate
     * mapper, while I420 takes the native NEON path on ARM64. Exercise full
     * blocks, even tails, padded source/destination strides and black bars. */
    const unsigned cases[][4] = {{16,8,8,16}, {18,10,10,18}, {24,16,16,24},
        {32,18,18,32}, {16,8,16,8}, {18,10,18,10}, {8,16,16,8},
        {16,8,12,16}, {16,8,20,10}, {18,10,24,12}};
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        unsigned sw=cases[i][0], sh=cases[i][1], dw=cases[i][2], dh=cases[i][3];
        unsigned ys=sw+7, cs=sw/2+3, uvs=sw+6, pitch=2*dw+8;
        uint8_t *yuv=malloc((size_t)ys*sh + (size_t)cs*sh), *uv=malloc((size_t)uvs*sh/2);
        size_t n=(size_t)pitch*dh;
        uint8_t *a=malloc(n+32), *b=malloc(n+32);
        CHECK(yuv && uv && a && b);
        uint8_t *u=yuv+ys*sh, *v=u+cs*sh/2;
        for(unsigned y=0;y<sh;++y) for(unsigned x=0;x<sw;++x) yuv[y*ys+x]=(uint8_t)(17*x+31*y+3);
        for(unsigned y=0;y<sh/2;++y) for(unsigned x=0;x<sw/2;++x) {
            uv[y*uvs+2*x]=u[y*cs+x]=(uint8_t)(19*x+11*y+23);
            uv[y*uvs+2*x+1]=v[y*cs+x]=(uint8_t)(7*x+29*y+91);
        }
        DisplayFrame planar={.width=sw,.height=sh,.format=DRM_FORMAT_YUV420,
            .planes={yuv,u,v},.pitches={ys,cs,cs}};
        DisplayFrame semi={.width=sw,.height=sh,.format=DRM_FORMAT_NV12,
            .planes={yuv,uv},.pitches={ys,uvs}};
        memset(a,0xa5,n+32); memset(b,0xa5,n+32);
        CHECK(scanout_pixels(a+16,n,pitch,DRM_FORMAT_YUYV,dw,dh,&planar));
        CHECK(scanout_pixels(b+16,n,pitch,DRM_FORMAT_YUYV,dw,dh,&semi));
        CHECK(!memcmp(a,b,n+32));
        for(unsigned g=0;g<16;++g) CHECK(a[g]==0xa5 && a[n+16+g]==0xa5);
        free(a);free(b);free(uv);free(yuv);
    }
}
