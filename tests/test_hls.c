/*
 * Unit tests for the core HLS parser and URL resolver. Runs natively; uses the
 * captured FailArmy playlists under tests/data/hls/ as real fixtures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vi_hls.h"

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, msg) do {                                   \
    g_checks++;                                                 \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

#define CHECK_STR(got, want, msg) do {                          \
    g_checks++;                                                 \
    if (strcmp((got), (want)) != 0) { g_fail++;                 \
        printf("  FAIL: %s: got \"%s\" want \"%s\" (%s:%d)\n",  \
               (msg), (got), (want), __FILE__, __LINE__); }     \
} while (0)

static const char *data_dir(void)
{
    const char *d = getenv("VITAIPTV_TEST_DATA");
    return d ? d : "tests/data";
}

/* Read a whole file into a malloc'd buffer; returns length, -1 if absent. */
static long read_file(const char *rel, char **out)
{
    char path[1024];
    FILE *f;
    long n;
    char *buf;

    snprintf(path, sizeof(path), "%s/%s", data_dir(), rel);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return -1; }
    buf[n] = '\0';
    fclose(f);
    *out = buf;
    return n;
}

static void test_master_real(void)
{
    char *buf;
    long n;
    vi_hls_master m;
    const vi_hls_variant *v;

    printf("test_master_real\n");
    n = read_file("hls/failarmy_master.m3u8", &buf);
    if (n < 0) { printf("  SKIP: fixture missing\n"); return; }

    CHECK(vi_hls_is_master(buf, (size_t)n) == 1, "detected as master");
    CHECK(vi_hls_parse_master(buf, (size_t)n, &m) == 0, "parse ok");
    CHECK(m.count == 3, "three variants");
    if (m.count == 3) {
        CHECK(m.variants[0].width == 640 && m.variants[0].height == 360, "v0 360");
        CHECK(m.variants[2].width == 1280 && m.variants[2].height == 720, "v2 720");
        CHECK(m.variants[0].is_h264, "v0 is h264");
        CHECK(m.variants[0].bandwidth == 921600, "v0 bandwidth");
        CHECK_STR(m.variants[0].uri, "1200.m3u8", "v0 uri");
    }

    v = vi_hls_select_h264(&m, 1280, 720);
    CHECK(v && v->height == 720, "cap 720 -> 720p");
    v = vi_hls_select_h264(&m, 960, 540);
    CHECK(v && v->height == 540, "cap 540 -> 540p");
    v = vi_hls_select_h264(&m, 640, 360);
    CHECK(v && v->height == 360, "cap 360 -> 360p");
    v = vi_hls_select_h264(&m, 320, 240);
    CHECK(v && v->height == 360, "below all -> smallest (360p)");
    v = vi_hls_select_h264(&m, 0, 0);
    CHECK(v && v->height == 720, "no cap -> highest (720p)");

    vi_hls_master_free(&m);
    free(buf);
}

static void test_media_real(void)
{
    char *buf;
    long n;
    vi_hls_media md;

    printf("test_media_real\n");
    n = read_file("hls/failarmy_media.m3u8", &buf);
    if (n < 0) { printf("  SKIP: fixture missing\n"); return; }

    CHECK(vi_hls_is_master(buf, (size_t)n) == 0, "not a master");
    CHECK(vi_hls_parse_media(buf, (size_t)n, &md) == 0, "parse ok");
    CHECK(md.count > 0, "has segments");
    CHECK(md.target_duration == 6, "target duration 6");
    CHECK(md.endlist == 0, "live (no endlist)");
    CHECK(md.encrypted == 0, "not encrypted");
    CHECK(md.media_sequence > 0, "has media sequence");
    if (md.count >= 2) {
        CHECK(md.segments[0].seq == md.media_sequence, "first seq = media_seq");
        CHECK(md.segments[1].seq == md.media_sequence + 1, "seq increments");
        CHECK(md.segments[0].duration > 5.0 && md.segments[0].duration < 7.0,
              "duration ~6s");
        CHECK(strncmp(md.segments[0].uri, "https://", 8) == 0, "seg uri https");
    }
    vi_hls_media_free(&md);
    free(buf);
}

static void test_media_synthetic(void)
{
    const char *vod =
        "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:10.0,\nseg0.ts\n#EXTINF:10.0,\nseg1.ts\n#EXT-X-ENDLIST\n";
    const char *enc =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:6,\ns.ts\n";
    vi_hls_media a, b;

    printf("test_media_synthetic\n");
    CHECK(vi_hls_parse_media(vod, strlen(vod), &a) == 0, "vod parse");
    CHECK(a.count == 2, "vod 2 segs");
    CHECK(a.endlist == 1, "vod endlist");
    CHECK(a.encrypted == 0, "vod not encrypted");
    if (a.count == 2) CHECK_STR(a.segments[1].uri, "seg1.ts", "vod seg1 uri");
    vi_hls_media_free(&a);

    CHECK(vi_hls_parse_media(enc, strlen(enc), &b) == 0, "enc parse");
    CHECK(b.encrypted == 1, "AES-128 flagged encrypted");
    vi_hls_media_free(&b);
}

static void test_url_resolve(void)
{
    char out[512];

    printf("test_url_resolve\n");
    CHECK(vi_url_resolve("https://h/a/b/p.m3u8",
                         "https://other/x.ts", out, sizeof(out)) == 0, "abs ok");
    CHECK_STR(out, "https://other/x.ts", "absolute unchanged");

    CHECK(vi_url_resolve("https://h/a/b/p.m3u8", "1200.m3u8",
                         out, sizeof(out)) == 0, "rel ok");
    CHECK_STR(out, "https://h/a/b/1200.m3u8", "relative joined");

    CHECK(vi_url_resolve("https://h/a/b/p.m3u8", "/x/y.ts",
                         out, sizeof(out)) == 0, "abspath ok");
    CHECK_STR(out, "https://h/x/y.ts", "absolute path");

    CHECK(vi_url_resolve("https://h/a/p.m3u8", "//h2/z.m3u8",
                         out, sizeof(out)) == 0, "protorel ok");
    CHECK_STR(out, "https://h2/z.m3u8", "protocol-relative");

    CHECK(vi_url_resolve("https://h/a/b/p.m3u8?token=abc", "seg.ts",
                         out, sizeof(out)) == 0, "query base ok");
    CHECK_STR(out, "https://h/a/b/seg.ts", "relative ignores query");

    CHECK(vi_url_resolve("https://failarmy-international-gb.samsung.wurl.tv/playlist.m3u8",
                         "1200.m3u8", out, sizeof(out)) == 0, "real base ok");
    CHECK_STR(out,
        "https://failarmy-international-gb.samsung.wurl.tv/1200.m3u8",
        "real relative");
}

int main(void)
{
    test_master_real();
    test_media_real();
    test_media_synthetic();
    test_url_resolve();

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
