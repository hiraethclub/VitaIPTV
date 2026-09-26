/*
 * VitaIPTV - custom HLS playback pipeline (M2/M3 device build).
 *
 * Pipeline: SceHttp fetch -> HLS master/variant/media parse (core) -> segment
 * fetch -> MPEG-TS demux (core) -> H.264 SPS parse (core) -> SceVideodec decode
 * -> vita2d render. Networking/demux/decode run on a worker thread; the main
 * thread renders the video and an on-screen log/stat HUD.
 *
 * This is device-only and UNVERIFIED in CI (no Vita here); every stage logs via
 * vi_log so a single hardware run shows exactly how far it gets. Audio is
 * demuxed and counted but not yet played (next step).
 *
 * SceVideodec usage follows the Moonlight Vita client (GPL-3.0); see NOTES.
 */
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>

#include <vita2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vi_log.h"
#include "vi_hls.h"
#include "vi_ts.h"
#include "vi_h264.h"
#include "vi_http.h"
#include "vi_vdec.h"

#define MASTER_URL \
    "https://failarmy-international-gb.samsung.wurl.tv/playlist.m3u8"

#define CEIL_W 1280
#define CEIL_H 720
#define FRAME_PACING_US 30000  /* ~33 fps playback pacing */

#define SCREEN_W 960
#define SCREEN_H 544
#define NET_POOL (512 * 1024)

#define COL_TITLE RGBA8(0x7A,0xC0,0xFF,0xFF)
#define COL_TEXT  RGBA8(0xE6,0xE6,0xE6,0xFF)
#define COL_DIM   RGBA8(0x9A,0xA0,0xA9,0xFF)
#define COL_ERR   RGBA8(0xFF,0x8A,0x7A,0xFF)
#define COL_OK    RGBA8(0x7A,0xD8,0x8A,0xFF)

/* ---- shared state ---- */
static volatile int g_running = 1;
static SceUID g_app_mutex = -1;

static char g_state[64] = "starting";
static int  g_sel_w, g_sel_h;
static volatile unsigned long g_segs, g_http_bytes_k;
static volatile unsigned long g_video_aus, g_audio_frames, g_dec_frames;

/* decoder handshake: worker requests, main thread creates (GXM ownership) */
static volatile int g_req_w, g_req_h, g_decoder_requested, g_decoder_failed;
static vi_vdec *g_vdec;               /* set by main thread */

static vi_ts_demux g_demux;
static char *g_net_pool;

static void set_state(const char *s)
{
    sceKernelLockMutex(g_app_mutex, 1, NULL);
    strncpy(g_state, s, sizeof(g_state) - 1);
    g_state[sizeof(g_state) - 1] = '\0';
    sceKernelUnlockMutex(g_app_mutex, 1);
}

/* ---- vi_log lock hooks + optional UDP sink ---- */
static SceUID g_log_mutex = -1;
static void log_lock(void *c)   { (void)c; sceKernelLockMutex(g_log_mutex, 1, NULL); }
static void log_unlock(void *c) { (void)c; sceKernelUnlockMutex(g_log_mutex, 1); }

static int g_log_sock = -1;
static SceNetSockaddrIn g_log_addr;
static void net_log_sink(void *ctx, vi_log_level lvl, const char *line)
{
    char b[256];
    int n;
    (void)ctx; (void)lvl;
    if (g_log_sock < 0)
        return;
    n = snprintf(b, sizeof(b), "%s\n", line);
    sceNetSendto(g_log_sock, b, (unsigned)n, 0,
                 (SceNetSockaddr *)&g_log_addr, sizeof(g_log_addr));
}

/* Read ux0:data/vitaiptv/pc_ip.txt ("A.B.C.D" or "A.B.C.D:port") and, if
 * present, mirror logs to that PC over UDP (read with tools/deploy.sh log). */
