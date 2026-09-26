/*
 * VitaIPTV - hardware H.264 decoder wrapper (vita/, device-only).
 *
 * Wraps SceVideodec/sceAvcdec: feed Annex-B access units, get decoded RGBA
 * frames rendered into a vita2d texture. Double-buffered so the render thread
 * can draw the front texture while the worker decodes into the back one.
 *
 * The SceVideodec flow (init dims rounded to 16, PHYCONT_NC_RW frame buffer,
 * per-frame SceAvcdecArrayPicture with pPicture[0] pointed at the texture,
 * pixelType RGBA8888) follows the Moonlight Vita client (GPL-3.0), verified
 * against psp2/videodec.h. See docs/NOTES.md.
 */
#ifndef VI_VDEC_H
#define VI_VDEC_H

#include <stddef.h>
#include <stdint.h>

#include <vita2d.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vi_vdec vi_vdec;

/* Create a decoder for the given coded picture size (from the SPS). Returns a
 * handle or NULL on failure (details logged). */
vi_vdec *vi_vdec_create(int width, int height);

/* Decode one Annex-B access unit. Returns 1 if a frame was produced (and the
 * front texture updated), 0 if not, <0 on a decode error. */
int vi_vdec_decode(vi_vdec *v, const uint8_t *au, size_t len);

/* Current display texture (front). May be NULL before the first frame. The
 * caller draws only the [0,0,width,height] region (coded size is 16-aligned
 * and may be larger). Safe to call from the render thread. */
vita2d_texture *vi_vdec_front(vi_vdec *v);
int vi_vdec_width(const vi_vdec *v);
int vi_vdec_height(const vi_vdec *v);
unsigned long vi_vdec_frame_count(const vi_vdec *v);

void vi_vdec_destroy(vi_vdec *v);

#ifdef __cplusplus
}
#endif

#endif /* VI_VDEC_H */
