/*
 * VitaIPTV - M2 playback spike: SceAvPlayer HLS smoke test.
 *
 * Purpose: find out, on real hardware, whether Sony's SceAvPlayer will accept a
 * live H.264/AAC HLS URL and produce decoded frames. This is deliberately a
 * throwaway experiment (the agreed real pipeline is a custom fetch + TS/HLS
 * demux + SceVideodec path); it exists to learn how SceAvPlayer behaves with
 * live input before committing to it or ruling it out.
 *
 * IMPORTANT - this code compiles in CI but its runtime behaviour is UNVERIFIED:
 * there is no Vita in the build environment. Every SCE call's result is shown
 * on screen so it can be reported back. Assumptions that need hardware
 * confirmation are marked "ASSUMPTION" below and in docs/NOTES.md.
 *
 * All API facts (function signatures, struct layouts, sysmodule ids) are from
 * the installed VitaSDK headers: psp2/avplayer.h, psp2/net/net.h,
 * psp2/net/netctl.h, psp2/sysmodule.h.
 *
 * Test stream (verified live, H.264 Main/AAC-LC, 360p/540p/720p, no special
 * User-Agent, on 2026-09-22):
 *   FailArmy UK: https://failarmy-international-gb.samsung.wurl.tv/playlist.m3u8
 * Alternates (also H.264 HLS, include a 1080p variant for ceiling testing):
 *   CNBC UK: https://amg01079-nbcuuk-amg01079c2-samsung-gb-1258.playouts.now.amagi.tv/playlist.m3u8
 *   GB News: https://amg01076-lightningintern-gbnewsau-samsungau-et7fz.amagi.tv/playlist/amg01076-lightningintern-gbnewsau-samsungau/playlist.m3u8
 */
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/avplayer.h>

#include <vita2d.h>

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#define STREAM_URL \
    "https://failarmy-international-gb.samsung.wurl.tv/playlist.m3u8"

#define SCREEN_W 960
#define SCREEN_H 544

#define COLOR_BG    RGBA8(0x10, 0x12, 0x16, 0xFF)
#define COLOR_TITLE RGBA8(0x7A, 0xC0, 0xFF, 0xFF)
#define COLOR_TEXT  RGBA8(0xE6, 0xE6, 0xE6, 0xFF)
#define COLOR_DIM   RGBA8(0x8A, 0x90, 0x99, 0xFF)
#define COLOR_OK    RGBA8(0x7A, 0xD8, 0x8A, 0xFF)
#define COLOR_ERR   RGBA8(0xFF, 0x8A, 0x7A, 0xFF)

/* ---- SceAvPlayer memory callbacks ---------------------------------------
 * ASSUMPTION: plain aligned main-RAM allocations satisfy SceAvPlayer on Vita
 * (this is how several open-source Vita ports drive it). To confirm on device.
 */
static void *cb_alloc(void *arg, uint32_t alignment, uint32_t size)
{
    (void)arg;
    return memalign(alignment, size);
}
static void cb_free(void *arg, void *ptr)
{
    (void)arg;
    free(ptr);
}

/* ---- event callback: capture the most recent event for the HUD --------- */
static volatile int32_t g_last_event = -999;
static volatile int32_t g_last_event_src = -999;
static void cb_event(void *p, int32_t eventId, int32_t sourceId, void *data)
{
    (void)p;
    (void)data;
    g_last_event = eventId;
    g_last_event_src = sourceId;
}

/* ---- networking ---------------------------------------------------------- */
#define NET_POOL_SIZE (1 * 1024 * 1024)
static char *g_net_pool;

/* Records every init step's result so the HUD can show what happened. */
typedef struct {
    int mod_net, mod_http, mod_ssl, mod_https, mod_avplayer;
    int net_init, netctl_init;
    int player_handle;
    int add_source;
    int start;
} init_status;

static int load_mod(int id)
{
    int rc = sceSysmoduleLoadModule((SceSysmoduleModuleId)id);
    return rc;
}

