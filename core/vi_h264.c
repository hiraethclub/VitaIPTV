#include "vi_h264.h"

#include <string.h>

/* ---- bit reader over an RBSP (emulation bytes already removed) ---------- */

typedef struct {
    const uint8_t *d;
    size_t         size;   /* bytes */
    size_t         bit;    /* current bit position */
    int            error;  /* set if a read ran past the end */
} bitr;

static uint32_t br_u(bitr *r, int n)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++) {
        size_t byte = r->bit >> 3;
        int off = 7 - (int)(r->bit & 7);
        if (byte >= r->size) {
            r->error = 1;
            return v << (n - i); /* pad */
        }
        v = (v << 1) | ((r->d[byte] >> off) & 1);
        r->bit++;
    }
    return v;
}

static int br_u1(bitr *r)
{
    return (int)br_u(r, 1);
}

static uint32_t br_ue(bitr *r)
{
    int zeros = 0;
    while (br_u1(r) == 0 && !r->error && zeros < 32)
        zeros++;
    if (zeros == 0)
        return 0;
    return ((1u << zeros) - 1u) + br_u(r, zeros);
}

static int32_t br_se(bitr *r)
{
    uint32_t k = br_ue(r);
    if (k & 1)
        return (int32_t)((k + 1) / 2);
    return -(int32_t)(k / 2);
}

static void skip_scaling_list(bitr *r, int size)
{
    int last = 8, next = 8, j;
    for (j = 0; j < size; j++) {
        if (next != 0) {
            int delta = br_se(r);
            next = (last + delta + 256) % 256;
        }
        last = (next == 0) ? last : next;
    }
}

static int is_high_profile(int p)
{
    switch (p) {
    case 100: case 110: case 122: case 244: case 44:
    case 83:  case 86:  case 118: case 128:
    case 138: case 139: case 134: case 135:
        return 1;
    default:
        return 0;
    }
}

int vi_h264_parse_sps_nal(const uint8_t *nal, size_t len, vi_h264_sps *out)
{
    uint8_t rbsp[512];
    size_t rlen = 0;
    const uint8_t *s;
    size_t n, i;
    int zeros;
    bitr r;
    int profile, chroma = 1, frame_mbs_only, crop;
    int cl = 0, cr = 0, ct = 0, cb = 0;
    int poc_type;
    int cropUnitX, cropUnitY;

    if (nal == NULL || len < 4 || (nal[0] & 0x1F) != 7)
        return -1;

    /* strip emulation-prevention bytes from the RBSP (after the NAL header) */
    s = nal + 1;
    n = len - 1;
    zeros = 0;
    for (i = 0; i < n && rlen < sizeof(rbsp); i++) {
        uint8_t b = s[i];
        if (zeros >= 2 && b == 0x03) { zeros = 0; continue; }
        rbsp[rlen++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }

    memset(&r, 0, sizeof(r));
    r.d = rbsp;
    r.size = rlen;

    memset(out, 0, sizeof(*out));

    profile = (int)br_u(&r, 8);
    out->profile_idc = profile;
    out->constraint_flags = (int)br_u(&r, 8);
    out->level_idc = (int)br_u(&r, 8);
    br_ue(&r); /* seq_parameter_set_id */

    if (is_high_profile(profile)) {
        chroma = (int)br_ue(&r);
        if (chroma == 3)
            br_u1(&r); /* separate_colour_plane_flag */
        br_ue(&r);     /* bit_depth_luma_minus8 */
        br_ue(&r);     /* bit_depth_chroma_minus8 */
        br_u1(&r);     /* qpprime_y_zero_transform_bypass_flag */
        if (br_u1(&r)) { /* seq_scaling_matrix_present_flag */
            int lists = (chroma != 3) ? 8 : 12;
            int li;
            for (li = 0; li < lists; li++) {
                if (br_u1(&r))
                    skip_scaling_list(&r, li < 6 ? 16 : 64);
            }
        }
    }
    out->chroma_format_idc = chroma;

    br_ue(&r); /* log2_max_frame_num_minus4 */
    poc_type = (int)br_ue(&r);
    if (poc_type == 0) {
        br_ue(&r); /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (poc_type == 1) {
        int k, num;
        br_u1(&r);           /* delta_pic_order_always_zero_flag */
        br_se(&r);           /* offset_for_non_ref_pic */
        br_se(&r);           /* offset_for_top_to_bottom_field */
        num = (int)br_ue(&r);
        for (k = 0; k < num; k++)
            br_se(&r);
    }

    br_ue(&r); /* max_num_ref_frames */
    br_u1(&r); /* gaps_in_frame_num_value_allowed_flag */

    out->mb_width  = (int)br_ue(&r) + 1;
    out->mb_height = (int)br_ue(&r) + 1;
    frame_mbs_only = br_u1(&r);
    out->frame_mbs_only_flag = frame_mbs_only;
    if (!frame_mbs_only)
        br_u1(&r); /* mb_adaptive_frame_field_flag */
    br_u1(&r);     /* direct_8x8_inference_flag */

    crop = br_u1(&r);
    if (crop) {
        cl = (int)br_ue(&r);
        cr = (int)br_ue(&r);
        ct = (int)br_ue(&r);
        cb = (int)br_ue(&r);
    }

    if (r.error)
        return -1;

    out->width  = out->mb_width * 16;
    out->height = (2 - frame_mbs_only) * out->mb_height * 16;

    /* apply frame cropping (ITU-T H.264 7.4.2.1.1) */
    if (chroma == 0) {
        cropUnitX = 1;
        cropUnitY = 2 - frame_mbs_only;
    } else {
        int subWidthC  = (chroma == 3) ? 1 : 2;
        int subHeightC = (chroma == 1) ? 2 : 1;
        cropUnitX = subWidthC;
        cropUnitY = subHeightC * (2 - frame_mbs_only);
    }
    out->width  -= (cl + cr) * cropUnitX;
    out->height -= (ct + cb) * cropUnitY;

    if (out->width <= 0 || out->height <= 0)
        return -1;
    return 0;
}

int vi_h264_parse_au_sps(const uint8_t *au, size_t len, vi_h264_sps *out)
{
    size_t i = 0;
    while (i + 4 < len) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            const uint8_t *nal = au + i + 3;
            size_t j = i + 3;
            size_t nal_len;
            if ((nal[0] & 0x1F) == 7) {
                /* find end (next start code) */
                while (j + 3 < len &&
                       !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1))
                    j++;
                nal_len = (j + 3 >= len) ? (len - (i + 3)) : (j - (i + 3));
                return vi_h264_parse_sps_nal(nal, nal_len, out);
            }
            i = j;
        } else {
            i++;
        }
    }
    return -1;
}
