/*
 * HTTP/HTTPS GET for VitaIPTV, using OpenSSL over POSIX sockets (backed by
 * sceNet on the Vita). Sony's SceHttp/SceSsl stack fails the TLS handshake on
 * modern CDNs (observed: SCE_HTTP_ERROR_SSL 0x80431075 on the Samsung Wurl
 * host), so we bring our own TLS. OpenSSL here is 1.0.2i: TLS 1.2 + SNI, which
 * modern servers accept.
 *
 * Debug build: certificate verification is OFF (logged). Proper verification
 * with a CA bundle and correct clock is a later task.
 */
#include "vi_http.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <psp2/kernel/rng.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "vi_log.h"

#define TAG "http"
#define UA  "Mozilla/5.0 (PlayStation Vita; VitaIPTV) AppleWebKit/537.36"

static SSL_CTX *g_ctx;

int vi_http_init(void)
{
    unsigned char seed[64];

    if (g_ctx)
        return 0;

    /* OpenSSL needs entropy or the handshake fails; Vita has no /dev/urandom. */
    sceKernelGetRandomNumber(seed, sizeof(seed));
    RAND_seed(seed, sizeof(seed));

    SSL_library_init();
    SSL_load_error_strings();

    g_ctx = SSL_CTX_new(SSLv23_client_method());
    if (!g_ctx) {
        VI_LOGE(TAG, "SSL_CTX_new failed");
        return -1;
    }
    SSL_CTX_set_options(g_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(g_ctx, SSL_MODE_AUTO_RETRY);
    VI_LOGW(TAG, "TLS via OpenSSL 1.0.2 (cert verification DISABLED - debug)");
    return 0;
}

void vi_http_term(void)
{
    if (g_ctx) { SSL_CTX_free(g_ctx); g_ctx = NULL; }
}

/* Split a URL into scheme/host/port/path. Returns 1 if https. */
static int parse_url(const char *url, char *host, size_t hostsz,
                     char *port, size_t portsz, char *path, size_t pathsz)
{
    int https = 0;
    const char *p = url, *h, *slash, *colon;
    size_t hlen;

    if (strncmp(p, "https://", 8) == 0) { https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { https = 0; p += 7; }

    h = p;
    slash = strchr(h, '/');
    colon = strchr(h, ':');
    if (colon && (!slash || colon < slash)) {
        hlen = (size_t)(colon - h);
        snprintf(port, portsz, "%.*s",
                 (int)((slash ? (size_t)(slash - colon - 1)
                              : strlen(colon + 1))), colon + 1);
    } else {
        hlen = slash ? (size_t)(slash - h) : strlen(h);
        snprintf(port, portsz, "%s", https ? "443" : "80");
    }
    if (hlen >= hostsz) hlen = hostsz - 1;
    memcpy(host, h, hlen); host[hlen] = '\0';

    snprintf(path, pathsz, "%s", slash ? slash : "/");
    return https;
}

/* Read the whole response (until the peer closes) into a growable buffer. */
static uint8_t *read_all(SSL *ssl, int fd, size_t *out_len)
{
    size_t cap = 65536, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        int n;
        if (len + 8192 > cap) {
            uint8_t *nb = (uint8_t *)realloc(buf, cap * 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb; cap *= 2;
        }
        if (ssl) n = SSL_read(ssl, buf + len, (int)(cap - len - 1));
        else     n = (int)recv(fd, buf + len, cap - len - 1, 0);
        if (n > 0) { len += (size_t)n; continue; }
        break;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* Case-insensitive substring search (newlib lacks strcasestr). */
static int ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle), i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nl; j++) {
            char a = hay[i + j], b = needle[j];
            if (!a) return 0;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

/* De-chunk a chunked transfer body in place. Returns new length. */
static size_t dechunk(uint8_t *b, size_t n)
{
    size_t r = 0, w = 0;
    while (r < n) {
        char *end;
        long sz = strtol((char *)b + r, &end, 16);
        size_t adv = (size_t)((uint8_t *)end - (b + r));
        if (adv == 0) break;
        r += adv;
        while (r < n && (b[r] == '\r' || b[r] == '\n')) r++;
        if (sz <= 0) break;
        if (r + (size_t)sz > n) sz = (long)(n - r);
        memmove(b + w, b + r, (size_t)sz);
        w += (size_t)sz; r += (size_t)sz;
        while (r < n && (b[r] == '\r' || b[r] == '\n')) r++;
    }
    return w;
}

uint8_t *vi_http_get(const char *url, size_t *out_len, int *status)
{
    char host[256], port[16], path[1024];
    struct addrinfo hints, *res = NULL, *ai;
    int fd = -1, https, gai;
    SSL *ssl = NULL;
    char req[1600];
    uint8_t *raw = NULL, *body = NULL;
    size_t rawlen = 0, bodylen;
    char *hdr_end;
    int st = 0;

    if (out_len) *out_len = 0;
    if (status) *status = 0;

    https = parse_url(url, host, sizeof(host), port, sizeof(port),
                      path, sizeof(path));

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0 || !res) { VI_LOGE(TAG, "DNS fail for %s (%d)", host, gai); return NULL; }

    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { VI_LOGE(TAG, "connect fail %s:%s", host, port); return NULL; }

    if (https) {
        ssl = SSL_new(g_ctx);
        if (!ssl) { VI_LOGE(TAG, "SSL_new"); goto done; }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host); /* SNI */
        if (SSL_connect(ssl) != 1) {
            VI_LOGE(TAG, "SSL_connect fail %s: ssl_err %d err 0x%lx",
                    host, SSL_get_error(ssl, -1),
                    (unsigned long)ERR_get_error());
            goto done;
        }
    }

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
             "Accept: */*\r\nConnection: close\r\n\r\n", path, host, UA);
    if (https) SSL_write(ssl, req, (int)strlen(req));
    else       send(fd, req, strlen(req), 0);

    raw = read_all(ssl, fd, &rawlen);
    if (!raw) { VI_LOGE(TAG, "read fail"); goto done; }

    /* status line */
    if (rawlen > 12 && memcmp(raw, "HTTP/1.", 7) == 0)
        st = atoi((char *)raw + 9);
    if (status) *status = st;

    hdr_end = strstr((char *)raw, "\r\n\r\n");
    if (!hdr_end) { VI_LOGE(TAG, "no header end"); goto done; }
    *hdr_end = '\0';
    {
        size_t hdrlen = (size_t)((uint8_t *)hdr_end + 4 - raw);
        int chunked = ci_contains((char *)raw, "transfer-encoding: chunked");
        bodylen = rawlen - hdrlen;
        body = (uint8_t *)malloc(bodylen + 1);
        if (!body) goto done;
        memcpy(body, raw + hdrlen, bodylen);
        if (chunked)
            bodylen = dechunk(body, bodylen);
        body[bodylen] = '\0';
    }

    if (st != 200)
        VI_LOGW(TAG, "HTTP %d for %.48s", st, url);

    free(raw); raw = NULL;
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    if (fd >= 0) close(fd);
    if (out_len) *out_len = bodylen;
    return body;

done:
    free(raw);
    free(body);
    if (ssl) SSL_free(ssl);
    if (fd >= 0) close(fd);
    return NULL;
}