static void init_all(init_status *s)
{
    SceNetInitParam np;

    memset(s, 0, sizeof(*s));

    s->mod_net      = load_mod(SCE_SYSMODULE_NET);
    s->mod_http     = load_mod(SCE_SYSMODULE_HTTP);
    s->mod_ssl      = load_mod(SCE_SYSMODULE_SSL);
    s->mod_https    = load_mod(SCE_SYSMODULE_HTTPS);
    s->mod_avplayer = load_mod(SCE_SYSMODULE_AVPLAYER);

    g_net_pool = (char *)malloc(NET_POOL_SIZE);
    np.memory = g_net_pool;
    np.size = NET_POOL_SIZE;
    np.flags = 0;
    s->net_init = sceNetInit(&np);      /* may already be up: non-fatal */
    s->netctl_init = sceNetCtlInit();
}

/*
 * sceAvPlayerInit returns an opaque handle that on Vita is a heap pointer, so
 * it can have bit 31 set and read as "negative" though it is perfectly valid.
 * Do NOT treat a negative handle as failure (an early hardware run showed
 * handle=0x81348FC0, a real pointer, being misread as an error). Only reject a
 * null handle or the documented SceAvPlayer error page (0x806A00xx).
 */
static int handle_is_valid(SceAvPlayerHandle h)
{
    uint32_t u = (uint32_t)h;
    if (h == 0)
        return 0;
    if ((u & 0xFFFFFF00u) == 0x806A0000u)
        return 0;
    return 1;
}

static SceAvPlayerHandle start_player(init_status *s)
{
    SceAvPlayerInitData data;
    SceAvPlayerHandle h;

    memset(&data, 0, sizeof(data));
    data.memoryReplacement.allocate = cb_alloc;
    data.memoryReplacement.deallocate = cb_free;
    data.memoryReplacement.allocateTexture = cb_alloc;
    data.memoryReplacement.deallocateTexture = cb_free;
    data.eventReplacement.eventCallback = cb_event;
    data.debugLevel = 0;
    data.basePriority = 0xA0;           /* ASSUMPTION: reasonable base prio */
    data.numOutputVideoFrameBuffers = 2;
    data.autoStart = 0;                 /* start explicitly below */
    data.defaultLanguage = 0;

    h = sceAvPlayerInit(&data);
    s->player_handle = h;
    if (!handle_is_valid(h))
        return h;

    s->add_source = sceAvPlayerAddSource(h, STREAM_URL);
    s->start = sceAvPlayerStart(h);
    return h;
}

/* ---- grayscale luma preview --------------------------------------------
 * Show the Y plane as grayscale. Y is plane 0 in every YUV420 layout, so this
 * is format-robust and proves frames are really decoding, without needing the
 * exact chroma format (which we cannot verify from here).
 * ASSUMPTION: source luma row stride == frame width. If the image looks
 * sheared/skewed on device, the true stride differs and must be read.
 */
static vita2d_texture *g_tex;
static int g_tex_w, g_tex_h;

static void upload_luma(const uint8_t *y, int w, int h)
{
    uint32_t *dst;
    unsigned dst_stride_px;
    int i, j;

    if (y == 0 || w <= 0 || h <= 0 || w > 1920 || h > 1088)
        return;

    if (g_tex == 0 || g_tex_w != w || g_tex_h != h) {
        if (g_tex)
            vita2d_free_texture(g_tex);
        g_tex = vita2d_create_empty_texture(w, h);
        g_tex_w = w;
        g_tex_h = h;
    }
    if (g_tex == 0)
        return;

    dst = (uint32_t *)vita2d_texture_get_datap(g_tex);
    dst_stride_px = vita2d_texture_get_stride(g_tex) / 4;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint8_t v = y[j * w + i];
            dst[j * dst_stride_px + i] =
                0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        }
    }
}

static void draw_video(void)
{
    float sx, sy, scale, ox, oy;
    if (g_tex == 0 || g_tex_w == 0 || g_tex_h == 0)
        return;
    sx = (float)SCREEN_W / (float)g_tex_w;
    sy = (float)SCREEN_H / (float)g_tex_h;
    scale = sx < sy ? sx : sy;
    ox = (SCREEN_W - g_tex_w * scale) * 0.5f;
    oy = (SCREEN_H - g_tex_h * scale) * 0.5f;
    vita2d_draw_texture_scale(g_tex, ox, oy, scale, scale);
}

