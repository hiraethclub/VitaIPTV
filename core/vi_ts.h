/*
 * VitaIPTV - MPEG-TS demuxer (core/, C99, portable).
 *
 * Feeds on 188-byte transport-stream packets (in arbitrary byte chunks) and
 * emits elementary-stream access units via a callback:
 *   - video: one H.264 Annex-B access unit per video PES (ready to hand to
 *     sceAvcdecDecode), with the PES PTS/DTS.
 *   - audio: the AAC (ADTS) payload of each audio PES, with its PTS.
 *
 * It discovers the program automatically: PAT -> first program's PMT -> the
 * first H.264 (stream_type 0x1B) elementary PID and first AAC-ADTS (0x0F) PID.
 * HEVC (0x24) and other video types are reported as unsupported via the log.
 *
 * Assumptions (true for iptv-org / FAST HLS, logged if violated):
 *   - PAT and PMT sections each fit in a single TS packet.
 *   - One coded picture per video PES (standard for broadcast/HLS TS).
 */
#ifndef VI_TS_H
#define VI_TS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VI_TS_PACKET_SIZE 188

/* Known stream types we care about. */
#define VI_TS_STREAM_H264      0x1B
#define VI_TS_STREAM_AAC_ADTS  0x0F
#define VI_TS_STREAM_AAC_LATM  0x11
#define VI_TS_STREAM_HEVC      0x24

/* A demuxed elementary-stream sample. `data` points into the demuxer's
 * internal buffer and is valid only for the duration of the callback; copy it
 * if you need to keep it. pts/dts are 90 kHz units, or VI_TS_NO_PTS. */
#define VI_TS_NO_PTS ((int64_t)-1)

typedef struct {
    int      is_video;   /* 1 video (H.264 AU), 0 audio (ADTS) */
    const uint8_t *data;
    size_t   len;
    int64_t  pts;        /* 90 kHz, or VI_TS_NO_PTS */
    int64_t  dts;        /* 90 kHz, or VI_TS_NO_PTS */
} vi_ts_sample;

typedef void (*vi_ts_sample_cb)(void *ctx, const vi_ts_sample *s);

/* Per-elementary-stream PES reassembly state. */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      collecting;   /* a PES is currently being accumulated */
} vi_ts_es;

typedef struct {
    vi_ts_sample_cb cb;
    void           *cb_ctx;

    int pmt_pid;            /* -1 until PAT parsed */
    int video_pid;         /* -1 until PMT parsed */
    int audio_pid;         /* -1 until PMT parsed */
    int video_stream_type;
    int audio_stream_type;

    vi_ts_es video;
    vi_ts_es audio;

    uint8_t  partial[VI_TS_PACKET_SIZE];
    size_t   partial_len;  /* bytes buffered toward the next full packet */

    /* stats (handy for logging / the on-screen HUD) */
    unsigned long packets;
    unsigned long video_aus;
    unsigned long audio_frames;
    int           saw_pat;
    int           saw_pmt;
    int           warned_unsupported_video;
} vi_ts_demux;

void vi_ts_init(vi_ts_demux *d, vi_ts_sample_cb cb, void *ctx);

/* Feed an arbitrary byte range; processes all whole packets it can form. */
void vi_ts_feed(vi_ts_demux *d, const uint8_t *data, size_t len);

/* Emit any final buffered PES (call at end of a segment/stream). */
void vi_ts_flush(vi_ts_demux *d);

void vi_ts_free(vi_ts_demux *d);

#ifdef __cplusplus
}
#endif

#endif /* VI_TS_H */
