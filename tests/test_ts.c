/*
 * Unit tests for the MPEG-TS demuxer. Synthetic hand-crafted packets exercise
 * PAT/PMT discovery, PES reassembly and PTS extraction deterministically; an
 * optional real FailArmy segment (tests/data/hls/sample_segment.ts, fetched by
 * tools/fetch-playlists.sh) checks behaviour on real data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vi_ts.h"
#include "vi_log.h"

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, msg) do {                                   \
    g_checks++;                                                 \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ---- synthetic TS builder ---- */

static void put_pts(uint8_t *b, int64_t pts, int guard)
{
    b[0] = (uint8_t)((guard << 4) | (((pts >> 30) & 0x07) << 1) | 1);
    b[1] = (uint8_t)((pts >> 22) & 0xFF);
    b[2] = (uint8_t)((((pts >> 15) & 0x7F) << 1) | 1);
    b[3] = (uint8_t)((pts >> 7) & 0xFF);
    b[4] = (uint8_t)(((pts & 0x7F) << 1) | 1);
}

/* Write one 188-byte packet: header + payload (payload padded/truncated). */
static void make_packet(uint8_t *pkt, int pusi, int pid, int cc,
                        const uint8_t *payload, size_t plen)
{
    size_t space = VI_TS_PACKET_SIZE - 4;
    memset(pkt, 0xFF, VI_TS_PACKET_SIZE); /* filler */
    pkt[0] = 0x47;
    pkt[1] = (uint8_t)((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    pkt[2] = (uint8_t)(pid & 0xFF);
    pkt[3] = (uint8_t)(0x10 | (cc & 0x0F)); /* payload only */
    if (plen > space)
        plen = space;
    memcpy(pkt + 4, payload, plen);
}

/* Build a video/audio PES payload with a PTS and given ES bytes. */
static size_t make_pes(uint8_t *out, int stream_id, int64_t pts,
                       const uint8_t *es, size_t es_len)
{
    size_t n = 0;
    out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x01;
    out[n++] = (uint8_t)stream_id;
    out[n++] = 0x00; out[n++] = 0x00;   /* PES_packet_length = 0 (unbounded) */
    out[n++] = 0x80;                    /* '10' marker */
    out[n++] = 0x80;                    /* PTS_DTS_flags = 10 (PTS only) */
    out[n++] = 0x05;                    /* PES_header_data_length */
    put_pts(out + n, pts, 0x02); n += 5;
    memcpy(out + n, es, es_len); n += es_len;
    return n;
}

static uint8_t g_pat[188], g_pmt[188];

static void build_psi(void)
{
    /* PAT: program 1 -> pmt_pid 32 */
    uint8_t sec[32];
    size_t n = 0;
    uint8_t pay[184];
    size_t pn;

    sec[0] = 0x00;                 /* table_id */
    sec[1] = 0xB0;                 /* ssi + reserved + len hi */
    sec[2] = 0x0D;                 /* section_length = 13 */
    sec[3] = 0x00; sec[4] = 0x01;  /* tsid */
    sec[5] = 0xC1;                 /* version/current */
    sec[6] = 0x00; sec[7] = 0x00;  /* section numbers */
    sec[8] = 0x00; sec[9] = 0x01;  /* program 1 */
    sec[10] = (uint8_t)(0xE0 | (32 >> 8)); sec[11] = 32 & 0xFF; /* pmt_pid 32 */
    sec[12] = sec[13] = sec[14] = sec[15] = 0x00; /* CRC (ignored) */
    n = 16;
    pay[0] = 0x00;                 /* pointer_field */
    memcpy(pay + 1, sec, n); pn = n + 1;
    make_packet(g_pat, 1, 0x0000, 0, pay, pn);

    /* PMT: H.264 pid 0x100, AAC pid 0x101 */
    {
        uint8_t p[40];
        p[0] = 0x02; p[1] = 0xB0; p[2] = 0x17;           /* len = 23 */
        p[3] = 0x00; p[4] = 0x01;                        /* program 1 */
        p[5] = 0xC1; p[6] = 0x00; p[7] = 0x00;
        p[8] = (uint8_t)(0xE0 | (0x100 >> 8)); p[9] = 0x100 & 0xFF; /* pcr */
        p[10] = 0xF0; p[11] = 0x00;                      /* prog_info_len 0 */
        p[12] = 0x1B; p[13] = (uint8_t)(0xE0 | (0x100 >> 8)); p[14] = 0x00;
        p[15] = 0xF0; p[16] = 0x00;
        p[17] = 0x0F; p[18] = (uint8_t)(0xE0 | (0x101 >> 8)); p[19] = 0x01;
        p[20] = 0xF0; p[21] = 0x00;
        p[22] = p[23] = p[24] = p[25] = 0x00;            /* CRC */
        pay[0] = 0x00;
        memcpy(pay + 1, p, 26);
        make_packet(g_pmt, 1, 32, 0, pay, 27);
    }
}

/* ---- callback capture ---- */

typedef struct {
    int v_count, a_count;
    int64_t v_pts[8], a_pts[8];
    uint8_t first_v[8];
    size_t  first_v_len;
    int     any_sps;
} capture;

static int find_sps(const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i + 4 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if ((d[i + 3] & 0x1F) == 7) return 1;
        }
    }
    return 0;
}

static void on_sample(void *ctx, const vi_ts_sample *s)
{
    capture *c = (capture *)ctx;
    if (s->is_video) {
        if (c->v_count < 8) c->v_pts[c->v_count] = s->pts;
        if (c->v_count == 0) {
            c->first_v_len = s->len < 8 ? s->len : 8;
            memcpy(c->first_v, s->data, c->first_v_len);
        }
        if (find_sps(s->data, s->len)) c->any_sps = 1;
        c->v_count++;
    } else {
        if (c->a_count < 8) c->a_pts[c->a_count] = s->pts;
        c->a_count++;
    }
}