int main(void)
{
    vita2d_pvf *font;
    init_status st;
    SceAvPlayerHandle player;
    SceCtrlData pad, prev;
    unsigned long video_frames = 0, audio_frames = 0;
    int last_w = 0, last_h = 0, last_ach = 0, last_arate = 0;

    vita2d_init();
    vita2d_set_clear_color(COLOR_BG);
    font = vita2d_load_default_pvf();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    memset(&prev, 0, sizeof(prev));

    init_all(&st);
    player = start_player(&st);

    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_START) &&
            !(prev.buttons & SCE_CTRL_START))
            break;
        prev = pad;

        if (handle_is_valid(player) && sceAvPlayerIsActive(player)) {
            SceAvPlayerFrameInfo vf, af;

            if (sceAvPlayerGetVideoData(player, &vf)) {
                last_w = (int)vf.details.video.width;
                last_h = (int)vf.details.video.height;
                video_frames++;
                upload_luma((const uint8_t *)vf.pData, last_w, last_h);
            }
            /* Drain audio so the pipeline does not stall (not output yet). */
            if (sceAvPlayerGetAudioData(player, &af)) {
                last_ach = (int)af.details.audio.channelCount;
                last_arate = (int)af.details.audio.sampleRate;
                audio_frames++;
            }
        }

        vita2d_start_drawing();
        vita2d_clear_screen();

        draw_video();

        /* Diagnostic HUD (always on top). */
        vita2d_pvf_draw_text(font, 20, 30, COLOR_TITLE, 1.0f,
                             "VitaIPTV - SceAvPlayer HLS smoke test");
        vita2d_pvf_draw_textf(font, 20, 60, COLOR_DIM, 0.9f,
                              "url: %s", STREAM_URL);

        vita2d_pvf_draw_textf(font, 20, 92, COLOR_TEXT, 0.9f,
            "sysmod net=%d http=%d ssl=%d https=%d avp=%d",
            st.mod_net, st.mod_http, st.mod_ssl, st.mod_https,
            st.mod_avplayer);
        vita2d_pvf_draw_textf(font, 20, 116, COLOR_TEXT, 0.9f,
            "netInit=%d netCtlInit=%d", st.net_init, st.netctl_init);
        vita2d_pvf_draw_textf(font, 20, 140,
            handle_is_valid(st.player_handle) ? COLOR_OK : COLOR_ERR, 0.9f,
            "avPlayerInit handle=0x%08X (%s)  addSource=%d  start=%d",
            (unsigned)st.player_handle,
            handle_is_valid(st.player_handle) ? "valid" : "error",
            st.add_source, st.start);

        if (handle_is_valid(player)) {
            vita2d_pvf_draw_textf(font, 20, 172, COLOR_TEXT, 0.9f,
                "active=%d  currentTime=%llu ms",
                (int)sceAvPlayerIsActive(player),
                (unsigned long long)sceAvPlayerCurrentTime(player));
            vita2d_pvf_draw_textf(font, 20, 196,
                video_frames ? COLOR_OK : COLOR_TEXT, 0.9f,
                "video frames=%lu  last=%dx%d", video_frames, last_w, last_h);
            vita2d_pvf_draw_textf(font, 20, 220, COLOR_TEXT, 0.9f,
                "audio frames=%lu  ch=%d  rate=%d",
                audio_frames, last_ach, last_arate);
            vita2d_pvf_draw_textf(font, 20, 244, COLOR_DIM, 0.9f,
                "last event id=%d src=%d",
                (int)g_last_event, (int)g_last_event_src);
        }

        vita2d_pvf_draw_text(font, 20, SCREEN_H - 20, COLOR_DIM, 0.9f,
            "Press START to exit. (video shown as grayscale luma)");

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    if (handle_is_valid(player)) {
        sceAvPlayerStop(player);
        sceAvPlayerClose(player);
    }
    if (g_tex)
        vita2d_free_texture(g_tex);
    vita2d_free_pvf(font);
    vita2d_fini();
    free(g_net_pool);

    sceKernelExitProcess(0);
    return 0;
}
