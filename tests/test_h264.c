/*
 * Unit tests for the H.264 SPS parser. Uses a committed real SPS byte vector
 * (from the FailArmy 360p stream) and, when present, extracts+parses the SPS
 * from the real segment via the TS demuxer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vi_h264.h"
#include "vi_ts.h"
#include "vi_log.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do {                                   \
    g_checks++;                                                 \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* Real SPS NAL from the FailArmy 360p variant (avc1.4d0029). Starts at the
 * NAL header 0x67 (type 7). Codec config only, not video content. */
static const uint8_t kSps360[] = {
    0x67,0x4d,0x40,0x29,0xd9,0x00,0xa0,0x2f,0xf9,0x70,0x16,0xa0,0x20,0x20,
    0x28,0x00,0x00,0x1f,0x48,0x00,0x07,0x53,0x04,0x78,0xc1,0x92,0x40,0x00
};

static void test_known_vector(void)
{
    vi_h264_sps sps;
    printf("test_known_vector\n");
    CHECK(vi_h264_parse_sps_nal(kSps360, sizeof(kSps360), &sps) == 0,
          "parse ok");
    CHECK(sps.profile_idc == 77, "profile Main (77)");
    CHECK(sps.level_idc == 41, "level 4.1");
    CHECK(sps.width == 640, "width 640");
    CHECK(sps.height == 360, "height 360 (crop applied)");
    CHECK(sps.chroma_format_idc == 1, "4:2:0");
    CHECK(sps.frame_mbs_only_flag == 1, "frame mbs only");
    printf("  parsed: profile=%d level=%d %dx%d\n",
           sps.profile_idc, sps.level_idc, sps.width, sps.height);
}

static void test_au_finder(void)
{
    /* Wrap the SPS in a fake Annex-B AU: AUD, then SPS. */
    uint8_t au[64];
    size_t n = 0;
    vi_h264_sps sps;

    printf("test_au_finder\n");
    au[n++]=0;au[n++]=0;au[n++]=1; au[n++]=0x09; au[n++]=0x10; /* AUD */
    au[n++]=0;au[n++]=0;au[n++]=1;
    memcpy(au + n, kSps360, sizeof(kSps360)); n += sizeof(kSps360);

    CHECK(vi_h264_parse_au_sps(au, n, &sps) == 0, "found SPS in AU");
    CHECK(sps.width == 640 && sps.height == 360, "dims from AU");
}

/* --- real segment path --- */
typedef struct { uint8_t *au; size_t len; int done; } grab;
static void on_sample(void *ctx, const vi_ts_sample *s)
{
    grab *g = (grab *)ctx;
    if (!s->is_video || g->done)
        return;
    /* keep the first video AU that contains an SPS */
    {
        size_t i;
        for (i = 0; i + 4 < s->len; i++) {
            if (s->data[i]==0 && s->data[i+1]==0 && s->data[i+2]==1 &&
                (s->data[i+3]&0x1F)==7) {
                g->au = (uint8_t *)malloc(s->len);
                if (g->au) { memcpy(g->au, s->data, s->len); g->len = s->len; }
                g->done = 1;
                return;
            }
        }
    }
}

static void test_real_segment(void)
{
    const char *dir = getenv("VITAIPTV_TEST_DATA");
    char path[1024];
    FILE *f;
    uint8_t buf[65536];
    size_t n;
    vi_ts_demux d;
    grab g;
    vi_h264_sps sps;

    printf("test_real_segment\n");
    snprintf(path, sizeof(path), "%s/hls/sample_segment.ts",
             dir ? dir : "tests/data");
    f = fopen(path, "rb");
    if (!f) { printf("  SKIP: sample_segment.ts not found\n"); return; }

    memset(&g, 0, sizeof(g));
    vi_ts_init(&d, on_sample, &g);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        vi_ts_feed(&d, buf, n);
    vi_ts_flush(&d);
    fclose(f);

    CHECK(g.au != NULL, "captured an SPS-bearing AU");
    if (g.au) {
        CHECK(vi_h264_parse_au_sps(g.au, g.len, &sps) == 0, "parsed real SPS");
        CHECK(sps.width == 640 && sps.height == 360, "real dims 640x360");
        CHECK(sps.profile_idc == 77 && sps.level_idc == 41, "real prof/level");
        printf("  real SPS: profile=%d level=%d %dx%d\n",
               sps.profile_idc, sps.level_idc, sps.width, sps.height);
        free(g.au);
    }
    vi_ts_free(&d);
}

int main(void)
{
    vi_log_init(VI_LOG_WARN);
    test_known_vector();
    test_au_finder();
    test_real_segment();
    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
