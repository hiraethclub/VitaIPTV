/*
 * VitaIPTV - portable logging (core/, C99).
 *
 * Design goals:
 *   - One printf-style API usable from core and from the Vita app.
 *   - A built-in ring buffer of the most recent lines, so the UI can draw an
 *     on-screen log without any I/O (invaluable when there is no debugger).
 *   - An optional extra sink (the Vita app routes this to the network so logs
 *     can be read on the PC while testing).
 *   - Optional lock hooks so it is safe to call from worker threads on device,
 *     while staying dependency-free and testable natively.
 *
 * Nothing here touches Vita headers.
 */
#ifndef VI_LOG_H
#define VI_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VI_LOG_ERROR = 0,
    VI_LOG_WARN  = 1,
    VI_LOG_INFO  = 2,
    VI_LOG_DEBUG = 3,
    VI_LOG_TRACE = 4
} vi_log_level;

/* Extra sink, called for each message at or below the max level. `line` is the
 * fully formatted "TAG: text" string (no trailing newline). */
typedef void (*vi_log_sink)(void *ctx, vi_log_level level, const char *line);

/* Reset the ring buffer and set the max level. Call once at startup. */
void vi_log_init(vi_log_level max_level);

/* Change the max level at runtime. */
void vi_log_set_level(vi_log_level max_level);
vi_log_level vi_log_get_level(void);

/* Install an optional extra sink (e.g. network). Pass NULL to remove. */
void vi_log_set_sink(vi_log_sink sink, void *ctx);

/* Install optional lock/unlock hooks for thread safety (e.g. a Vita mutex).
 * If unset, no locking is done (fine for single-threaded native tests). */
void vi_log_set_lock(void (*lock)(void *), void (*unlock)(void *), void *ctx);

/* Core log call (printf-style). Prefer the VI_LOG* macros below. */
void vi_log_write(vi_log_level level, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* Ring-buffer access for on-screen display. Index 0 is the OLDEST retained
 * line. Copies the line into `out` under the lock (safe against concurrent
 * writers). Returns 1 if a line was copied, 0 if index is past the end. */
size_t vi_log_ring_count(void);
int vi_log_ring_copy(size_t index, char *out, size_t out_size,
                     vi_log_level *level_out);

/* Total lines ever written (for a "N dropped" indicator if desired). */
unsigned long vi_log_total(void);

#define VI_LOGE(tag, ...) vi_log_write(VI_LOG_ERROR, (tag), __VA_ARGS__)
#define VI_LOGW(tag, ...) vi_log_write(VI_LOG_WARN,  (tag), __VA_ARGS__)
#define VI_LOGI(tag, ...) vi_log_write(VI_LOG_INFO,  (tag), __VA_ARGS__)
#define VI_LOGD(tag, ...) vi_log_write(VI_LOG_DEBUG, (tag), __VA_ARGS__)
#define VI_LOGT(tag, ...) vi_log_write(VI_LOG_TRACE, (tag), __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* VI_LOG_H */
