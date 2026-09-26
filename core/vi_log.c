#include "vi_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define VI_LOG_RING_LINES 64
#define VI_LOG_LINE_MAX   192

typedef struct {
    char         text[VI_LOG_LINE_MAX];
    vi_log_level level;
} log_line;

static struct {
    vi_log_level max_level;
    vi_log_sink  sink;
    void        *sink_ctx;

    void (*lock)(void *);
    void (*unlock)(void *);
    void  *lock_ctx;

    log_line ring[VI_LOG_RING_LINES];
    size_t   head;   /* index of next write */
    size_t   count;  /* number of valid entries (<= RING_LINES) */
    unsigned long total;
} g;

static void do_lock(void)
{
    if (g.lock)
        g.lock(g.lock_ctx);
}
static void do_unlock(void)
{
    if (g.unlock)
        g.unlock(g.lock_ctx);
}

void vi_log_init(vi_log_level max_level)
{
    memset(&g, 0, sizeof(g));
    g.max_level = max_level;
}

void vi_log_set_level(vi_log_level max_level)
{
    g.max_level = max_level;
}

vi_log_level vi_log_get_level(void)
{
    return g.max_level;
}

void vi_log_set_sink(vi_log_sink sink, void *ctx)
{
    g.sink = sink;
    g.sink_ctx = ctx;
}

void vi_log_set_lock(void (*lock)(void *), void (*unlock)(void *), void *ctx)
{
    g.lock = lock;
    g.unlock = unlock;
    g.lock_ctx = ctx;
}

static const char *level_prefix(vi_log_level l)
{
    switch (l) {
    case VI_LOG_ERROR: return "E";
    case VI_LOG_WARN:  return "W";
    case VI_LOG_INFO:  return "I";
    case VI_LOG_DEBUG: return "D";
    case VI_LOG_TRACE: return "T";
    default:           return "?";
    }
}

void vi_log_write(vi_log_level level, const char *tag, const char *fmt, ...)
{
    char body[VI_LOG_LINE_MAX];
    char line[VI_LOG_LINE_MAX + 64]; /* room for prefix+tag; ring copy truncates */
    va_list ap;

    if (level > g.max_level)
        return;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    snprintf(line, sizeof(line), "%s/%s: %s",
             level_prefix(level), tag ? tag : "-", body);

    do_lock();
    {
        log_line *slot = &g.ring[g.head];
        strncpy(slot->text, line, VI_LOG_LINE_MAX - 1);
        slot->text[VI_LOG_LINE_MAX - 1] = '\0';
        slot->level = level;
        g.head = (g.head + 1) % VI_LOG_RING_LINES;
        if (g.count < VI_LOG_RING_LINES)
            g.count++;
        g.total++;
    }
    do_unlock();

    if (g.sink)
        g.sink(g.sink_ctx, level, line);
}

size_t vi_log_ring_count(void)
{
    size_t c;
    do_lock();
    c = g.count;
    do_unlock();
    return c;
}

int vi_log_ring_copy(size_t index, char *out, size_t out_size,
                     vi_log_level *level_out)
{
    int ok = 0;
    if (out == NULL || out_size == 0)
        return 0;
    do_lock();
    if (index < g.count) {
        /* oldest entry is head - count (mod ring) */
        size_t start = (g.head + VI_LOG_RING_LINES - g.count) % VI_LOG_RING_LINES;
        size_t pos = (start + index) % VI_LOG_RING_LINES;
        strncpy(out, g.ring[pos].text, out_size - 1);
        out[out_size - 1] = '\0';
        if (level_out)
            *level_out = g.ring[pos].level;
        ok = 1;
    }
    do_unlock();
    return ok;
}

unsigned long vi_log_total(void)
{
    return g.total;
}
