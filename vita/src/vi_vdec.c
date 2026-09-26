#include "vi_vdec.h"

#include <psp2/videodec.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <stdlib.h>
#include <string.h>

#include "vi_log.h"

#define TAG "vdec"

#define ROUND_UP_16(x) (((x) + 15) & ~15)
#define AU_PADDING 64  /* trailing zero padding some decoders expect */

struct vi_vdec {
    SceAvcdecCtrl decoder;
    SceUID        block;
    int           coded_w;   /* 16-aligned */
    int           coded_h;
    int           disp_w;    /* real display size */
    int           disp_h;

    vita2d_texture *tex[2];
    int             front;   /* index shown */
    SceUID          mutex;

    uint8_t *au_buf;
    size_t   au_cap;

    unsigned long frames;
    int inited_lib;
};

static void lock(vi_vdec *v)   { sceKernelLockMutex(v->mutex, 1, NULL); }
static void unlock(vi_vdec *v) { sceKernelUnlockMutex(v->mutex, 1); }

vi_vdec *vi_vdec_create(int width, int height)
{
    vi_vdec *v;
    SceVideodecQueryInitInfoHwAvcdec init;
    SceAvcdecQueryDecoderInfo qinfo;
    SceAvcdecDecoderInfo qout;
    int ret, i;
    size_t sz;

    v = (vi_vdec *)calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->block = -1;
    v->mutex = -1;
    v->disp_w = width;
    v->disp_h = height;
    v->coded_w = ROUND_UP_16(width);
    v->coded_h = ROUND_UP_16(height);

    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = v->coded_w;
    init.vertical = v->coded_h;
    init.numOfRefFrames = 4;
    init.numOfStreams = 1;

    ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (ret < 0) {
        VI_LOGE(TAG, "sceVideodecInitLibrary 0x%08X (%dx%d)",
                ret, v->coded_w, v->coded_h);
        goto fail;
    }
    v->inited_lib = 1;

    memset(&qinfo, 0, sizeof(qinfo));
    qinfo.horizontal = init.horizontal;
    qinfo.vertical = init.vertical;
    qinfo.numOfRefFrames = init.numOfRefFrames;
    memset(&qout, 0, sizeof(qout));
    ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &qinfo, &qout);
    if (ret < 0) {
        VI_LOGE(TAG, "QueryDecoderMemSize 0x%08X", ret);
        goto fail;
    }

    sz = (qout.frameMemSize + 0xFFFFF) & ~0xFFFFFu;
    v->decoder.frameBuf.size = sz;
    v->block = sceKernelAllocMemBlock("vi_vdec",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
    if (v->block < 0) {
        VI_LOGE(TAG, "AllocMemBlock(%u) 0x%08X", (unsigned)sz, v->block);
        goto fail;
    }
    ret = sceKernelGetMemBlockBase(v->block, &v->decoder.frameBuf.pBuf);
    if (ret < 0) {
        VI_LOGE(TAG, "GetMemBlockBase 0x%08X", ret);
        goto fail;
    }

    ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &v->decoder, &qinfo);
    if (ret < 0) {
        VI_LOGE(TAG, "sceAvcdecCreateDecoder 0x%08X", ret);
        goto fail;
    }

    for (i = 0; i < 2; i++) {
        v->tex[i] = vita2d_create_empty_texture_format(
            (unsigned)v->coded_w, (unsigned)v->coded_h,
            SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (!v->tex[i]) {
            VI_LOGE(TAG, "create texture %d failed", i);
            goto fail;
        }
    }

    v->mutex = sceKernelCreateMutex("vi_vdec", 0, 0, NULL);
    v->front = 0;

    VI_LOGI(TAG, "decoder ready: coded %dx%d, display %dx%d, frameMem %u",
            v->coded_w, v->coded_h, v->disp_w, v->disp_h,
            (unsigned)qout.frameMemSize);
    return v;

fail:
    vi_vdec_destroy(v);
    return NULL;
}

int vi_vdec_decode(vi_vdec *v, const uint8_t *au, size_t len)
{
    SceAvcdecAu dau;
    SceAvcdecArrayPicture array;
    SceAvcdecPicture picture;
    SceAvcdecPicture *plist = &picture;
    int back = v->front ^ 1;
    int ret;

    if (!v || len == 0)
        return 0;

    if (v->au_cap < len + AU_PADDING) {
        size_t nc = len + AU_PADDING;
        uint8_t *nb = (uint8_t *)realloc(v->au_buf, nc);
        if (!nb) { VI_LOGE(TAG, "OOM au buf"); return -1; }
        v->au_buf = nb; v->au_cap = nc;
    }
    memcpy(v->au_buf, au, len);
    memset(v->au_buf + len, 0, AU_PADDING);

    memset(&dau, 0, sizeof(dau));
    dau.es.pBuf = v->au_buf;
    dau.es.size = (uint32_t)len;
    dau.pts.lower = dau.pts.upper = 0xFFFFFFFF;
    dau.dts.lower = dau.dts.upper = 0xFFFFFFFF;

    memset(&array, 0, sizeof(array));
    memset(&picture, 0, sizeof(picture));
    array.numOfElm = 1;
    array.pPicture = &plist;

    picture.size = sizeof(picture);
    picture.frame.pixelType = 0; /* RGBA8888 */
    picture.frame.framePitch = (uint32_t)v->coded_w;
    picture.frame.frameWidth = (uint32_t)v->coded_w;
    picture.frame.frameHeight = (uint32_t)v->coded_h;
    picture.frame.pPicture[0] = vita2d_texture_get_datap(v->tex[back]);

    ret = sceAvcdecDecode(&v->decoder, &dau, &array);
    if (ret < 0) {
        VI_LOGE(TAG, "sceAvcdecDecode 0x%08X (len=%u)", ret, (unsigned)len);
        return -1;
    }
    if (array.numOfOutput < 1)
        return 0; /* buffered, no output frame yet */

    lock(v);
    v->front = back;
    v->frames++;
    unlock(v);
    return 1;
}

vita2d_texture *vi_vdec_front(vi_vdec *v)
{
    vita2d_texture *t;
    if (!v) return NULL;
    lock(v);
    t = (v->frames > 0) ? v->tex[v->front] : NULL;
    unlock(v);
    return t;
}

int vi_vdec_width(const vi_vdec *v)  { return v ? v->disp_w : 0; }
int vi_vdec_height(const vi_vdec *v) { return v ? v->disp_h : 0; }
unsigned long vi_vdec_frame_count(const vi_vdec *v) { return v ? v->frames : 0; }

void vi_vdec_destroy(vi_vdec *v)
{
    int i;
    if (!v)
        return;
    if (v->mutex >= 0)
        sceKernelDeleteMutex(v->mutex);
    for (i = 0; i < 2; i++)
        if (v->tex[i])
            vita2d_free_texture(v->tex[i]);
    /* delete decoder before freeing its frame buffer */
    if (v->decoder.frameBuf.pBuf)
        sceAvcdecDeleteDecoder(&v->decoder);
    if (v->block >= 0)
        sceKernelFreeMemBlock(v->block);
    if (v->inited_lib)
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    free(v->au_buf);
    free(v);
}
