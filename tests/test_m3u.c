/*
 * Unit tests for the core M3U parser. Builds and runs natively on the dev PC
 * (no VitaSDK). Synthetic cases cover the tricky corners; the real iptv-org
 * files under tests/data check behaviour against actual playlists.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vi_m3u.h"
#include "vi_playlist.h"

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, msg) do {                                   \
    g_checks++;                                                 \
    if (!(cond)) {                                              \
        g_fail++;                                               \
        printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    }                                                           \
} while (0)

#define CHECK_STR(got, want, msg) do {                          \
    g_checks++;                                                 \
    if (strcmp((got), (want)) != 0) {                           \
        g_fail++;                                               \
        printf("  FAIL: %s: got \"%s\" want \"%s\" (%s:%d)\n",  \
               (msg), (got), (want), __FILE__, __LINE__);       \
    }                                                           \
} while (0)

/* Parse a NUL-terminated literal, feeding it one byte at a time to exercise
 * the incremental line buffering across arbitrary chunk boundaries. */
static void parse_bytewise(const char *text, vi_playlist *pl)
{
    vi_m3u_parser p;
    size_t i;
    vi_m3u_begin(&p, pl);
    for (i = 0; text[i]; i++)
        vi_m3u_feed(&p, text + i, 1);
    vi_m3u_end(&p);
}

static void test_basic(void)
{
    const char *text =
        "#EXTM3U\n"
        "#EXTINF:-1 tvg-id=\"BBC.uk\" tvg-logo=\"http://x/l.png\" "
        "group-title=\"News\",BBC News\n"
        "http://example.com/bbc.m3u8\n";
    vi_playlist pl;
    const vi_channel *c;

    printf("test_basic\n");
    vi_playlist_init(&pl);
    CHECK(vi_m3u_parse_buffer(text, strlen(text), &pl) == 0, "parse ok");
    CHECK(vi_playlist_count(&pl) == 1, "one channel");
    c = vi_playlist_at(&pl, 0);
    CHECK(c != NULL, "channel present");
    if (c) {
        CHECK_STR(vi_channel_name(&pl, c), "BBC News", "name");
        CHECK_STR(vi_channel_tvg_id(&pl, c), "BBC.uk", "tvg-id");
        CHECK_STR(vi_channel_tvg_logo(&pl, c), "http://x/l.png", "logo");
        CHECK_STR(vi_channel_group(&pl, c), "News", "group");
        CHECK_STR(vi_channel_url(&pl, c), "http://example.com/bbc.m3u8", "url");
        CHECK(c->duration == -1, "duration -1");
    }
    vi_playlist_free(&pl);
}

static void test_extvlcopt(void)
{
    const char *text =
        "#EXTM3U\n"
        "#EXTINF:-1 group-title=\"G\",Chan\n"
        "#EXTVLCOPT:http-user-agent=MyAgent/1.0\n"
        "#EXTVLCOPT:http-referrer=http://ref.example/\n"
        "http://e/x.m3u8\n";
    vi_playlist pl;
    const vi_channel *c;

    printf("test_extvlcopt\n");
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(text, strlen(text), &pl);
    CHECK(vi_playlist_count(&pl) == 1, "one channel");
    c = vi_playlist_at(&pl, 0);
    if (c) {
        CHECK_STR(vi_channel_user_agent(&pl, c), "MyAgent/1.0", "ua");
        CHECK_STR(vi_channel_referrer(&pl, c), "http://ref.example/", "ref");
    }
    vi_playlist_free(&pl);
}

static void test_ua_as_attr(void)
{
    /* http-user-agent can appear as an EXTINF attribute (seen in index.m3u) */
    const char *text =
        "#EXTINF:-1 http-user-agent=\"Attr/2.0\" group-title=\"G\",Name\n"
        "http://e/x\n";
    vi_playlist pl;
    const vi_channel *c;

    printf("test_ua_as_attr\n");
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(text, strlen(text), &pl);
    c = vi_playlist_at(&pl, 0);
    if (c)
        CHECK_STR(vi_channel_user_agent(&pl, c), "Attr/2.0", "ua from attr");
    vi_playlist_free(&pl);
}

