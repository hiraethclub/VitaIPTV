/*
 * VitaIPTV - minimal H.264 SPS parsing (core/, C99, portable).
 *
 * Just enough of the bitstream to answer two questions before we start the
 * hardware decoder:
 *   - Is this stream within what the Vita's H.264 decoder can handle? (profile,
 *     level, resolution)
 *   - What are the coded dimensions, to size the decoder and the output?
 *
 * Handles Exp-Golomb, emulation-prevention bytes, high-profile chroma/scaling
 * fields, and frame cropping.
 */
#ifndef VI_H264_H
#define VI_H264_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int profile_idc;
    int level_idc;          /* e.g. 41 == level 4.1 */
    int constraint_flags;
    int chroma_format_idc;  /* 1 == 4:2:0 */
    int frame_mbs_only_flag;
    int mb_width;           /* in macroblocks */
    int mb_height;
    int width;              /* cropped luma width in pixels */
    int height;             /* cropped luma height in pixels */
} vi_h264_sps;

/*
 * Parse an SPS NAL. `nal` starts at the NAL header byte (its type must be 7).
 * Returns 0 on success, -1 if it is not an SPS or the bitstream is truncated.
 */
int vi_h264_parse_sps_nal(const uint8_t *nal, size_t len, vi_h264_sps *out);

/*
 * Find the first SPS (NAL type 7) in an Annex-B access unit and parse it.
 * Returns 0 on success, -1 if no SPS is found / parse fails.
 */
int vi_h264_parse_au_sps(const uint8_t *au, size_t len, vi_h264_sps *out);

#ifdef __cplusplus
}
#endif

#endif /* VI_H264_H */
