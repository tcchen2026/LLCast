#ifndef LLSTREAM_PROBE_H
#define LLSTREAM_PROBE_H

/* Asks a receiver whether it is there: a PING every 200 ms for up to 3 seconds
 * (user decision), so a receiver waking from Wi-Fi power saving or still
 * starting has time to answer. Returns at the first answer. Receivers answer
 * PING whatever they are doing, and the probe's session never disturbs a
 * running stream. Portable over lls_net. */

#include <stdint.h>
#include "llstream_proto.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LLS_PROBE_ANSWERED,    /* *rtt_us is the receiver's reply time */
    LLS_PROBE_SILENT,      /* nothing answered */
    LLS_PROBE_REFUSED,     /* the host answered, but no receiver on that port */
    LLS_PROBE_UNREACHABLE, /* no route, or (macOS) Local Network access denied */
    LLS_PROBE_FAILED,      /* bad address or no socket; *error says why */
} LlsProbeResult;

LlsProbeResult lls_probe_receiver(const char *host, uint16_t port, uint32_t *rtt_us,
                                 uint32_t *codecs, int *error);
#ifdef __cplusplus
}
#endif

#endif
