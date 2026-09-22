#include "vi_m3u.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------- */

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

static int is_key_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int str_has_prefix(const char *s, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    return len >= n && memcmp(s, prefix, n) == 0;
}

/* Compare a [s,len) span against a NUL-terminated key, case-insensitively. */
static int span_eq_ci(const char *s, size_t len, const char *key)
{
    size_t i;
    for (i = 0; i < len; i++) {
        char a = s[i];
        char b = key[i];
        if (b == '\0')
            return 0;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return key[len] == '\0';
}

/* Assign an interned value to the matching channel field. */
static void set_attr(vi_playlist *out, vi_channel *ch,
                     const char *key, size_t klen,
                     const char *val, size_t vlen)
{
    uint32_t off;

    if (span_eq_ci(key, klen, "tvg-id"))
        off = vi_playlist_intern(out, val, vlen), ch->tvg_id = off;
    else if (span_eq_ci(key, klen, "tvg-name"))
        off = vi_playlist_intern(out, val, vlen), ch->tvg_name = off;
    else if (span_eq_ci(key, klen, "tvg-logo"))
        off = vi_playlist_intern(out, val, vlen), ch->tvg_logo = off;
    else if (span_eq_ci(key, klen, "group-title"))
        off = vi_playlist_intern(out, val, vlen), ch->group = off;
    else if (span_eq_ci(key, klen, "http-user-agent"))
        off = vi_playlist_intern(out, val, vlen), ch->user_agent = off;
    else if (span_eq_ci(key, klen, "http-referrer"))
        off = vi_playlist_intern(out, val, vlen), ch->referrer = off;
    /* unknown attributes are ignored */
}

/* ---- attribute / line parsing --------------------------------------- */

/*
 * Parse the attribute region of an EXTINF line: a run of key="value" or
 * key=value tokens. Values may contain anything except (for quoted values) a
 * closing quote, or (for unquoted values) a space or comma.
 */
static void parse_attrs(vi_playlist *out, vi_channel *ch,
                        const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t ks, ke, vs, ve;

        while (i < len && (is_space(s[i]) || s[i] == ','))
            i++;
        if (i >= len)
            break;

        ks = i;
        while (i < len && is_key_char(s[i]))
            i++;
        ke = i;
        if (ke == ks) {
            /* not a key character; skip it to avoid stalling */
            i++;
            continue;
        }
        if (i >= len || s[i] != '=') {
            /* key with no value; skip to next separator */
            while (i < len && !is_space(s[i]))
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
                i++; /* consume closing quote */
        } else {
            vs = i;
            while (i < len && !is_space(s[i]))
                i++;
            ve = i;
        }
        set_attr(out, ch, s + ks, ke - ks, s + vs, ve - vs);
    }
}

/* Parse an EXTINF line body (everything after "#EXTINF:"). */
static void parse_extinf(vi_m3u_parser *p, const char *s, size_t len)
{
    size_t i = 0;
    size_t attr_start;
    size_t split;      /* index of the name-separating comma, or len */
    int in_quote = 0;
    long dur = 0;
    int neg = 0;

    /* A fresh EXTINF discards any previous channel that never got a URL. */
    memset(&p->cur, 0, sizeof(p->cur));
    p->have_pending = 1;

    while (i < len && is_space(s[i]))
        i++;

    /* duration: optional sign then digits (fractional part ignored) */
    if (i < len && (s[i] == '-' || s[i] == '+')) {
        neg = (s[i] == '-');
        i++;
    }
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        dur = dur * 10 + (s[i] - '0');
        i++;
    }
    p->cur.duration = (int)(neg ? -dur : dur);

    /* find the first comma that is not inside a quoted attribute value */
    attr_start = i;
    split = len;
    while (i < len) {
        if (s[i] == '"')
            in_quote = !in_quote;
        else if (s[i] == ',' && !in_quote) {
            split = i;
            break;
        }
        i++;
    }

    if (split > attr_start)
        parse_attrs(p->out, &p->cur, s + attr_start, split - attr_start);

    if (split < len) {
        const char *name = s + split + 1;
        size_t nlen = len - (split + 1);
        p->cur.name = vi_playlist_intern(p->out, name, nlen);
    }
}

