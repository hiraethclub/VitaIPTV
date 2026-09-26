#include "vi_ts.h"

#include <stdlib.h>
#include <string.h>

#include "vi_log.h"

#define TAG "ts"

void vi_ts_init(vi_ts_demux *d, vi_ts_sample_cb cb, void *ctx)
{
    memset(d, 0, sizeof(*d));
    d->cb = cb;
    d->cb_ctx = ctx;
    d->pmt_pid = -1;
    d->video_pid = -1;
    d->audio_pid = -1;
}

void vi_ts_free(vi_ts_demux *d)
{
    free(d->video.buf);
    free(d->audio.buf);
    memset(d, 0, sizeof(*d));
    d->pmt_pid = d->video_pid = d->audio_pid = -1;
}

static int es_append(vi_ts_es *es, const uint8_t *p, size_t n)
{
    if (es->len + n > es->cap) {
        size_t cap = es->cap ? es->cap : 4096;
        uint8_t *g;
        while (cap < es->len + n)
            cap *= 2;
        g = (uint8_t *)realloc(es->buf, cap);
        if (g == NULL)
            return -1;
        es->buf = g;
        es->cap = cap;
    }
    memcpy(es->buf + es->len, p, n);
    es->len += n;
    return 0;
}

/* Parse a 33-bit PTS/DTS field (5 bytes with marker bits). */
static int64_t parse_ts_field(const uint8_t *p)
{
    int64_t v;
    v  = (int64_t)((p[0] >> 1) & 0x07) << 30;
    v |= (int64_t)p[1] << 22;
    v |= (int64_t)(p[2] >> 1) << 15;
    v |= (int64_t)p[3] << 7;
    v |= (int64_t)(p[4] >> 1);
    return v;
}

/* Emit the accumulated PES for one elementary stream, then reset it. */
static void emit_pes(vi_ts_demux *d, vi_ts_es *es, int is_video)
{
    const uint8_t *b = es->buf;
    size_t n = es->len;
    int64_t pts = VI_TS_NO_PTS, dts = VI_TS_NO_PTS;
    size_t hdr;
    vi_ts_sample s;

    es->collecting = 0;
    if (n < 9)
        goto done;
    if (!(b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x01))
        goto done; /* not a PES start */

    /* b[3] = stream_id, b[4..5] = PES_packet_length */
    /* b[6] flags1, b[7] flags2 (PTS_DTS_flags = top 2 bits), b[8] hdr len */
    if (b[3] >= 0xC0) {
        /* audio (0xC0-0xDF) and video (0xE0-0xEF) carry the optional header;
         * padding (0xBE) / private_stream_2 (0xBF) do not. */
        int pts_dts = (b[7] >> 6) & 0x03;
        size_t hdr_data_len = b[8];
        hdr = 9 + hdr_data_len;
        if (hdr > n)
            goto done;
        if ((pts_dts & 0x02) && hdr_data_len >= 5)
            pts = parse_ts_field(b + 9);
        if (pts_dts == 0x03 && hdr_data_len >= 10)
            dts = parse_ts_field(b + 14);
    } else {
        goto done; /* not a media PES */
    }

    s.is_video = is_video;
    s.data = b + hdr;
    s.len = n - hdr;
    s.pts = pts;
    s.dts = dts;
    if (s.len > 0 && d->cb) {
        d->cb(d->cb_ctx, &s);
        if (is_video)
            d->video_aus++;
        else
            d->audio_frames++;
    }
done:
    es->len = 0;
}

/* --- PSI (PAT/PMT) parsing: single-packet sections assumed --- */

static void parse_pat(vi_ts_demux *d, const uint8_t *sec, size_t n)
{
    size_t section_length, s, end;

    if (n < 8 || sec[0] != 0x00)
        return;
    section_length = ((sec[1] & 0x0F) << 8) | sec[2];
    if (section_length + 3 > n) {
        VI_LOGW(TAG, "PAT spans packets (len=%u), unsupported",
                (unsigned)section_length);
        return;
    }
    end = 3 + section_length - 4; /* drop CRC */
    s = 8;
    while (s + 4 <= end) {
        int program = (sec[s] << 8) | sec[s + 1];
        int pmt_pid = ((sec[s + 2] & 0x1F) << 8) | sec[s + 3];
        if (program != 0) {
            d->pmt_pid = pmt_pid;
            d->saw_pat = 1;
            VI_LOGI(TAG, "PAT: program %d -> pmt_pid %d", program, pmt_pid);
            return;
        }
        s += 4;
    }
}

