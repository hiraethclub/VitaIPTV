/*
 * Memory and speed report for the M3U parser, aimed at the iptv-org index.m3u
 * stress case. Prints wall-clock parse time, throughput, the parser's own
 * compact footprint (arena + channel array), and process peak RSS.
 *
 * Usage: bench_m3u [path-to.m3u]   (defaults to tests/data/index.m3u)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#include "vi_m3u.h"
#include "vi_playlist.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "tests/data/index.m3u";
    vi_playlist pl;
    double t0, t1;
    long fsize;
    int rc;
    struct rusage ru;
    size_t chan_bytes, arena_bytes;

    fsize = file_size(path);
    if (fsize < 0) {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }

    vi_playlist_init(&pl);
    t0 = now_sec();
    rc = vi_m3u_parse_file(path, &pl);
    t1 = now_sec();

    if (rc != 0) {
        fprintf(stderr, "parse failed (rc=%d, oom=%d)\n", rc, pl.oom);
        vi_playlist_free(&pl);
        return 1;
    }

    chan_bytes = pl.cap * sizeof(vi_channel);
    arena_bytes = pl.arena_cap;
    getrusage(RUSAGE_SELF, &ru);

    printf("file:            %s\n", path);
    printf("file size:       %ld bytes (%.2f MiB)\n",
           fsize, (double)fsize / (1024.0 * 1024.0));
    printf("channels:        %zu\n", vi_playlist_count(&pl));
    printf("parse time:      %.1f ms\n", (t1 - t0) * 1000.0);
    if (t1 > t0)
        printf("throughput:      %.1f MiB/s\n",
               ((double)fsize / (1024.0 * 1024.0)) / (t1 - t0));
    printf("arena (strings): %zu bytes used, %zu allocated\n",
           pl.arena_len, arena_bytes);
    printf("channel array:   %zu bytes (%zu B/channel struct)\n",
           chan_bytes, sizeof(vi_channel));
    printf("parser heap:     %.2f MiB (arena alloc + channel array)\n",
           (double)(arena_bytes + chan_bytes) / (1024.0 * 1024.0));
    if (vi_playlist_count(&pl) > 0)
        printf("bytes/channel:   %.1f (parser heap / channel)\n",
               (double)(arena_bytes + chan_bytes) /
                   (double)vi_playlist_count(&pl));
    printf("process peak RSS:%.2f MiB\n",
           (double)ru.ru_maxrss / 1024.0); /* ru_maxrss is KiB on Linux */

    vi_playlist_free(&pl);
    return 0;
}