/* Parse an EXTVLCOPT line body (everything after "#EXTVLCOPT:"). */
static void parse_extvlcopt(vi_m3u_parser *p, const char *s, size_t len)
{
    size_t eq = 0;
    size_t klen, vs, vlen;

    if (!p->have_pending)
        return; /* option with no channel to attach to */

    while (eq < len && s[eq] != '=')
        eq++;
    if (eq >= len)
        return; /* no '=' */

    klen = eq;
    while (klen > 0 && is_space(s[klen - 1]))
        klen--;

    vs = eq + 1;
    vlen = len - vs;
    /* strip one layer of surrounding quotes if present */
    if (vlen >= 2 && s[vs] == '"' && s[vs + vlen - 1] == '"') {
        vs++;
        vlen -= 2;
    }

    if (span_eq_ci(s, klen, "http-user-agent"))
        p->cur.user_agent = vi_playlist_intern(p->out, s + vs, vlen);
    else if (span_eq_ci(s, klen, "http-referrer"))
        p->cur.referrer = vi_playlist_intern(p->out, s + vs, vlen);
}

/* A non-directive line: the stream URL that completes the current channel. */
static void parse_url(vi_m3u_parser *p, const char *s, size_t len)
{
    while (len > 0 && is_space(s[len - 1]))
        len--;
    if (len == 0)
        return;

    if (!p->have_pending)
        memset(&p->cur, 0, sizeof(p->cur));

    p->cur.url = vi_playlist_intern(p->out, s, len);
    vi_playlist_add(p->out, &p->cur);

    memset(&p->cur, 0, sizeof(p->cur));
    p->have_pending = 0;
}

static void process_line(vi_m3u_parser *p, const char *s, size_t len)
{
    int first = (p->line_no == 0);
    p->line_no++;

    /* strip a leading UTF-8 BOM on the very first line */
    if (first && len >= 3 &&
        (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        s += 3;
        len -= 3;
    }

    /* strip trailing CR (CRLF endings) */
    if (len > 0 && s[len - 1] == '\r')
        len--;

    /* left-trim whitespace for tolerant classification */
    while (len > 0 && is_space(s[0])) {
        s++;
        len--;
    }

    if (len == 0)
        return; /* blank line */

    if (s[0] == '#') {
        if (str_has_prefix(s, len, "#EXTINF:"))
            parse_extinf(p, s + 8, len - 8);
        else if (str_has_prefix(s, len, "#EXTVLCOPT:"))
            parse_extvlcopt(p, s + 11, len - 11);
        /* #EXTM3U and any other directive: ignored */
        return;
    }

    parse_url(p, s, len);
}

/* ---- line buffering ------------------------------------------------- */

static int line_append(vi_m3u_parser *p, const char *data, size_t len)
{
    if (p->line_len + len > p->line_cap) {
        size_t cap = p->line_cap ? p->line_cap : 256;
        char *grown;
        while (cap < p->line_len + len)
            cap *= 2;
        grown = (char *)realloc(p->line, cap);
        if (grown == NULL) {
            p->out->oom = 1;
            return -1;
        }
        p->line = grown;
        p->line_cap = cap;
    }
    memcpy(p->line + p->line_len, data, len);
    p->line_len += len;
    return 0;
}

/* ---- public API ----------------------------------------------------- */

int vi_m3u_begin(vi_m3u_parser *p, vi_playlist *out)
{
    memset(p, 0, sizeof(*p));
    p->out = out;
    return 0;
}

int vi_m3u_feed(vi_m3u_parser *p, const char *data, size_t len)
{
    while (len > 0) {
        const char *nl = (const char *)memchr(data, '\n', len);
        size_t chunk = nl ? (size_t)(nl - data) : len;

        if (chunk > 0 && line_append(p, data, chunk) != 0)
            return -1;

        if (nl) {
            process_line(p, p->line, p->line_len);
            p->line_len = 0;
            data = nl + 1;
            len -= chunk + 1;
        } else {
            break;
        }
        if (p->out->oom)
            return -1;
    }
    return p->out->oom ? -1 : 0;
}

int vi_m3u_end(vi_m3u_parser *p)
{
    if (p->line_len > 0)
        process_line(p, p->line, p->line_len);
    p->line_len = 0;
    free(p->line);
    p->line = NULL;
    p->line_cap = 0;
    return p->out->oom ? -1 : 0;
}

int vi_m3u_parse_buffer(const char *data, size_t len, vi_playlist *out)
{
    vi_m3u_parser p;
    vi_m3u_begin(&p, out);
    vi_m3u_feed(&p, data, len);
    return vi_m3u_end(&p);
}

int vi_m3u_parse_file(const char *path, vi_playlist *out)
{
    vi_m3u_parser p;
    FILE *f;
    char buf[16384];
    size_t n;
    int rc = 0;

    f = fopen(path, "rb");
    if (f == NULL)
        return -2;

    vi_m3u_begin(&p, out);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (vi_m3u_feed(&p, buf, n) != 0) {
            rc = -1;
            break;
        }
    }
    if (vi_m3u_end(&p) != 0)
        rc = -1;

    fclose(f);
    return rc;
}
