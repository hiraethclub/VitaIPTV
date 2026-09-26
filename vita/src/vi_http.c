#include "vi_http.h"

#include <psp2/net/http.h>
#include <psp2/libssl.h>

#include <stdlib.h>
#include <string.h>

#include "vi_log.h"

#define TAG "http"
#define UA  "Mozilla/5.0 (PlayStation Vita; VitaIPTV) AppleWebKit/537.36"

static int g_ready;

int vi_http_init(void)
{
    int r;

    if (g_ready)
        return 0;

    r = sceSslInit(300 * 1024);
    if (r < 0)
        VI_LOGW(TAG, "sceSslInit -> 0x%08X (may be already up)", r);

    r = sceHttpInit(300 * 1024);
    if (r < 0) {
        VI_LOGE(TAG, "sceHttpInit -> 0x%08X", r);
        return -1;
    }

    /* Debug: skip certificate validation so a wrong RTC / missing CA does not
     * block testing. MUST be revisited before real use. */
    sceHttpsDisableOption(SCE_HTTPS_FLAG_SERVER_VERIFY |
                          SCE_HTTPS_FLAG_CN_CHECK |
                          SCE_HTTPS_FLAG_NOT_AFTER_CHECK |
                          SCE_HTTPS_FLAG_NOT_BEFORE_CHECK |
                          SCE_HTTPS_FLAG_KNOWN_CA_CHECK);
    VI_LOGW(TAG, "TLS certificate verification DISABLED (debug build)");

    g_ready = 1;
    return 0;
}

void vi_http_term(void)
{
    if (!g_ready)
        return;
    sceHttpTerm();
    sceSslTerm();
    g_ready = 0;
}

uint8_t *vi_http_get(const char *url, size_t *out_len, int *status)
{
    int tmpl = -1, conn = -1, req = -1;
    int r, st = 0;
    uint8_t *buf = NULL;
    size_t cap = 64 * 1024, len = 0;

    if (out_len) *out_len = 0;
    if (status) *status = 0;

    tmpl = sceHttpCreateTemplate(UA, SCE_HTTP_VERSION_1_1, 1);
    if (tmpl < 0) { VI_LOGE(TAG, "CreateTemplate 0x%08X", tmpl); goto fail; }

    conn = sceHttpCreateConnectionWithURL(tmpl, url, 1);
    if (conn < 0) { VI_LOGE(TAG, "CreateConnection 0x%08X", conn); goto fail; }

    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
    if (req < 0) { VI_LOGE(TAG, "CreateRequest 0x%08X", req); goto fail; }

    r = sceHttpSendRequest(req, NULL, 0);
    if (r < 0) { VI_LOGE(TAG, "SendRequest 0x%08X", r); goto fail; }

    sceHttpGetStatusCode(req, &st);
    if (status) *status = st;
    if (st != 200) {
        VI_LOGW(TAG, "HTTP %d for %.64s", st, url);
        if (st >= 400)
            goto fail;
    }

    buf = (uint8_t *)malloc(cap);
    if (!buf) { VI_LOGE(TAG, "OOM"); goto fail; }

    for (;;) {
        if (len + 4096 > cap) {
            size_t ncap = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, ncap);
            if (!nb) { VI_LOGE(TAG, "OOM grow"); goto fail; }
            buf = nb; cap = ncap;
        }
        r = sceHttpReadData(req, buf + len, (unsigned)(cap - len - 1));
        if (r < 0) { VI_LOGE(TAG, "ReadData 0x%08X", r); goto fail; }
        if (r == 0) break;
        len += (size_t)r;
    }
    buf[len] = '\0';

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tmpl);
    if (out_len) *out_len = len;
    return buf;

fail:
    free(buf);
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
    return NULL;
}
