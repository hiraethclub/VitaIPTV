/*
 * VitaIPTV - portable playlist data model (core/, C99, no Vita headers).
 *
 * Storage is deliberately compact so large playlists (iptv-org index.m3u has
 * ~11k channels) do not blow the Vita's limited memory:
 *
 *   - All strings live in a single growing "arena" buffer.
 *   - Each channel stores 32-bit byte OFFSETS into that arena, not pointers,
 *     so the arena can be realloc'd freely and channels stay valid.
 *   - Offset 0 is reserved as the empty string, so a zeroed channel reads as
 *     all-empty fields.
 */
#ifndef VI_PLAYLIST_H
#define VI_PLAYLIST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A single channel. Fields are offsets into vi_playlist.arena; read them with
 * the vi_channel_* accessors. duration is the EXTINF duration (typically -1
 * for live streams).
 */
typedef struct {
    uint32_t name;        /* display name after the EXTINF comma */
    uint32_t tvg_id;      /* kept even though unused for now */
    uint32_t tvg_name;
    uint32_t tvg_logo;
    uint32_t group;       /* group-title */
    uint32_t url;
    uint32_t user_agent;  /* from http-user-agent (attr or EXTVLCOPT) */
    uint32_t referrer;    /* from http-referrer (attr or EXTVLCOPT) */
    int32_t  duration;
} vi_channel;

typedef struct {
    char       *arena;      /* string pool; arena[0] == '\0' */
    size_t      arena_len;
    size_t      arena_cap;

    vi_channel *channels;
    size_t      count;
    size_t      cap;

    int         oom;        /* set if any allocation failed during parsing */
} vi_playlist;

void vi_playlist_init(vi_playlist *pl);
void vi_playlist_free(vi_playlist *pl);

/* Number of channels parsed. */
size_t vi_playlist_count(const vi_playlist *pl);

/* Channel accessor; returns NULL if index is out of range. */
const vi_channel *vi_playlist_at(const vi_playlist *pl, size_t index);

/*
 * Field accessors. Never return NULL for a valid channel: an unset field
 * reads back as "" (empty string), which keeps callers simple.
 */
const char *vi_channel_name(const vi_playlist *pl, const vi_channel *ch);
const char *vi_channel_tvg_id(const vi_playlist *pl, const vi_channel *ch);
const char *vi_channel_tvg_name(const vi_playlist *pl, const vi_channel *ch);
const char *vi_channel_tvg_logo(const vi_playlist *pl, const vi_channel *ch);
const char *vi_channel_group(const vi_playlist *pl, const vi_channel *ch);
const char *vi_channel_url(const vi_playlist *pl, const vi_channel *ch);
const char *vi_channel_user_agent(const vi_playlist *pl, const vi_channel *ch);
const char *vi_channel_referrer(const vi_playlist *pl, const vi_channel *ch);

/*
 * Low-level building blocks, exposed for the parser (and tests). Most callers
 * should use the M3U parser instead.
 *
 * vi_playlist_intern copies [s, s+len) into the arena, appends a NUL, and
 * returns its offset. An empty string returns 0 without allocating. On OOM it
 * sets pl->oom and returns 0.
 */
uint32_t vi_playlist_intern(vi_playlist *pl, const char *s, size_t len);

/* Append a channel (copied by value). Returns 0 on success, -1 on OOM. */
int vi_playlist_add(vi_playlist *pl, const vi_channel *ch);

/* Resolve an arena offset to a C string (offset 0 -> ""). */
const char *vi_playlist_str(const vi_playlist *pl, uint32_t offset);

#ifdef __cplusplus
}
#endif

#endif /* VI_PLAYLIST_H */
