/*
 * VitaIPTV - M0 skeleton (vita/).
 *
 * Minimal on-device app: brings up vita2d, draws status text on the OLED, and
 * proves the portable core/ M3U parser compiles and runs under the arm-vita
 * toolchain by parsing a small embedded sample and showing the result. Exits
 * cleanly on START.
 *
 * The SceAvPlayer playback smoke test is layered on top of this in the next
 * step; kept out here so M0 is a clean, buildable baseline.
 */
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <vita2d.h>

#include <string.h>

#include "vi_m3u.h"
#include "vi_playlist.h"

/* Small embedded playlist so the core parser can be exercised on device
 * without depending on filesystem path semantics. Includes a Welsh name to
 * eyeball the system font's Latin Extended coverage (Sŵn = "Swn"). */
static const char kSample[] =
    "#EXTM3U\r\n"
    "#EXTINF:-1 tvg-id=\"BBCOne.uk@Wales\" group-title=\"General\","
    "BBC One Wales\r\n"
    "https://example.com/one.m3u8\r\n"
    "#EXTINF:-1 group-title=\"Cymraeg\",S\xC5\xB5n a Ll\xC5\xB7n\r\n"
    "https://example.com/swn.m3u8\r\n";

#define COLOR_BG    RGBA8(0x10, 0x12, 0x16, 0xFF)
#define COLOR_TITLE RGBA8(0x7A, 0xC0, 0xFF, 0xFF)
#define COLOR_TEXT  RGBA8(0xE6, 0xE6, 0xE6, 0xFF)
#define COLOR_DIM   RGBA8(0x8A, 0x90, 0x99, 0xFF)

int main(void)
{
    vita2d_pvf *font;
    vi_playlist pl;
    const vi_channel *first;
    const char *first_name;
    size_t count;
    SceCtrlData pad;
    SceCtrlData prev;

    /* Parse the embedded sample with the portable core. */
    vi_playlist_init(&pl);
    vi_m3u_parse_buffer(kSample, sizeof(kSample) - 1, &pl);
    count = vi_playlist_count(&pl);
    first = vi_playlist_at(&pl, 0);
    first_name = first ? vi_channel_name(&pl, first) : "(none)";

    vita2d_init();
    vita2d_set_clear_color(COLOR_BG);
    font = vita2d_load_default_pvf();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    memset(&prev, 0, sizeof(prev));

    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);

        /* Exit on a fresh START press. */
        if ((pad.buttons & SCE_CTRL_START) &&
            !(prev.buttons & SCE_CTRL_START))
            break;
        prev = pad;

        vita2d_start_drawing();
        vita2d_clear_screen();

        vita2d_pvf_draw_text(font, 40, 60, COLOR_TITLE, 1.4f, "VitaIPTV");
        vita2d_pvf_draw_text(font, 40, 110, COLOR_TEXT, 1.0f,
                             "M0 skeleton: vita2d up, core parser linked.");
        vita2d_pvf_draw_textf(font, 40, 150, COLOR_TEXT, 1.0f,
                              "core parsed embedded sample: %u channels",
                              (unsigned)count);
        vita2d_pvf_draw_textf(font, 40, 190, COLOR_TEXT, 1.0f,
                              "first channel: %s", first_name);
        vita2d_pvf_draw_text(font, 40, 230, COLOR_DIM, 1.0f,
                             "Welsh font check: S\xC5\xB5n a Ll\xC5\xB7n");

        vita2d_pvf_draw_text(font, 40, 500, COLOR_DIM, 1.0f,
                             "Press START to exit.");

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    vita2d_fini();
    vita2d_free_pvf(font);
    vi_playlist_free(&pl);

    sceKernelExitProcess(0);
    return 0;
}