static void parse_pmt(vi_ts_demux *d, const uint8_t *sec, size_t n)
{
    size_t section_length, prog_info_len, s, end;

    if (n < 12 || sec[0] != 0x02)
        return;
    section_length = ((sec[1] & 0x0F) << 8) | sec[2];
    if (section_length + 3 > n) {
        VI_LOGW(TAG, "PMT spans packets (len=%u), unsupported",
                (unsigned)section_length);
        return;
    }
    end = 3 + section_length - 4;
    prog_info_len = ((sec[10] & 0x0F) << 8) | sec[11];
    s = 12 + prog_info_len;
    while (s + 5 <= end) {
        int stream_type = sec[s];
        int epid = ((sec[s + 1] & 0x1F) << 8) | sec[s + 2];
        size_t es_info_len = ((sec[s + 3] & 0x0F) << 8) | sec[s + 4];

        if ((stream_type == VI_TS_STREAM_H264) && d->video_pid < 0) {
            d->video_pid = epid;
            d->video_stream_type = stream_type;
            VI_LOGI(TAG, "PMT: H.264 video pid %d", epid);
        } else if ((stream_type == VI_TS_STREAM_HEVC) && d->video_pid < 0) {
            if (!d->warned_unsupported_video) {
                VI_LOGE(TAG, "video stream_type 0x%02X (HEVC) not supported",
                        stream_type);
                d->warned_unsupported_video = 1;
            }
        } else if ((stream_type == VI_TS_STREAM_AAC_ADTS) && d->audio_pid < 0) {
            d->audio_pid = epid;
            d->audio_stream_type = stream_type;
            VI_LOGI(TAG, "PMT: AAC(ADTS) audio pid %d", epid);
        } else if ((stream_type == VI_TS_STREAM_AAC_LATM) && d->audio_pid < 0) {
            d->audio_pid = epid;
            d->audio_stream_type = stream_type;
            VI_LOGW(TAG, "PMT: AAC(LATM) audio pid %d (LATM not yet handled)",
                    epid);
        }
        s += 5 + es_info_len;
    }
    d->saw_pmt = 1;
}

/* --- one whole 188-byte packet --- */

static void handle_packet(vi_ts_demux *d, const uint8_t *p)
{
    int pusi, pid, afc;
    size_t off = 4;

    d->packets++;
    if (p[0] != 0x47)
        return; /* lost sync; caller resyncs */

    pusi = (p[1] >> 6) & 0x01;
    pid = ((p[1] & 0x1F) << 8) | p[2];
    afc = (p[3] >> 4) & 0x03;

    if (afc & 0x02) {
        size_t af_len = p[4];
        off = 5 + af_len;
    }
    if (!(afc & 0x01) || off >= VI_TS_PACKET_SIZE)
        return; /* no payload */

    /* PSI: PAT / PMT */
    if (pid == 0x0000 || (d->pmt_pid >= 0 && pid == d->pmt_pid)) {
        size_t p_off = off;
        if (pusi) {
            size_t ptr = p[p_off];
            p_off += 1 + ptr; /* pointer_field */
        }
        if (p_off < VI_TS_PACKET_SIZE) {
            const uint8_t *sec = p + p_off;
            size_t seclen = VI_TS_PACKET_SIZE - p_off;
            if (pid == 0x0000)
                parse_pat(d, sec, seclen);
            else
                parse_pmt(d, sec, seclen);
        }
        return;
    }

    /* Elementary streams */
    {
        vi_ts_es *es = NULL;
        int is_video = 0;
        if (pid == d->video_pid) { es = &d->video; is_video = 1; }
        else if (pid == d->audio_pid) { es = &d->audio; is_video = 0; }
        if (es == NULL)
            return;

        if (pusi) {
            /* start of a new PES: finalise the previous one first */
            if (es->collecting && es->len > 0)
                emit_pes(d, es, is_video);
            es->len = 0;
            es->collecting = 1;
        }
        if (es->collecting) {
            if (es_append(es, p + off, VI_TS_PACKET_SIZE - off) != 0)
                VI_LOGE(TAG, "OOM appending PES payload");
        }
    }
}

void vi_ts_feed(vi_ts_demux *d, const uint8_t *data, size_t len)
{
    size_t i = 0;

    /* Complete a partially buffered packet first. */
    if (d->partial_len > 0) {
        size_t need = VI_TS_PACKET_SIZE - d->partial_len;
        size_t take = need < len ? need : len;
        memcpy(d->partial + d->partial_len, data, take);
        d->partial_len += take;
        i = take;
        if (d->partial_len == VI_TS_PACKET_SIZE) {
            handle_packet(d, d->partial);
            d->partial_len = 0;
        } else {
            return; /* still incomplete */
        }
    }

    /* Whole packets. Resync on a missing sync byte. */
    while (i + VI_TS_PACKET_SIZE <= len) {
        if (data[i] != 0x47) {
            i++;
            continue;
        }
        handle_packet(d, data + i);
        i += VI_TS_PACKET_SIZE;
    }

    /* Buffer the trailing remainder. */
    if (i < len) {
        size_t rem = len - i;
        if (rem > VI_TS_PACKET_SIZE)
            rem = VI_TS_PACKET_SIZE; /* defensive */
        memcpy(d->partial, data + i, rem);
        d->partial_len = rem;
    }
}

void vi_ts_flush(vi_ts_demux *d)
{
    if (d->video.collecting && d->video.len > 0)
        emit_pes(d, &d->video, 1);
    if (d->audio.collecting && d->audio.len > 0)
        emit_pes(d, &d->audio, 0);
}
