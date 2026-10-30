#ifndef LLCAST_MPP_DISPLAY_H
#define LLCAST_MPP_DISPLAY_H
#include "display_frame.h"
#include "mpp_frame.h"
/* Owns *frame on success. No pixel copy. */
DisplayFrame *mpp_display_frame(MppFrame *frame);
#endif
