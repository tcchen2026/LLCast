#ifndef LLS_PROFILE_H
#define LLS_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct LlsProfile LlsProfile;
typedef struct {
    const char *listen; /* [ADDR:]PORT, default loopback; NULL disables listening */
    const char *connect; /* HOST:PORT; retry outgoing connections */
    uint32_t capacity; /* 0 uses each table's default, otherwise rows per table */
} LlsProfileConfig;
typedef struct {
    const char *name, *columns; /* comma-separated column names, all int64 */
    uint32_t capacity;
} LlsProfileTable;
enum { LLS_PROFILE_TABLES = 8, LLS_PROFILE_COLUMNS = 64, LLS_PROFILE_CLIENTS = 4 };

/* No endpoint: returns NULL without allocating or initializing networking.
 * One endpoint at most. Table IDs are their positions in tables[]. All schema
 * strings are copied. Returns NULL on failure and reports the cause on stderr. */
LlsProfile *lls_profile_open(const LlsProfileConfig *config, const char *program,
    const char *clock, const LlsProfileTable *tables, unsigned count);
bool lls_profile_enabled(const LlsProfileConfig *config);
/* O(1), no allocation or blocking I/O. NULL is a no-op. Concurrent producers
 * are serialized only for the fixed-sized copy and ring cursor update. */
void lls_profile_record(LlsProfile *profile, unsigned table, const int64_t *values);
uint16_t lls_profile_port(const LlsProfile *profile); /* actual listen port; 0 otherwise */
/* Call after producers stop. Flushes ready clients for at most 250 ms; slow or
 * disconnected collectors never hold up stream teardown. */
void lls_profile_close(LlsProfile *profile);

#endif
