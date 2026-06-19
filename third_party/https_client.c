/*
 * HTTPS client over lwIP sockets + mbed TLS (TLS 1.2/1.3; ALPN http/1.1).
 * Adapted from XboxQRCodeLogin/native/https_client.c and extended with https_request()
 * so large request bodies (e.g. base64-encoded save archives) can be streamed without a
 * fixed-size request buffer.
 *
 * NOTE: certificate verification is disabled (MBEDTLS_SSL_VERIFY_NONE) — demo-grade only.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/build_info.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"

#include <hal/debug.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <windows.h>

#include "https_client.h"

#define HTTP_HDR_MAX 8192

static void ssl_dbg(void *ctx, int level, const char *file, int line, const char *str)
{
    (void)ctx;
    (void)level;
    (void)file;
    (void)line;
    (void)str;
}

static int lwip_send_cb(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    return send(fd, buf, len, 0);
}

static int lwip_recv_cb(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    return recv(fd, buf, len, 0);
}

static int ssl_write_all(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(ssl, buf + written, len - written);
        if (ret <= 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            return ret;
        }
        written += (size_t)ret;
    }
    return 0;
}

static int ssl_write_all_progress(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len,
                                  HttpsUploadProgressFn progress, void *progress_ctx)
{
    size_t written = 0;
    const size_t chunk = 32 * 1024;

    if (progress) {
        progress(0, len, progress_ctx);
    }

    while (written < len) {
        size_t n = len - written;
        if (n > chunk) {
            n = chunk;
        }
        int ret = mbedtls_ssl_write(ssl, buf + written, n);
        if (ret <= 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            return ret;
        }
        written += (size_t)ret;
        if (progress) {
            progress(written, len, progress_ctx);
        }
    }
    return 0;
}

int https_request(const char *hostname, const char *port, const char *method, const char *path,
                  const char *content_type, const char *const *extra_headers, int n_headers,
                  const char *body, size_t body_len, char *out, size_t out_sz,
                  HttpsUploadProgressFn upload_progress, void *upload_progress_ctx)
{
    int ret;
    int fd = -1;
    struct addrinfo hints, *res = NULL, *rp;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    const char *pers = "insignia_nxdk";

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, port, &hints, &res) != 0) {
        debugPrint("getaddrinfo failed for %s\n", hostname);
        ret = -1;
        goto cleanup;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
            break;
        }
        closesocket(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    res = NULL;

    if (fd < 0) {
        debugPrint("TCP connect failed\n");
        ret = -2;
        goto cleanup;
    }

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        debugPrint("ctr_drbg_seed failed: -0x%04x\n", (unsigned int)-ret);
        goto cleanup;
    }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        debugPrint("ssl_config_defaults failed: -0x%04x\n", (unsigned int)-ret);
        goto cleanup;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_dbg(&conf, ssl_dbg, NULL);

#if defined(MBEDTLS_SSL_ALPN)
    {
        const char *alpn[] = { "http/1.1", NULL };
        ret = mbedtls_ssl_conf_alpn_protocols(&conf, alpn);
        if (ret != 0) {
            debugPrint("alpn failed: -0x%04x\n", (unsigned int)-ret);
            goto cleanup;
        }
    }
#endif

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        debugPrint("ssl_setup failed: -0x%04x\n", (unsigned int)-ret);
        goto cleanup;
    }

    ret = mbedtls_ssl_set_hostname(&ssl, hostname);
    if (ret != 0) {
        debugPrint("set_hostname failed: -0x%04x\n", (unsigned int)-ret);
        goto cleanup;
    }

    mbedtls_ssl_set_bio(&ssl, &fd, lwip_send_cb, lwip_recv_cb, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            debugPrint("handshake failed: -0x%04x\n", (unsigned int)-ret);
            goto cleanup;
        }
    }

    /* Build and send the request header (small), then stream the body separately. */
    {
        char hdr[HTTP_HDR_MAX];
        int off = 0;
        int n = snprintf(hdr + off, sizeof(hdr) - off,
                         "%s %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Length: %u\r\n"
                         "Connection: close\r\n",
                         method, path, hostname, content_type ? content_type : "application/json",
                         (unsigned)body_len);
        if (n < 0 || n >= (int)(sizeof(hdr) - off)) {
            debugPrint("request header too large\n");
            ret = -1;
            goto cleanup;
        }
        off += n;
        for (int i = 0; i < n_headers && extra_headers && extra_headers[i]; i++) {
            n = snprintf(hdr + off, sizeof(hdr) - off, "%s\r\n", extra_headers[i]);
            if (n < 0 || n >= (int)(sizeof(hdr) - off)) {
                debugPrint("too many/long headers\n");
                ret = -1;
                goto cleanup;
            }
            off += n;
        }
        n = snprintf(hdr + off, sizeof(hdr) - off, "\r\n");
        if (n < 0 || n >= (int)(sizeof(hdr) - off)) {
            ret = -1;
            goto cleanup;
        }
        off += n;

        ret = ssl_write_all(&ssl, (const unsigned char *)hdr, (size_t)off);
        if (ret != 0) {
            debugPrint("ssl_write header failed: -0x%04x\n", (unsigned int)-ret);
            goto cleanup;
        }
        if (body && body_len) {
            if (upload_progress) {
                ret = ssl_write_all_progress(&ssl, (const unsigned char *)body, body_len,
                                             upload_progress, upload_progress_ctx);
            } else {
                ret = ssl_write_all(&ssl, (const unsigned char *)body, body_len);
            }
            if (ret != 0) {
                debugPrint("ssl_write body failed: -0x%04x\n", (unsigned int)-ret);
                goto cleanup;
            }
        }
    }

    /* Read the full response into out. */
    {
        size_t total = 0;
        out[0] = '\0';
        while (total < out_sz - 1) {
            ret = mbedtls_ssl_read(&ssl, (unsigned char *)out + total, out_sz - 1 - total);
            if (ret == 0) {
                break;
            }
            if (ret < 0) {
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    continue;
                }
                if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                    break;
                }
                debugPrint("ssl_read failed: -0x%04x\n", (unsigned int)-ret);
                goto cleanup;
            }
            total += (size_t)ret;
        }
        out[total] = '\0';
    }

    ret = 0;