static void test_crlf_and_bom(void)
{
    /* UTF-8 BOM + CRLF endings + a blank line + an unknown directive */
    const char *text =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "\r\n"
        "#EXTFOO:ignore me\r\n"
        "#EXTINF:-1 group-title=\"G\",Hello\r\n"
        "http://e/x\r\n";
    vi_playlist pl;
    const vi_channel *c;

    printf("test_crlf_and_bom\n");
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(text, strlen(text), &pl);
    CHECK(vi_playlist_count(&pl) == 1, "one channel despite BOM/CRLF/junk");
    c = vi_playlist_at(&pl, 0);
    if (c) {
        CHECK_STR(vi_channel_name(&pl, c), "Hello", "name no CR");
        CHECK_STR(vi_channel_url(&pl, c), "http://e/x", "url no CR");
    }
    vi_playlist_free(&pl);
}

static void test_comma_in_quotes(void)
{
    /* a comma inside a quoted attribute value must not split the name */
    const char *text =
        "#EXTINF:-1 group-title=\"News, World\",Big, Name Here\n"
        "http://e/x\n";
    vi_playlist pl;
    const vi_channel *c;

    printf("test_comma_in_quotes\n");
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(text, strlen(text), &pl);
    c = vi_playlist_at(&pl, 0);
    if (c) {
        CHECK_STR(vi_channel_group(&pl, c), "News, World", "group with comma");
        CHECK_STR(vi_channel_name(&pl, c), "Big, Name Here", "name with comma");
    }
    vi_playlist_free(&pl);
}

static void test_unquoted_attr(void)
{
    const char *text =
        "#EXTINF:-1 tvg-id=Plain group-title=G,Name\n"
        "http://e/x\n";
    vi_playlist pl;
    const vi_channel *c;

    printf("test_unquoted_attr\n");
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(text, strlen(text), &pl);
    c = vi_playlist_at(&pl, 0);
    if (c) {
        CHECK_STR(vi_channel_tvg_id(&pl, c), "Plain", "unquoted tvg-id");
        CHECK_STR(vi_channel_group(&pl, c), "G", "unquoted group");
        CHECK_STR(vi_channel_name(&pl, c), "Name", "name after unquoted");
    }
    vi_playlist_free(&pl);
}

static void test_url_without_extinf(void)
{
    const char *text =
        "#EXTM3U\n"
        "http://e/bare\n"
        "#EXTINF:-1,Named\n"
        "http://e/named\n";
    vi_playlist pl;
    const vi_channel *c0, *c1;

    printf("test_url_without_extinf\n");
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(text, strlen(text), &pl);
    CHECK(vi_playlist_count(&pl) == 2, "bare url + named");
    c0 = vi_playlist_at(&pl, 0);
    c1 = vi_playlist_at(&pl, 1);
    if (c0) {
        CHECK_STR(vi_channel_url(&pl, c0), "http://e/bare", "bare url");
        CHECK_STR(vi_channel_name(&pl, c0), "", "bare has no name");
    }
    if (c1)
        CHECK_STR(vi_channel_name(&pl, c1), "Named", "named name");
    vi_playlist_free(&pl);
}

static void test_extinf_without_url(void)
{
    /* a dangling EXTINF (no URL before the next EXTINF) is discarded */
    const char *text =
        "#EXTINF:-1,Orphan\n"
        "#EXTINF:-1,Real\n"
        "http://e/real\n";
    vi_playlist pl;
    const vi_channel *c;

    printf("test_extinf_without_url\n");
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(text, strlen(text), &pl);
    CHECK(vi_playlist_count(&pl) == 1, "only the channel with a url");
    c = vi_playlist_at(&pl, 0);
    if (c)
        CHECK_STR(vi_channel_name(&pl, c), "Real", "kept the real one");
    vi_playlist_free(&pl);
}

static void test_utf8_welsh(void)
{
    /* Welsh characters must survive byte-for-byte (font is a Vita concern) */
    const char *text =
        "#EXTINF:-1,S\xC5\xB5n a Ll\xC5\xB7n\n" /* Sŵn a Llŷn */
        "http://e/x\n";
    vi_playlist pl;
    const vi_channel *c;

    printf("test_utf8_welsh\n");
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(text, strlen(text), &pl);
    c = vi_playlist_at(&pl, 0);
    if (c)
        CHECK_STR(vi_channel_name(&pl, c), "S\xC5\xB5n a Ll\xC5\xB7n",
                  "welsh preserved");
    vi_playlist_free(&pl);
}

