/*
 * VitaIPTV - HLS playlist parsing (core/, C99, portable).
 *
 * Parses the two kinds of .m3u8 an HLS stream uses:
 *   - a MASTER playlist: a set of variant streams (#EXT-X-STREAM-INF), each
 *     with bandwidth / codecs / resolution and a URI to a media playlist.
 *   - a MEDIA playlist: an ordered list of segments (#EXTINF + URI), plus
 *     media-sequence, target-duration, an endlist flag (VOD vs live), and
 *     whether the segments are encrypted (#EXT-X-KEY).
 *
 * Variant selection prefers the best H.264 ("avc1") variant at or below a
 * resolution ceiling, per the brief - not simply the highest bandwidth.
 *
 * A small URL-resolve helper turns a relative playlist/segment URI into an
 * absolute URL against the playlist it came from.
 */
#ifndef VI_HLS_H
#define VI_HLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *uri;         /* variant media-playlist URI (may be relative) */
    long   bandwidth;   /* BANDWIDTH attribute, bits/s (0 if absent) */
    int    width;       /* RESOLUTION width (0 if absent) */
    int    height;      /* RESOLUTION height (0 if absent) */
    int    is_h264;     /* 1 if CODECS contains an avc1 (H.264) entry */
    char  *codecs;      /* raw CODECS string (may be NULL) */
} vi_hls_variant;

typedef struct {
    vi_hls_variant *variants;
    size_t          count;
    size_t          cap;
} vi_hls_master;

typedef struct {
    char      *uri;         /* segment URI (may be relative) */
    double     duration;    /* EXTINF duration in seconds */
    long long  seq;         /* absolute media sequence number of this segment */
} vi_hls_segment;

typedef struct {
    vi_hls_segment *segments;
    size_t          count;
    size_t          cap;
    long long       media_sequence;   /* EXT-X-MEDIA-SEQUENCE (default 0) */
    int             target_duration;  /* EXT-X-TARGETDURATION seconds */
    int             endlist;          /* 1 if EXT-X-ENDLIST (VOD), 0 = live */
    int             encrypted;        /* 1 if any EXT-X-KEY METHOD != NONE */
} vi_hls_media;

/* Returns 1 if the buffer looks like a master playlist (has STREAM-INF). */
int vi_hls_is_master(const char *buf, size_t len);

/* Parse. Return 0 on success, -1 on OOM. Output is zero-initialised first. */
int vi_hls_parse_master(const char *buf, size_t len, vi_hls_master *out);
int vi_hls_parse_media(const char *buf, size_t len, vi_hls_media *out);

void vi_hls_master_free(vi_hls_master *m);
void vi_hls_media_free(vi_hls_media *m);

/*
 * Choose a variant: the highest-resolution H.264 variant whose width and
 * height are both <= the ceiling. If none fit (or resolutions are unknown),
 * falls back to the lowest-resolution H.264 variant as a best effort. Returns
 * NULL only if there are no H.264 variants at all. Pass 0 for a ceiling
 * dimension to mean "no limit on that axis".
 */
const vi_hls_variant *vi_hls_select_h264(const vi_hls_master *m,
                                         int max_w, int max_h);

/*
 * Resolve a (possibly relative) URI against the absolute URL of the playlist
 * it came from. Handles absolute URLs, protocol-relative ("//host/..."),
 * absolute paths ("/path"), and plain relative refs. Returns 0 on success, -1
 * if the result would not fit in out.
 */
int vi_url_resolve(const char *base_url, const char *ref,
                   char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* VI_HLS_H */
