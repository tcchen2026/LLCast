#ifndef LLCAST_DRM_PRECHECK_H
#define LLCAST_DRM_PRECHECK_H
#include <stddef.h>
#include <stdio.h>
/* Read-only KMS queries; never acquire master, modeset or change services.
 * CHECK<TAB>{card,hdmi,mode,plane}<TAB>{PASS,FAIL}<TAB>detail (mode: the one that will be shown,
 * e.g. 1920x1080@60.00; any mode passes)
 * On success, card receives the usable /dev/dri/cardN path. */
int lls_drm_check(FILE *output, char *card, size_t capacity);
#endif