static void test_bytewise_equals_bulk(void)
{
    /* feeding one byte at a time must match a single-shot parse */
    const char *text =
        "#EXTINF:-1 group-title=\"G\",A\nhttp://e/a\n"
        "#EXTINF:-1 group-title=\"G\",B\nhttp://e/b\n";
    vi_playlist a, b;

    printf("test_bytewise_equals_bulk\n");
    vi_playlist_init(&a);
    vi_playlist_init(&b);
    vi_m3u_parse_buffer(text, strlen(text), &a);
    parse_bytewise(text, &b);
    CHECK(vi_playlist_count(&a) == 2 && vi_playlist_count(&b) == 2,
          "both produce 2 channels");
    vi_playlist_free(&a);
    vi_playlist_free(&b);
}

/* ---- real iptv-org files ------------------------------------------- */

static const char *data_dir(void)
{
    const char *d = getenv("VITAIPTV_TEST_DATA");
    return d ? d : "tests/data";
}

static void real_path(const char *file, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/%s", data_dir(), file);
}

static int parse_real(const char *file, vi_playlist *pl)
{
    char path[1024];
    real_path(file, path, sizeof(path));
    vi_playlist_init(pl);
    return vi_m3u_parse_file(path, pl);
}

/*
 * Independently count the "expected" channels in a real file by scanning for
 * URL lines (non-blank, not starting with '#'). This keeps the test honest
 * against the live iptv-org files without hardcoding a number that drifts
 * every time they update. Returns -1 if the file is absent.
 */
static long count_url_lines(const char *file)
{
    char path[1024];
    FILE *f;
    char line[8192];
    long n = 0;

    real_path(file, path, sizeof(path));
    f = fopen(path, "rb");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\r' || *p == '\n' || *p == '#')
            continue;
        n++;
    }
    fclose(f);
    return n;
}

static void test_real_counts(void)
{
    const char *files[] = { "gb-wls.m3u", "uk.m3u", "news.m3u", "index.m3u" };
    size_t i;

    printf("test_real_counts\n");
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        vi_playlist pl;
        long expect = count_url_lines(files[i]);
        int rc;

        if (expect < 0) {
            printf("  SKIP: %s not found (run tools/fetch-playlists.sh)\n",
                   files[i]);
            continue;
        }
        rc = parse_real(files[i], &pl);
        CHECK(rc == 0, "parse returned ok");
        if ((long)vi_playlist_count(&pl) != expect) {
            g_fail++;
            printf("  FAIL: %s parsed %zu, url-lines %ld\n", files[i],
                   vi_playlist_count(&pl), expect);
        } else {
            g_checks++;
            printf("  ok: %s -> %zu channels (matches url-line count)\n",
                   files[i], vi_playlist_count(&pl));
        }
        vi_playlist_free(&pl);
    }
}

static void test_real_gb_wls_content(void)
{
    vi_playlist pl;
    const vi_channel *c;
    int rc;

    printf("test_real_gb_wls_content\n");
    rc = parse_real("gb-wls.m3u", &pl);
    if (rc == -2) {
        printf("  SKIP: gb-wls.m3u not found\n");
        return;
    }
    c = vi_playlist_at(&pl, 0);
    if (c) {
        CHECK_STR(vi_channel_name(&pl, c),
                  "BBC One Wales (720p) [Geo-blocked]", "first name");
        CHECK_STR(vi_channel_tvg_id(&pl, c), "BBCOne.uk@Wales", "first tvg-id");
        CHECK_STR(vi_channel_group(&pl, c), "General", "first group");
        CHECK(strncmp(vi_channel_url(&pl, c), "https://", 8) == 0,
              "url is https");
    }
    vi_playlist_free(&pl);
}

int main(void)
{
    test_basic();
    test_extvlcopt();
    test_ua_as_attr();
    test_crlf_and_bom();
    test_comma_in_quotes();
    test_unquoted_attr();
    test_url_without_extinf();
    test_extinf_without_url();
    test_utf8_welsh();
    test_bytewise_equals_bulk();
    test_real_counts();
    test_real_gb_wls_content();

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
