#include "vi_hls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers ------------------------------------------------------- */

static char *dup_span(const char *s, size_t len)
{
    char *d = (char *)malloc(len + 1);
    if (d == NULL)
        return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

/* Iterate lines over [*, end), stripping the trailing CR and surrounding
 * spaces/tabs. Advances *p. Returns 1 while a line is produced. */
static int next_line(const char **p, const char *end,
                     const char **line, size_t *line_len)
{
    const char *s, *nl, *b, *e;

    if (*p >= end)
        return 0;

    s = *p;
    nl = (const char *)memchr(s, '\n', (size_t)(end - s));
    if (nl) {
        e = nl;
        *p = nl + 1;
    } else {
        e = end;
        *p = end;
    }
    b = s;
    if (e > b && e[-1] == '\r')
        e--;
    while (b < e && (*b == ' ' || *b == '\t'))
        b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
        e--;

    *line = b;
    *line_len = (size_t)(e - b);
    return 1;
}

static int has_prefix(const char *s, size_t len, const char *pfx)
{
    size_t n = strlen(pfx);
    return len >= n && memcmp(s, pfx, n) == 0;
}

/* Case-insensitive substring search within a bounded span. */
static int span_contains_ci(const char *s, size_t len, const char *needle)
{
    size_t nlen = strlen(needle), i, j;
    if (nlen == 0 || nlen > len)
        return 0;
    for (i = 0; i + nlen <= len; i++) {
        for (j = 0; j < nlen; j++) {
            char a = s[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (j == nlen)
            return 1;
    }
    return 0;
}

/*
 * Find attribute `key` in a comma-separated attribute list (the part of an
 * EXT-X-* line after the colon), copying its value into out. Quoted values may
 * contain commas. Returns 1 if found.
 */
static int get_attr(const char *s, size_t len, const char *key,
                    char *out, size_t out_size)
{
    size_t klen = strlen(key);
    size_t i = 0;

    while (i < len) {
        size_t ks, ke, vs, ve;

        while (i < len && (s[i] == ',' || s[i] == ' '))
            i++;
        ks = i;
        while (i < len && s[i] != '=' && s[i] != ',')
            i++;
        ke = i;
        if (i >= len || s[i] != '=') {
            /* value-less token; skip to next comma */
            while (i < len && s[i] != ',')
                i++;
            continue;
        }
        i++; /* consume '=' */
        if (i < len && s[i] == '"') {
            i++;
            vs = i;
            while (i < len && s[i] != '"')
                i++;
            ve = i;
            if (i < len)
                i++; /* closing quote */
        } else {
            vs = i;
            while (i < len && s[i] != ',')
                i++;
            ve = i;
        }
        if (ke - ks == klen && memcmp(s + ks, key, klen) == 0) {
            size_t vlen = ve - vs;
            if (vlen >= out_size)
                vlen = out_size - 1;
            memcpy(out, s + vs, vlen);
            out[vlen] = '\0';
            return 1;
        }
    }
    return 0;
}

/* ---- master playlist ----------------------------------------------------- */

int vi_hls_is_master(const char *buf, size_t len)
{
    return span_contains_ci(buf, len, "#EXT-X-STREAM-INF");
}

static int master_push(vi_hls_master *m, const vi_hls_variant *v)
{
    if (m->count == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        vi_hls_variant *g =
            (vi_hls_variant *)realloc(m->variants, cap * sizeof(*g));
        if (g == NULL)
            return -1;
        m->variants = g;
        m->cap = cap;
    }
    m->variants[m->count++] = *v;
    return 0;
}

int vi_hls_parse_master(const char *buf, size_t len, vi_hls_master *out)
{
    const char *p = buf, *line;
    size_t ll;
    vi_hls_variant cur;
    int have_pending = 0;

    memset(out, 0, sizeof(*out));
    memset(&cur, 0, sizeof(cur));

    while (next_line(&p, buf + len, &line, &ll)) {
        if (ll == 0)
            continue;
        if (line[0] == '#') {
            if (has_prefix(line, ll, "#EXT-X-STREAM-INF:")) {
                char tmp[128];
                const char *a = line + 18; /* after the colon */
                size_t alen = ll - 18;

                memset(&cur, 0, sizeof(cur));
                if (get_attr(a, alen, "BANDWIDTH", tmp, sizeof(tmp)))
                    cur.bandwidth = strtol(tmp, NULL, 10);
                if (get_attr(a, alen, "RESOLUTION", tmp, sizeof(tmp))) {
                    char *x = strchr(tmp, 'x');
                    cur.width = (int)strtol(tmp, NULL, 10);
                    if (x)
                        cur.height = (int)strtol(x + 1, NULL, 10);
                }
                if (get_attr(a, alen, "CODECS", tmp, sizeof(tmp))) {
                    cur.codecs = dup_span(tmp, strlen(tmp));
                    if (cur.codecs == NULL)
                        return -1;
                    cur.is_h264 = span_contains_ci(tmp, strlen(tmp), "avc1");
                }
                have_pending = 1;
            }
            /* other #EXT tags (VERSION, MEDIA, ...) ignored */
            continue;
        }
        if (have_pending) {
            cur.uri = dup_span(line, ll);
            if (cur.uri == NULL) {
                free(cur.codecs);
                return -1;
            }
            if (master_push(out, &cur) != 0) {
                free(cur.uri);
                free(cur.codecs);
                return -1;
            }
            memset(&cur, 0, sizeof(cur));
            have_pending = 0;
        }
    }
    return 0;
}

void vi_hls_master_free(vi_hls_master *m)
{
    size_t i;
    for (i = 0; i < m->count; i++) {
        free(m->variants[i].uri);
        free(m->variants[i].codecs);
    }
    free(m->variants);
    memset(m, 0, sizeof(*m));
}

const vi_hls_variant *vi_hls_select_h264(const vi_hls_master *m,
                                         int max_w, int max_h)
{
    const vi_hls_variant *best_fit = NULL;   /* highest area within ceiling */
    const vi_hls_variant *smallest = NULL;   /* fallback: lowest area */
    long best_fit_area = -1;
    long smallest_area = -1;
    size_t i;

    for (i = 0; i < m->count; i++) {
        const vi_hls_variant *v = &m->variants[i];
        long area;
        if (!v->is_h264)
            continue;
        area = (long)v->width * (long)v->height;

        if (smallest == NULL || area < smallest_area) {
            smallest = v;
            smallest_area = area;
        }
        if ((max_w == 0 || v->width <= max_w) &&
            (max_h == 0 || v->height <= max_h)) {
            if (area > best_fit_area) {
                best_fit = v;
                best_fit_area = area;
            }
        }
    }
    return best_fit ? best_fit : smallest;
}

/* ---- media playlist ------------------------------------------------------ */

static int media_push(vi_hls_media *m, const vi_hls_segment *s)
{
    if (m->count == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 16;
        vi_hls_segment *g =
            (vi_hls_segment *)realloc(m->segments, cap * sizeof(*g));
        if (g == NULL)
            return -1;
        m->segments = g;
        m->cap = cap;
    }
    m->segments[m->count++] = *s;
    return 0;
}

int vi_hls_parse_media(const char *buf, size_t len, vi_hls_media *out)
{
    const char *p = buf, *line;
    size_t ll;
    double pending_dur = 0.0;
    int have_pending = 0;

    memset(out, 0, sizeof(*out));

    while (next_line(&p, buf + len, &line, &ll)) {
        if (ll == 0)
            continue;
        if (line[0] == '#') {
            if (has_prefix(line, ll, "#EXT-X-MEDIA-SEQUENCE:")) {
                out->media_sequence = strtoll(line + 22, NULL, 10);
            } else if (has_prefix(line, ll, "#EXT-X-TARGETDURATION:")) {
                out->target_duration = (int)strtol(line + 22, NULL, 10);
            } else if (has_prefix(line, ll, "#EXT-X-ENDLIST")) {
                out->endlist = 1;
            } else if (has_prefix(line, ll, "#EXT-X-KEY:")) {
                char method[32];
                if (get_attr(line + 11, ll - 11, "METHOD",
                             method, sizeof(method))) {
                    if (strcmp(method, "NONE") != 0)
                        out->encrypted = 1;
                }
            } else if (has_prefix(line, ll, "#EXTINF:")) {
                pending_dur = strtod(line + 8, NULL);
                have_pending = 1;
            }
            continue;
        }
        if (have_pending) {
            vi_hls_segment seg;
            seg.uri = dup_span(line, ll);
            if (seg.uri == NULL)
                return -1;
            seg.duration = pending_dur;
            seg.seq = out->media_sequence + (long long)out->count;
            if (media_push(out, &seg) != 0) {
                free(seg.uri);
                return -1;
            }
            have_pending = 0;
        }
    }
    return 0;
}

void vi_hls_media_free(vi_hls_media *m)
{
    size_t i;
    for (i = 0; i < m->count; i++)
        free(m->segments[i].uri);
    free(m->segments);
    memset(m, 0, sizeof(*m));
}

/* ---- URL resolution ------------------------------------------------------ */

static int starts_with_ci(const char *s, const char *pfx)
{
    while (*pfx) {
        char a = *s++, b = *pfx++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

int vi_url_resolve(const char *base_url, const char *ref,
                   char *out, size_t out_size)
{
    const char *scheme_end, *authority, *host_end, *base_q, *last_slash;
    int n;

    if (ref == NULL || out_size == 0)
        return -1;

    /* Absolute URL: use as-is. */
    if (starts_with_ci(ref, "http://") || starts_with_ci(ref, "https://")) {
        if (strlen(ref) >= out_size)
            return -1;
        strcpy(out, ref);
        return 0;
    }

    /* Locate scheme:// and host in the base. */
    scheme_end = strstr(base_url, "://");
    if (scheme_end == NULL)
        return -1;
    authority = scheme_end + 3;
    host_end = strchr(authority, '/');
    if (host_end == NULL)
        host_end = authority + strlen(authority);

    /* Protocol-relative: //host/path */
    if (ref[0] == '/' && ref[1] == '/') {
        n = snprintf(out, out_size, "%.*s:%s",
                     (int)(scheme_end - base_url), base_url, ref);
        return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
    }

    /* Absolute path: /path -> scheme://host + ref */
    if (ref[0] == '/') {
        n = snprintf(out, out_size, "%.*s%s",
                     (int)(host_end - base_url), base_url, ref);
        return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
    }

    /* Relative ref: base directory (strip query and last path segment) + ref */
    base_q = strchr(base_url, '?');
    {
        const char *scan_end = base_q ? base_q : base_url + strlen(base_url);
        const char *dir_start = host_end; /* first '/' after host, or end */
        last_slash = NULL;
        {
            const char *c;
            for (c = dir_start; c < scan_end; c++)
                if (*c == '/')
                    last_slash = c;
        }
        if (last_slash == NULL) {
            /* no path after host: base is scheme://host, append "/ref" */
            n = snprintf(out, out_size, "%.*s/%s",
                         (int)(host_end - base_url), base_url, ref);
        } else {
            n = snprintf(out, out_size, "%.*s%s",
                         (int)(last_slash - base_url + 1), base_url, ref);
        }
    }
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
}
