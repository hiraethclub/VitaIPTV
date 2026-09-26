/*
 * VitaIPTV - SceHttp fetch helper (vita/, device-only).
 * Blocking GET of a URL into a malloc'd buffer. Runs on a worker thread.
 */
#ifndef VI_HTTP_H
#define VI_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init SceSsl + SceHttp. Net + the NET/HTTP/SSL/HTTPS sysmodules must already
 * be up. Returns 0 on success. In this debug build TLS cert verification is
 * disabled (loudly logged) to sidestep device-clock/CA issues during testing. */
int vi_http_init(void);
void vi_http_term(void);

/*
 * GET url into a freshly malloc'd buffer (caller frees). On success returns the
 * buffer and sets *out_len and *status (HTTP status code); returns NULL on
 * failure. A NUL terminator is appended (not counted in *out_len) so text
 * bodies can be treated as C strings.
 */
uint8_t *vi_http_get(const char *url, size_t *out_len, int *status);

#ifdef __cplusplus
}
#endif

#endif /* VI_HTTP_H */
