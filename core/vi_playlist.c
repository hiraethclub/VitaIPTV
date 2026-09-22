#include "vi_playlist.h"

#include <stdlib.h>
#include <string.h>

void vi_playlist_init(vi_playlist *pl)
{
    memset(pl, 0, sizeof(*pl));
}

void vi_playlist_free(vi_playlist *pl)
{
    free(pl->arena);
    free(pl->channels);
    memset(pl, 0, sizeof(*pl));
}

size_t vi_playlist_count(const vi_playlist *pl)
{
    return pl->count;
}

const vi_channel *vi_playlist_at(const vi_playlist *pl, size_t index)
{
    if (index >= pl->count)
        return NULL;
    return &pl->channels[index];
}

const char *vi_playlist_str(const vi_playlist *pl, uint32_t offset)
{
    if (pl->arena == NULL || offset >= pl->arena_len)
        return "";
    return pl->arena + offset;
}

static int arena_reserve(vi_playlist *pl, size_t extra)
{
    size_t need;
    size_t cap;
    char *grown;

    /* Ensure arena[0] exists as the empty string. */
    if (pl->arena == NULL) {
        cap = 1024;
        grown = (char *)malloc(cap);
        if (grown == NULL) {
            pl->oom = 1;
            return -1;
        }
        grown[0] = '\0';
        pl->arena = grown;
        pl->arena_cap = cap;
        pl->arena_len = 1; /* offset 0 taken by the empty string */
    }

    need = pl->arena_len + extra;
    if (need <= pl->arena_cap)
        return 0;

    cap = pl->arena_cap;
    while (cap < need) {
        if (cap > (SIZE_MAX / 2)) {
            pl->oom = 1;
            return -1;
        }
        cap *= 2;
    }
    grown = (char *)realloc(pl->arena, cap);
    if (grown == NULL) {
        pl->oom = 1;
        return -1;
    }
    pl->arena = grown;
    pl->arena_cap = cap;
    return 0;
}

uint32_t vi_playlist_intern(vi_playlist *pl, const char *s, size_t len)
{
    uint32_t offset;

    if (len == 0)
        return 0;

    /* Offsets are 32-bit; refuse to build an arena that would overflow them. */
    if (len > (size_t)UINT32_MAX - 2) {
        pl->oom = 1;
        return 0;
    }

    if (arena_reserve(pl, len + 1) != 0)
        return 0;

    offset = (uint32_t)pl->arena_len;
    memcpy(pl->arena + pl->arena_len, s, len);
    pl->arena[pl->arena_len + len] = '\0';
    pl->arena_len += len + 1;
    return offset;
}

int vi_playlist_add(vi_playlist *pl, const vi_channel *ch)
{
    if (pl->count == pl->cap) {
        size_t cap = pl->cap ? pl->cap * 2 : 64;
        vi_channel *grown;
        if (cap > SIZE_MAX / sizeof(vi_channel)) {
            pl->oom = 1;
            return -1;
        }
        grown = (vi_channel *)realloc(pl->channels, cap * sizeof(vi_channel));
        if (grown == NULL) {
            pl->oom = 1;
            return -1;
        }
        pl->channels = grown;
        pl->cap = cap;
    }
    pl->channels[pl->count++] = *ch;
    return 0;
}

const char *vi_channel_name(const vi_playlist *pl, const vi_channel *ch)
{
    return vi_playlist_str(pl, ch->name);
}
const char *vi_channel_tvg_id(const vi_playlist *pl, const vi_channel *ch)
{
    return vi_playlist_str(pl, ch->tvg_id);
}
const char *vi_channel_tvg_name(const vi_playlist *pl, const vi_channel *ch)
{
    return vi_playlist_str(pl, ch->tvg_name);
}
const char *vi_channel_tvg_logo(const vi_playlist *pl, const vi_channel *ch)
{
    return vi_playlist_str(pl, ch->tvg_logo);
}
const char *vi_channel_group(const vi_playlist *pl, const vi_channel *ch)
{
    return vi_playlist_str(pl, ch->group);
}
const char *vi_channel_url(const vi_playlist *pl, const vi_channel *ch)
{
    return vi_playlist_str(pl, ch->url);
}
const char *vi_channel_user_agent(const vi_playlist *pl, const vi_channel *ch)
{
    return vi_playlist_str(pl, ch->user_agent);
}
const char *vi_channel_referrer(const vi_playlist *pl, const vi_channel *ch)
{
    return vi_playlist_str(pl, ch->referrer);
}