static void net_log_setup(void)
{
    char cfg[64];
    SceUID fd;
    int n, a, b, c, d, port = 18194;
    unsigned long ip;

    fd = sceIoOpen("ux0:data/vitaiptv/pc_ip.txt", SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    n = sceIoRead(fd, cfg, sizeof(cfg) - 1);
    sceIoClose(fd);
    if (n <= 0)
        return;
    cfg[n] = '\0';
    if (sscanf(cfg, "%d.%d.%d.%d:%d", &a, &b, &c, &d, &port) < 4)
        return;

    g_log_sock = sceNetSocket("vilog", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (g_log_sock < 0) { g_log_sock = -1; return; }
    ip = ((unsigned long)a << 24) | (b << 16) | (c << 8) | d;
    memset(&g_log_addr, 0, sizeof(g_log_addr));
    g_log_addr.sin_family = SCE_NET_AF_INET;
    g_log_addr.sin_port = sceNetHtons((unsigned short)port);
    g_log_addr.sin_addr.s_addr = sceNetHtonl(ip);
    vi_log_set_sink(net_log_sink, NULL);
    VI_LOGI("app", "net log -> %d.%d.%d.%d:%d", a, b, c, d, port);
}

/* ---- demux callback: decode video, count audio ---- */
static void on_ts_sample(void *ctx, const vi_ts_sample *s)
{
    (void)ctx;
    if (!g_running)
        return;

    if (!s->is_video) {
        g_audio_frames++;
        return;
    }
    g_video_aus++;

    if (g_vdec == NULL) {
        /* need a decoder: parse SPS and ask the main thread to create it */
        vi_h264_sps sps;
        if (g_decoder_failed)
            return;
        if (!g_decoder_requested) {
            if (vi_h264_parse_au_sps(s->data, s->len, &sps) != 0)
                return; /* wait for a keyframe carrying an SPS */
            VI_LOGI("app", "SPS %dx%d profile %d level %d",
                    sps.width, sps.height, sps.profile_idc, sps.level_idc);
            if (sps.width > CEIL_W || sps.height > CEIL_H) {
                VI_LOGE("app", "stream %dx%d exceeds ceiling %dx%d",
                        sps.width, sps.height, CEIL_W, CEIL_H);
                set_state("stream exceeds decoder ceiling");
                g_decoder_failed = 1;
                return;
            }
            g_req_w = sps.width;
            g_req_h = sps.height;
            g_decoder_requested = 1;
            set_state("creating decoder");
        }
        /* wait (briefly) for the main thread to bring the decoder up */
        {
            int spins = 0;
            while (g_vdec == NULL && !g_decoder_failed && g_running &&
                   spins < 600) {
                sceKernelDelayThread(10000);
                spins++;
            }
        }
        if (g_vdec == NULL)
            return;
        set_state("playing");
    }

    {
        int r = vi_vdec_decode(g_vdec, s->data, s->len);
        if (r == 1) {
            g_dec_frames++;
            sceKernelDelayThread(FRAME_PACING_US);
        }
    }
}

/* ---- worker: fetch + demux the live stream ---- */
static int worker_thread(SceSize argsz, void *argp)
{
    uint8_t *mb;
    size_t mn;
    int st;
    vi_hls_master master;
    const vi_hls_variant *v;
    char media_url[1024];
    long long last_seq = -1;

    (void)argsz; (void)argp;

    set_state("fetching master");
    VI_LOGI("app", "GET master");
    mb = vi_http_get(MASTER_URL, &mn, &st);
    if (!mb) { set_state("master fetch failed"); return 0; }
    if (vi_hls_parse_master((char *)mb, mn, &master) != 0) {
        set_state("master parse OOM"); free(mb); return 0;
    }
    free(mb);
    VI_LOGI("app", "master: %u variants", (unsigned)master.count);

    v = vi_hls_select_h264(&master, CEIL_W, CEIL_H);
    if (!v) { set_state("no H.264 variant"); vi_hls_master_free(&master); return 0; }
    g_sel_w = v->width; g_sel_h = v->height;
    vi_url_resolve(MASTER_URL, v->uri, media_url, sizeof(media_url));
    VI_LOGI("app", "variant %dx%d -> %.80s", v->width, v->height, media_url);
    vi_hls_master_free(&master);

    while (g_running) {
        uint8_t *mp;
        size_t mpn;
        vi_hls_media md;
        size_t i;
        int td;

        set_state(g_vdec ? "playing" : "fetching media playlist");
        mp = vi_http_get(media_url, &mpn, &st);
        if (!mp) { VI_LOGW("app", "media fetch failed"); sceKernelDelayThread(2000000); continue; }
        if (vi_hls_parse_media((char *)mp, mpn, &md) != 0) { free(mp); continue; }
        free(mp);
        if (md.encrypted) {
            VI_LOGE("app", "segments are AES encrypted; not supported");
            set_state("encrypted stream unsupported");
            vi_hls_media_free(&md);
            break;
        }

        for (i = 0; i < md.count && g_running; i++) {
            char seg_url[1024];
            uint8_t *sb;
            size_t sn;
            int sst;
            if ((long long)md.segments[i].seq <= last_seq)
                continue;
            vi_url_resolve(media_url, md.segments[i].uri, seg_url, sizeof(seg_url));
            sb = vi_http_get(seg_url, &sn, &sst);
            if (!sb) { VI_LOGW("app", "segment fetch failed"); continue; }
            g_segs++;
            g_http_bytes_k += (unsigned long)(sn / 1024);
            vi_ts_feed(&g_demux, sb, sn);
            vi_ts_flush(&g_demux);
            free(sb);
            last_seq = md.segments[i].seq;
        }
        td = md.target_duration;
        if (md.endlist) { vi_hls_media_free(&md); break; }
        vi_hls_media_free(&md);
        sceKernelDelayThread((SceUInt)((td > 0 ? td : 4) * 500000)); /* td/2 s */
    }
    set_state("stopped");
    return 0;
}

/* ---- networking bring-up ---- */
static int net_up(void)
{
    SceNetInitParam np;
    int r;

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
    sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);

    g_net_pool = (char *)malloc(NET_POOL);
    np.memory = g_net_pool;
    np.size = NET_POOL;
    np.flags = 0;
    r = sceNetInit(&np);
    if (r < 0 && r != (int)0x80410103 /* already inited */)
        VI_LOGW("app", "sceNetInit 0x%08X", r);
    sceNetCtlInit();
    return 0;
}

/* ---- HUD ---- */
static void draw_hud(vita2d_pvf *font)
{
    char line[256];
    vi_log_level lvl;
    size_t total, i, start, shown;
    float y;

    sceKernelLockMutex(g_app_mutex, 1, NULL);
    snprintf(line, sizeof(line), "state: %s", g_state);
    sceKernelUnlockMutex(g_app_mutex, 1);

    vita2d_pvf_draw_text(font, 12, 24, COL_TITLE, 1.0f, "VitaIPTV - HLS pipeline");
    vita2d_pvf_draw_text(font, 12, 48, COL_TEXT, 0.9f, line);
    vita2d_pvf_draw_textf(font, 12, 70, COL_TEXT, 0.9f,
        "variant %dx%d  segs %lu  %lu KiB  vAU %lu  aFR %lu  dec %lu",
        g_sel_w, g_sel_h, g_segs, g_http_bytes_k,
        g_video_aus, g_audio_frames, g_dec_frames);

    /* last log lines */
    total = vi_log_ring_count();
    shown = 12;
    start = total > shown ? total - shown : 0;
    y = 96;
    for (i = start; i < total; i++) {
        unsigned int col = COL_DIM;
        if (vi_log_ring_copy(i, line, sizeof(line), &lvl)) {
            if (lvl == VI_LOG_ERROR) col = COL_ERR;
            else if (lvl == VI_LOG_WARN) col = RGBA8(0xFF,0xD0,0x7A,0xFF);
            vita2d_pvf_draw_text(font, 12, y, col, 0.8f, line);
            y += 18;
        }
    }
    vita2d_pvf_draw_text(font, 12, SCREEN_H - 14, COL_DIM, 0.8f,
        "START exit   TRIANGLE toggle HUD");
}

int main(void)
{
    vita2d_pvf *font;
    SceCtrlData pad, prev;
    SceUID worker;
    int hud = 1;

    g_app_mutex = sceKernelCreateMutex("app", 0, 0, NULL);
    g_log_mutex = sceKernelCreateMutex("log", 0, 0, NULL);
    vi_log_init(VI_LOG_DEBUG);
    vi_log_set_lock(log_lock, log_unlock, NULL);
    VI_LOGI("app", "VitaIPTV starting");

    vita2d_init();
    vita2d_set_clear_color(RGBA8(0x08,0x0A,0x0E,0xFF));
    font = vita2d_load_default_pvf();

    net_up();
    net_log_setup();
    if (vi_http_init() != 0)
        set_state("http init failed");

    vi_ts_init(&g_demux, on_ts_sample, NULL);

    worker = sceKernelCreateThread("vi_worker", worker_thread,
                                   0x10000100, 0x40000, 0, 0, NULL);
    if (worker >= 0)
        sceKernelStartThread(worker, 0, NULL);
    else
        set_state("worker create failed");

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    memset(&prev, 0, sizeof(prev));

    while (g_running) {
        vita2d_texture *frame;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_START) && !(prev.buttons & SCE_CTRL_START))
            break;
        if ((pad.buttons & SCE_CTRL_TRIANGLE) && !(prev.buttons & SCE_CTRL_TRIANGLE))
            hud = !hud;
        prev = pad;

        /* fulfil a decoder-creation request on this (GXM-owning) thread */
        if (g_decoder_requested && g_vdec == NULL && !g_decoder_failed) {
            vi_vdec *d = vi_vdec_create(g_req_w, g_req_h);
            if (d) g_vdec = d;
            else { g_decoder_failed = 1; set_state("decoder init failed"); }
        }

        vita2d_start_drawing();
        vita2d_clear_screen();

        frame = vi_vdec_front(g_vdec);
        if (frame) {
            int w = vi_vdec_width(g_vdec), h = vi_vdec_height(g_vdec);
            float sx = (float)SCREEN_W / w, sy = (float)SCREEN_H / h;
            float s = sx < sy ? sx : sy;
            float ox = (SCREEN_W - w * s) * 0.5f;
            float oy = (SCREEN_H - h * s) * 0.5f;
            vita2d_draw_texture_part_scale(frame, ox, oy, 0, 0,
                                           (float)w, (float)h, s, s);
        }
        if (hud || !frame)
            draw_hud(font);

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    /* The worker may be blocked in a network read; do not wait on it (that
     * could hang exit). Signal stop and let the process teardown reclaim
     * everything - avoids racing the worker's decode against vita2d_fini. */
    g_running = 0;
    (void)worker;
    sceKernelExitProcess(0);
    return 0;
}