cleanup:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (fd >= 0) {
        closesocket(fd);
    }
    return ret;
}

int https_post_json(const char *hostname, const char *port, const char *path, const char *json_body,
                    char *out, size_t out_sz)
{
    return https_request(hostname, port, "POST", path, "application/json", NULL, 0, json_body,
                         json_body ? strlen(json_body) : 0, out, out_sz, NULL, NULL);
}

int https_get_to_file(const char *hostname, const char *port, const char *path,
                      const char *const *extra_headers, int n_headers, const char *destPath,
                      int *statusOut)
{
    int ret;
    int fd = -1;
    struct addrinfo hints, *res = NULL, *rp;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    const char *pers = "insignia_nxdk";
    HANDLE fout = INVALID_HANDLE_VALUE;
    static char hdr[16384];
    size_t hdrLen = 0;
    int headersDone = 0;

    if (statusOut) {
        *statusOut = 0;
    }

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, port, &hints, &res) != 0) {
        ret = -1;
        goto cleanup;
    }
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
            break;
        }
        closesocket(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    res = NULL;
    if (fd < 0) {
        ret = -2;
        goto cleanup;
    }

    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        ret = -3;
        goto cleanup;
    }
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        ret = -4;
        goto cleanup;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_dbg(&conf, ssl_dbg, NULL);
#if defined(MBEDTLS_SSL_ALPN)
    {
        const char *alpn[] = { "http/1.1", NULL };
        mbedtls_ssl_conf_alpn_protocols(&conf, alpn);
    }
#endif
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
        ret = -5;
        goto cleanup;
    }
    if (mbedtls_ssl_set_hostname(&ssl, hostname) != 0) {
        ret = -6;
        goto cleanup;
    }
    mbedtls_ssl_set_bio(&ssl, &fd, lwip_send_cb, lwip_recv_cb, NULL);
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            debugPrint("download handshake failed: -0x%04x\n", (unsigned int)-ret);
            goto cleanup;
        }
    }

    {
        char req[HTTP_HDR_MAX];
        int off = 0;
        int n = snprintf(req + off, sizeof(req) - off, "GET %s HTTP/1.1\r\nHost: %s\r\n", path,
                         hostname);
        if (n < 0 || n >= (int)(sizeof(req) - off)) {
            ret = -1;
            goto cleanup;
        }
        off += n;
        for (int i = 0; i < n_headers && extra_headers && extra_headers[i]; i++) {
            n = snprintf(req + off, sizeof(req) - off, "%s\r\n", extra_headers[i]);
            if (n < 0 || n >= (int)(sizeof(req) - off)) {
                ret = -1;
                goto cleanup;
            }
            off += n;
        }
        n = snprintf(req + off, sizeof(req) - off, "Connection: close\r\n\r\n");
        if (n < 0 || n >= (int)(sizeof(req) - off)) {
            ret = -1;
            goto cleanup;
        }
        off += n;
        ret = ssl_write_all(&ssl, (const unsigned char *)req, (size_t)off);
        if (ret != 0) {
            goto cleanup;
        }
    }

    for (;;) {
        unsigned char buf[4096];
        int n = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                break;
            }
            ret = n;
            goto cleanup;
        }

        if (!headersDone) {
            size_t canCopy = sizeof(hdr) - 1 - hdrLen;
            size_t copy = ((size_t)n < canCopy) ? (size_t)n : canCopy;
            memcpy(hdr + hdrLen, buf, copy);
            hdrLen += copy;
            hdr[hdrLen] = '\0';

            char *bound = strstr(hdr, "\r\n\r\n");
            if (!bound) {
                continue; /* need more header bytes */
            }
            headersDone = 1;

            int status = 0;
            char *sp = strchr(hdr, ' ');
            if (sp) {
                status = atoi(sp + 1);
            }
            if (statusOut) {
                *statusOut = status;
            }
            if (status < 200 || status >= 300) {
                ret = -10;
                goto cleanup;
            }

            fout = CreateFile(destPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              NULL);
            if (fout == INVALID_HANDLE_VALUE) {
                ret = -11;
                goto cleanup;
            }

            size_t headerLen = (size_t)(bound - hdr) + 4;
            if (hdrLen > headerLen) {
                DWORD w = 0;
                WriteFile(fout, hdr + headerLen, (DWORD)(hdrLen - headerLen), &w, NULL);
            }
            /* Any bytes of this read that did not fit into hdr are also body. */
            if (copy < (size_t)n) {
                DWORD w = 0;
                WriteFile(fout, buf + copy, (DWORD)((size_t)n - copy), &w, NULL);
            }
        } else {
            DWORD w = 0;
            if (!WriteFile(fout, buf, (DWORD)n, &w, NULL) || w != (DWORD)n) {
                ret = -12;
                goto cleanup;
            }
        }
    }

    ret = (fout != INVALID_HANDLE_VALUE) ? 0 : -13;

cleanup:
    if (fout != INVALID_HANDLE_VALUE) {
        CloseHandle(fout);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (fd >= 0) {
        closesocket(fd);
    }
    return ret;
}
