/* Minimal HTTP/1.1 over tls3ds: what the Spotify endpoints actually need.
 *
 * GET and form POST, Content-Length and chunked bodies, one redirect hop. No
 * keep-alive: Spotify's token endpoint closes anyway, and a fresh connection
 * reuses the session ticket tls3ds already caches.
 */
#ifndef HTTP3DS_H
#define HTTP3DS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	int    status;        /* HTTP status, or 0 if the request never completed */
	char  *body;          /* NUL-terminated; caller frees */
	size_t len;
	int    retry_after;   /* seconds, from a 429's Retry-After; 0 otherwise */
	char   err[160];
} http_reply;

/* `body` is sent as application/x-www-form-urlencoded. Caller frees reply.body
 * on success. Returns 0 when a response was read, -1 otherwise -- note that an
 * HTTP 400 is a successful read: these endpoints report errors as JSON at 400. */
int http_post_form(const char *host, const char *path, const char *body,
                   http_reply *out);
int http_get(const char *host, const char *path, const char *bearer,
             http_reply *out);

/* Spotify's internal services speak protobuf over HTTPS, with the body and the
 * reply both binary; `out->body` is still NUL-terminated for convenience but
 * `out->len` is what counts. */
int http_post_proto(const char *host, const char *path,
                    const void *body, size_t len, http_reply *out);
int http_get_proto(const char *host, const char *path, const char *bearer,
                   http_reply *out);

/* The general form the ones above are built from. `bearer`, `ctype` and
 * `accept` may be NULL. `limit` caps the response body; 0 uses the default,
 * which is sized for API replies, not for audio. */
int http_request(const char *host, const char *path, const char *method,
                 const char *bearer, const char *ctype, const char *accept,
                 const void *body, size_t len, size_t limit, http_reply *out);

/* GET a full https:// URL, for the signed CDN links storage-resolve hands
 * back: those carry host, path and a query that must be passed through
 * untouched. `limit` is required here -- an audio file is megabytes. */
int http_get_url(const char *url, size_t limit, http_reply *out);

/* The same, asking the CDN for only the first `bytes` of the file. Enough to
 * start playing -- and, for a probe, enough to prove the chain without waiting
 * on megabytes. 0 means the whole file. */
int http_get_url_range(const char *url, size_t bytes, size_t limit,
                       http_reply *out);

/* Streaming variant for the audio path: instead of buffering the whole
 * response, every piece of body goes to `cb` as it arrives, and the piece is
 * only considered consumed when the callback returns true -- false aborts the
 * request, which is how a track skip cancels a download mid-flight.
 *
 * `offset` resumes from that byte (a 206); 0 asks for the whole file (a 200).
 * Either way the reply headers carry the file's total size, stored in
 * `total_out` before the first callback, which is when the caller reserves
 * its buffer. A resume answered with 200 restarts from byte zero and would
 * corrupt the buffer, so it is refused here. The CDN answers with a length;
 * a chunked body is not handled. */
int http_get_url_stream(const char *url, size_t offset,
                        bool (*cb)(void *ctx, const char *data, size_t n),
                        void *ctx, size_t *total_out, int *status_out,
                        char *err, size_t errlen);
void http_reply_free(http_reply *r);

/* Keep-alive session: one TLS connection, many sequential requests to the
 * same host. Naming a 500-song list costs a full handshake per 50-track
 * chunk otherwise (~3 s each on this console); over a session only the
 * first chunk pays it. A request that finds the server dropped an idle
 * connection reconnects and retries once, transparently. */
typedef struct http_session http_session;
http_session *http_session_open(const char *host);
int  http_session_request(http_session *s, const char *path,
                          const char *method, const char *bearer,
                          const char *ctype, const char *accept,
                          const void *body, size_t len, size_t limit,
                          http_reply *out);
void http_session_close(http_session *s);
/* 1 when the last request reused the open link, 0 when it reconnected. */
int  http_session_last_reused(const http_session *s);
/* The last request's split: waiting for the server, then reading the body. */
void http_session_last_timing(const http_session *s, unsigned *wait_ms,
                              unsigned *body_ms);

/* Percent-encodes everything outside the unreserved set, for form bodies. */
size_t url_encode(const char *in, char *out, size_t outlen);

#endif
