/* The receiver test supplies drmGetCap; it never opens a real DRM device. */
#ifndef LLS_TEST_XF86DRM_H
#define LLS_TEST_XF86DRM_H
#include <stdint.h>
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
#endif
