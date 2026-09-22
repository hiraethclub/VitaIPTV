/*
 * VitaIPTV - streaming M3U / M3U8 playlist parser (core/, C99).
 *
 * This is an incremental "push" parser: feed it byte chunks as they arrive
 * (e.g. from the network fetch layer) and it appends channels to a
 * vi_playlist without ever holding the whole input as one string. Only the
 * current line is buffered.
 *
 * It is deliberately tolerant: malformed lines, missing attributes, CRLF or
 * LF endings, a leading UTF-8 BOM, blank lines, quoted and unquoted attribute
 * values, and unknown "#" directives are all handled without failing. The
 * only failure mode is out-of-memory, reported via the return value and
 * vi_playlist.oom.
 */
#ifndef VI_M3U_H
#define VI_M3U_H

#include <stddef.h>

#include "vi_playlist.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    vi_playlist *out;
    char        *line;      /* current partial line buffer */
    size_t       line_len;
    size_t       line_cap;
    size_t       line_no;   /* lines processed so far (BOM strip on line 0) */
    vi_channel   cur;       /* channel being built between EXTINF and URL */
    int          have_pending;
} vi_m3u_parser;

/* Begin parsing into an initialised, empty (or existing) playlist. */
int vi_m3u_begin(vi_m3u_parser *p, vi_playlist *out);

/* Feed a chunk of bytes. Returns 0 on success, -1 on OOM. */
int vi_m3u_feed(vi_m3u_parser *p, const char *data, size_t len);

/* Flush the final line and release the parser's line buffer. */
int vi_m3u_end(vi_m3u_parser *p);

/* Convenience: parse an entire in-memory buffer. */
int vi_m3u_parse_buffer(const char *data, size_t len, vi_playlist *out);

/*
 * Convenience: parse a file, read in chunks. Returns 0 on success, -1 on OOM,
 * -2 if the file could not be opened.
 */
int vi_m3u_parse_file(const char *path, vi_playlist *out);

#ifdef __cplusplus
}
#endif

#endif /* VI_M3U_H */