static void test_synthetic(void)
{
    vi_ts_demux d;
    capture cap;
    uint8_t vpes[184], apes[184], pkt[188];
    uint8_t vau[8]  = { 0x00,0x00,0x00,0x01, 0x67, 0xAA, 0xBB, 0xCC };
    uint8_t aau[6]  = { 0xFF,0xF1, 0x50, 0x80, 0x01, 0x23 };
    size_t vn, an;

    printf("test_synthetic\n");
    memset(&cap, 0, sizeof(cap));
    build_psi();
    vi_ts_init(&d, on_sample, &cap);

    vi_ts_feed(&d, g_pat, VI_TS_PACKET_SIZE);
    vi_ts_feed(&d, g_pmt, VI_TS_PACKET_SIZE);
    CHECK(d.video_pid == 0x100, "video pid discovered");
    CHECK(d.audio_pid == 0x101, "audio pid discovered");
    CHECK(d.video_stream_type == VI_TS_STREAM_H264, "video is h264");

    /* two video PES (pts 90000, 93000) and one audio PES (pts 90000) */
    vn = make_pes(vpes, 0xE0, 90000, vau, sizeof(vau));
    make_packet(pkt, 1, 0x100, 0, vpes, vn);
    vi_ts_feed(&d, pkt, VI_TS_PACKET_SIZE);

    an = make_pes(apes, 0xC0, 90000, aau, sizeof(aau));
    make_packet(pkt, 1, 0x101, 0, apes, an);
    vi_ts_feed(&d, pkt, VI_TS_PACKET_SIZE);

    vn = make_pes(vpes, 0xE0, 93000, vau, sizeof(vau));
    make_packet(pkt, 1, 0x100, 1, vpes, vn);
    vi_ts_feed(&d, pkt, VI_TS_PACKET_SIZE);

    vi_ts_flush(&d);

    CHECK(cap.v_count == 2, "two video AUs");
    CHECK(cap.a_count == 1, "one audio frame");
    CHECK(cap.v_pts[0] == 90000, "video pts 0");
    CHECK(cap.v_pts[1] == 93000, "video pts 1");
    CHECK(cap.a_pts[0] == 90000, "audio pts");
    CHECK(cap.first_v_len >= 5 &&
          cap.first_v[0] == 0 && cap.first_v[1] == 0 &&
          cap.first_v[2] == 0 && cap.first_v[3] == 1 &&
          cap.first_v[4] == 0x67, "first video AU is Annex-B SPS");
    CHECK(cap.any_sps == 1, "SPS found in video AU");

    vi_ts_free(&d);
}

static void test_chunked_feed(void)
{
    /* Feeding the same stream one byte at a time must give the same result. */
    vi_ts_demux d;
    capture cap;
    uint8_t vpes[184], pkt[188];
    uint8_t vau[8] = { 0x00,0x00,0x00,0x01, 0x67, 1, 2, 3 };
    size_t vn, i;

    printf("test_chunked_feed\n");
    memset(&cap, 0, sizeof(cap));
    build_psi();
    vi_ts_init(&d, on_sample, &cap);

    for (i = 0; i < VI_TS_PACKET_SIZE; i++) vi_ts_feed(&d, g_pat + i, 1);
    for (i = 0; i < VI_TS_PACKET_SIZE; i++) vi_ts_feed(&d, g_pmt + i, 1);
    vn = make_pes(vpes, 0xE0, 12345, vau, sizeof(vau));
    make_packet(pkt, 1, 0x100, 0, vpes, vn);
    for (i = 0; i < VI_TS_PACKET_SIZE; i++) vi_ts_feed(&d, pkt + i, 1);
    vi_ts_flush(&d);

    CHECK(d.video_pid == 0x100, "pid found via byte feed");
    CHECK(cap.v_count == 1 && cap.v_pts[0] == 12345, "one AU, pts ok");
    vi_ts_free(&d);
}

static void test_real_segment(void)
{
    const char *dir = getenv("VITAIPTV_TEST_DATA");
    char path[1024];
    FILE *f;
    uint8_t buf[65536];
    size_t n;
    vi_ts_demux d;
    capture cap;

    printf("test_real_segment\n");
    snprintf(path, sizeof(path), "%s/hls/sample_segment.ts",
             dir ? dir : "tests/data");
    f = fopen(path, "rb");
    if (!f) { printf("  SKIP: sample_segment.ts not found\n"); return; }

    memset(&cap, 0, sizeof(cap));
    vi_ts_init(&d, on_sample, &cap);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        vi_ts_feed(&d, buf, n);
    vi_ts_flush(&d);
    fclose(f);

    CHECK(d.saw_pat && d.saw_pmt, "found PAT and PMT");
    CHECK(d.video_pid == 481, "real video pid 481");
    CHECK(d.audio_pid == 482, "real audio pid 482");
    CHECK(d.video_aus > 10, "many video AUs");
    CHECK(d.audio_frames > 10, "many audio frames");
    CHECK(cap.any_sps == 1, "SPS present in real stream");
    printf("  real: %lu video AUs, %lu audio frames, first v pts=%lld\n",
           d.video_aus, d.audio_frames, (long long)cap.v_pts[0]);
    vi_ts_free(&d);
}

int main(void)
{
    vi_log_init(VI_LOG_WARN); /* keep test output quiet */
    test_synthetic();
    test_chunked_feed();
    test_real_segment();
    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
