#ifndef HTTPS_CLIENT_H
#define HTTPS_CLIENT_H

#include <stddef.h>

/*
 * Minimal HTTPS client for NXDK (lwIP sockets + mbed TLS, TLS 1.2/1.3).
 * Certificate verification is DISABLED (demo-grade), matching XboxQRCodeLogin.
 */

/* Convenience: POST a JSON body and read the full response (headers + body) into out. */
int https_post_json(const char *hostname, const char *port, const char *path, const char *json_body,
                    char *out, size_t out_sz);

typedef void (*HttpsUploadProgressFn)(size_t sent, size_t total, void *ctx);

/*
 * General request. method e.g. "POST"/"GET". extra_headers is an array of full header
 * lines WITHOUT trailing CRLF (e.g. "X-Session-Key: abc"); pass NULL/0 for none.
 * body may be NULL (body_len 0). The body is streamed directly, so it may be large.
 * upload_progress is called while sending body (optional).
 */
int https_request(const char *hostname, const char *port, const char *method, const char *path,
                  const char *content_type, const char *const *extra_headers, int n_headers,
                  const char *body, size_t body_len, char *out, size_t out_sz,
                  HttpsUploadProgressFn upload_progress, void *upload_progress_ctx);

/*
 * GETs path and streams the response body straight to destPath (created/truncated),
 * so large downloads need not fit in RAM. The HTTP status code is written to
 * *statusOut when non-NULL. Returns 0 only when the status is 2xx and the file was
 * written; a negative value otherwise (the file is not created on a non-2xx status).
 */
int https_get_to_file(const char *hostname, const char *port, const char *path,
                      const char *const *extra_headers, int n_headers, const char *destPath,
                      int *statusOut);

#endif
