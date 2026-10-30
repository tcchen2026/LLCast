#ifndef LLCAST_AV1_H
#define LLCAST_AV1_H

#include <android/native_window.h>
#include "lls_rx_session.h"

typedef struct LlcAv1 LlcAv1;
/* Input-thread API; the renderer owns its EGL context on a separate thread.
 * presented runs there after an EGL submission, not a physical scanout event. */
LlcAv1 *llc_av1_open(LlsRxSession *rx, ANativeWindow *window, void *context,
                    void (*presented)(void *context, int width, int height));
/* Takes ownership of frame, including on failure. */
bool llc_av1_queue(LlcAv1 *decoder, LlsRxFrame *frame);
void llc_av1_close(LlcAv1 *decoder);

#endif
