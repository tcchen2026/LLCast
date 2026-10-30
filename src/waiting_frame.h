#ifndef LLSTREAM_WAITING_FRAME_H
#define LLSTREAM_WAITING_FRAME_H

#include "display_frame.h"
#include <stdbool.h>

/* CPU I420 startup card, packed/rotated by the same worker as dav1d output. */
DisplayFrame *lls_waiting_frame(unsigned port, bool back_key, double refresh_hz);

#endif
