#include "http3ds.h"
#include "tls3ds.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RECV_CHUNK  4096
/* Enough for any API reply. Audio passes its own, far larger limit. */
#define BODY_MAX    (512 * 1024)

/* newlib hides strcasestr behind _GNU_SOURCE; header names are few and short,
 * so a plain scan is cheaper than reasoning about feature macros. */
static bool header_present(const char *headers, const char *needle)
{
	size_t n = strlen(needle);
	for (; *headers; headers++) {
		size_t i = 0;
		while (i < n && headers[i] &&
		       tolower((unsigned char)headers[i]) == needle[i])
			i++;
		if (i == n)
			return true;
	}
	return false;
}

/* Defined below, beside the streaming reader that also uses it. */
static const char *find_header(const char *headers, const char *name);

size_t url_encode(const char *in, char *out, size_t outlen)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t n = 0;
	for (; *in; in++) {
		unsigned char c = (unsigned char)*in;
		bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		            c == '.' || c == '~';
		if (safe) {
			if (n + 2 > outlen)
				break;
			out[n++] = (char)c;
		} else {
			if (n + 4 > outlen)
				break;
			out[n++] = '%';
			out[n++] = HEX[c >> 4];
			out[n++] = HEX[c & 15];
		}
	}
	out[n < outlen ? n : outlen - 1] = '\0';
	return n;
}

void http_reply_free(http_reply *r)
{
	if (!r)
		return;
	free(r->body);
	r->body = NULL;
	r->len = 0;
}

/* Reads the whole response into one buffer. These responses are small; the
 * audio path later streams instead of doing this.
 *
 * Reads land in RECV_CHUNK blocks rather than byte by byte: on this console the
 * cost is the number of syscalls, not the number of bytes. */
static char *slurp(tls_conn *c, size_t *len, size_t limit, char *err, size_t errlen)
{
	size_t cap = 8192, have = 0;
	char *buf = malloc(cap);
	if (!buf) {
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	for (;;) {
		if (have + RECV_CHUNK + 1 > cap) {
			if (cap >= limit) {
				snprintf(err, errlen, "response over %u bytes",
				         (unsigned)limit);
				free(buf);
				return NULL;
			}
			size_t want = cap * 2;
			char *bigger = realloc(buf, want);
			if (!bigger) {
				snprintf(err, errlen, "out of memory at %u bytes", (unsigned)want);
				free(buf);
				return NULL;
			}
			buf = bigger;
			cap = want;
		}
		int n = tls_read(c, buf + have, RECV_CHUNK);
		if (n == 0)
			break;                 /* the server closed, which is the signal */
		if (n < 0) {
			char reason[128];
			tls_strerror(n, reason, sizeof(reason));
			snprintf(err, errlen, "read: %s", reason);
			free(buf);
			return NULL;
		}
		have += n;
	}
	buf[have] = '\0';
	*len = have;
	return buf;
}

/* Rewrites a chunked body in place; returns the decoded length. */
static size_t dechunk(char *p, size_t len)
{
	char *out = p, *in = p;
	const char *end = p + len;
	while (in < end) {
		char *nl = memchr(in, '\n', end - in);
		if (!nl)
			break;
		unsigned long size = strtoul(in, NULL, 16);
		in = nl + 1;
		/* Compare lengths, not pointers: a huge size wraps in + size on a
		 * 32-bit address space and slips past an end-pointer check. */
		if (size == 0 || size > (unsigned long)(end - in))
			break;
		memmove(out, in, size);
		out += size;
		in += size;
		/* Step over the CRLF that closes the chunk. */
		while (in < end && (*in == '\r' || *in == '\n'))
			in++;
	}
	*out = '\0';
	return (size_t)(out - p);
}

static int finish(tls_conn *c, size_t limit, http_reply *out)
{
	size_t total = 0;
	char *raw = slurp(c, &total, limit, out->err, sizeof(out->err));
	tls_close(c);
	if (!raw)
		return -1;

	if (sscanf(raw, "HTTP/1.%*d %d", &out->status) != 1) {
		snprintf(out->err, sizeof(out->err), "no status line");
		free(raw);
		return -1;
	}

	char *sep = strstr(raw, "\r\n\r\n");
	if (!sep) {
		snprintf(out->err, sizeof(out->err), "headers never ended");
		free(raw);
		return -1;
	}
	*sep = '\0';
	char *body = sep + 4;
	size_t blen = total - (size_t)(body - raw);

	/* Compared case-insensitively: edges title-case header names differently. */
	bool chunked = header_present(raw, "transfer-encoding: chunked");
	if (chunked)
		blen = dechunk(body, blen);

	/* 429s carry how long the quota wants us to wait; surface it so callers
	 * can retry on the server's clock instead of a guessed backoff. */
	{
		const char *ra = find_header(raw, "retry-after:");
		if (ra)
			out->retry_after = atoi(ra);
	}

	/* Hand back just the body, so callers never have to think about headers. */
	char *only = malloc(blen + 1);
	if (!only) {
		snprintf(out->err, sizeof(out->err), "out of memory");
		free(raw);
		return -1;
	}
	memcpy(only, body, blen);
	only[blen] = '\0';
	free(raw);

	out->body = only;
	out->len = blen;
	return 0;
}

/* Set for the next request only; the CDN is the only caller that needs it. */
static size_t pending_range;

int http_request(const char *host, const char *path, const char *method,
                 const char *bearer, const char *ctype, const char *accept,
                 const void *body, size_t len, size_t limit, http_reply *out)
{
	memset(out, 0, sizeof(*out));
	if (limit == 0)
		limit = BODY_MAX;
	tls_conn *c = tls_connect(host, 443, 20000, NULL, out->err, sizeof(out->err));
	if (!c)
		return -1;

	/* A bearer token is long -- login5's runs past 400 characters -- so the
	 * header buffer is sized for it rather than for a typical request. */
	size_t headcap = 1024 + (bearer ? strlen(bearer) : 0) + strlen(path);
	char *head = malloc(headcap);
	if (!head) {
		snprintf(out->err, sizeof(out->err), "out of memory");
		tls_close(c);
		return -1;
	}

	/* The range goes into its own buffer rather than being spliced in with
	 * ternaries: a %u conversion cannot be skipped the way a %s can, so with
	 * no range set it still printed a stray "0" into the headers and
	 * malformed every single request. */
	char range[48] = "";
	if (pending_range)
		snprintf(range, sizeof(range), "Range: bytes=0-%u\r\n",
		         (unsigned)(pending_range - 1));
	pending_range = 0;

	int n = snprintf(head, headcap,
	                 "%s %s HTTP/1.1\r\nHost: %s\r\n"
	                 "User-Agent: 3ds-spotify/0.1\r\n"
	                 "%s%s%s"
	                 "%s%s%s"
	                 "%s%s%s"
	                 "%s"
	                 "Content-Length: %u\r\nConnection: close\r\n\r\n",
	                 method, path, host,
	                 bearer ? "Authorization: Bearer " : "", bearer ? bearer : "",
	                 bearer ? "\r\n" : "",
	                 ctype ? "Content-Type: " : "", ctype ? ctype : "",
	                 ctype ? "\r\n" : "",
	                 accept ? "Accept: " : "", accept ? accept : "",
	                 accept ? "\r\n" : "",
	                 range,
	                 (unsigned)len);
	if (n < 0 || (size_t)n >= headcap) {
		snprintf(out->err, sizeof(out->err), "request headers too long");
		free(head);
		tls_close(c);
		return -1;
	}

	bool sent = tls_write(c, head, n) == n &&
	            (len == 0 || tls_write(c, body, len) == (int)len);
	free(head);
	if (!sent) {
		snprintf(out->err, sizeof(out->err), "write failed");
		tls_close(c);
		return -1;
	}
	return finish(c, limit, out);
}

int http_post_form(const char *host, const char *path, const char *body,
                   http_reply *out)
{
	return http_request(host, path, "POST", NULL,
	                    "application/x-www-form-urlencoded", NULL,
	                    body, strlen(body), 0, out);
}

int http_get(const char *host, const char *path, const char *bearer,
             http_reply *out)
{
	return http_request(host, path, "GET", bearer, NULL, NULL, NULL, 0, 0, out);
}

int http_post_proto(const char *host, const char *path,
                    const void *body, size_t len, http_reply *out)
{
	return http_request(host, path, "POST", NULL, "application/x-protobuf",
	                    "application/x-protobuf", body, len, 0, out);
}

int http_get_proto(const char *host, const char *path, const char *bearer,
                   http_reply *out)
{
	return http_request(host, path, "GET", bearer, NULL,
	                    "application/x-protobuf", NULL, 0, 0, out);
}

int http_get_url(const char *url, size_t limit, http_reply *out)
{
	memset(out, 0, sizeof(*out));
	if (strncmp(url, "https://", 8) != 0) {
		snprintf(out->err, sizeof(out->err), "not an https url");
		return -1;
	}

	const char *p = url + 8;
	const char *slash = strchr(p, '/');
	if (!slash) {
		snprintf(out->err, sizeof(out->err), "url has no path");
		return -1;
	}
	char host[160];
	size_t hlen = (size_t)(slash - p);
	if (hlen >= sizeof(host)) {
		snprintf(out->err, sizeof(out->err), "host too long");
		return -1;
	}
	memcpy(host, p, hlen);
	host[hlen] = '\0';

	/* The path keeps its query exactly as given: the CDN signature covers it. */
	return http_request(host, slash, "GET", NULL, NULL, NULL, NULL, 0,
	                    limit, out);
}

int http_get_url_range(const char *url, size_t bytes, size_t limit,
                       http_reply *out)
{
	pending_range = bytes;
	int rc = http_get_url(url, limit, out);
	pending_range = 0;
	return rc;
}

/* Finds a header's value, anchored to a line start and case-insensitive:
 * edges title-case header names differently. */
static const char *find_header(const char *headers, const char *name)
{
	size_t n = strlen(name);
	for (const char *p = headers; *p; p++)
		if ((p == headers || p[-1] == '\n') &&
		    strncasecmp(p, name, n) == 0)
			return p + n;
	return NULL;
}

int http_get_url_stream(const char *url, size_t offset,
                        bool (*cb)(void *ctx, const char *data, size_t n),
                        void *ctx, size_t *total_out, int *status_out,
                        char *err, size_t errlen)
{
	if (strncmp(url, "https://", 8) != 0) {
		snprintf(err, errlen, "not an https url");
		return -1;
	}
	const char *p = url + 8;
	const char *slash = strchr(p, '/');
	if (!slash) {
		snprintf(err, errlen, "url has no path");
		return -1;
	}
	char host[160];
	size_t hlen = (size_t)(slash - p);
	if (hlen >= sizeof(host)) {
		snprintf(err, errlen, "host too long");
		return -1;
	}
	memcpy(host, p, hlen);
	host[hlen] = '\0';

	tls_conn *c = tls_connect(host, 443, 20000, NULL, err, errlen);
	if (!c)
		return -1;

	char range[48] = "";
	if (offset)
		snprintf(range, sizeof(range), "Range: bytes=%u-\r\n",
		         (unsigned)offset);
	char head[1024];
	int n = snprintf(head, sizeof(head),
	                 "GET %s HTTP/1.1\r\nHost: %s\r\n"
	                 "User-Agent: 3ds-spotify/0.1\r\n%s"
	                 "Connection: close\r\n\r\n",
	                 slash, host, range);
	if (n < 0 || (size_t)n >= sizeof(head) ||
	    tls_write(c, head, n) != n) {
		snprintf(err, errlen, "write failed");
		tls_close(c);
		return -1;
	}

	/* Headers first: they end at the blank line, and whatever the same
	 * reads dragged in past it is already body. */
	char hbuf[8192];
	size_t have = 0;
	char *sep = NULL;
	while (!sep) {
		if (have >= sizeof(hbuf) - 1) {
			snprintf(err, errlen, "headers over %u bytes",
			         (unsigned)sizeof(hbuf));
			tls_close(c);
			return -1;
		}
		int r = tls_read(c, hbuf + have, sizeof(hbuf) - 1 - have);
		if (r <= 0) {
			snprintf(err, errlen, "connection closed before the headers");
			tls_close(c);
			return -1;
		}
		have += (size_t)r;
		hbuf[have] = '\0';
		sep = strstr(hbuf, "\r\n\r\n");
	}
	*sep = '\0';

	int status = 0;
	if (sscanf(hbuf, "HTTP/1.%*d %d", &status) != 1) {
		snprintf(err, errlen, "no status line");
		tls_close(c);
		return -1;
	}
	if (status_out)
		*status_out = status;
	if (status != 200 && status != 206) {
		snprintf(err, errlen, "CDN answered %d", status);
		tls_close(c);
		return -1;
	}
	if (offset && status == 200) {
		snprintf(err, errlen, "edge ignored the range");
		tls_close(c);
		return -1;
	}

	size_t total = 0;
	const char *cr = find_header(hbuf, "content-range:");
	if (cr) {
		cr = strchr(cr, '/');
		if (cr)
			total = strtoul(cr + 1, NULL, 10);
	}
	if (!total) {
		const char *cl = find_header(hbuf, "content-length:");
		if (cl)
			total = strtoul(cl, NULL, 10) + offset;
	}
	if (total_out)
		*total_out = total;

	/* Body: first the piece the header reads overshot, then straight reads. */
	const char *body = sep + 4;
	size_t blen = have - (size_t)(body - hbuf);
	/* The callback may have said why it stopped; keep that. */
	if (blen && !cb(ctx, body, blen)) {
		if (!err[0])
			snprintf(err, errlen, "cancelled");
		tls_close(c);
		return -1;
	}
	/* A whole TLS record per read on the long CDN body: a quarter of the
	 * calls (and callbacks) of RECV_CHUNK pieces. Heap, not stack: this
	 * runs on worker threads with modest stacks. */
	enum { STREAM_PIECE = 16 * 1024 };
	char *piece = malloc(STREAM_PIECE);
	if (!piece) {
		snprintf(err, errlen, "out of memory");
		tls_close(c);
		return -1;
	}
	int rc = 0;
	for (;;) {
		int r = tls_read(c, piece, STREAM_PIECE);
		if (r == 0)
			break;
		if (r < 0) {
			char reason[128];
			tls_strerror(r, reason, sizeof(reason));
			snprintf(err, errlen, "read: %s", reason);
			rc = -1;
			break;
		}
		if (!cb(ctx, piece, (size_t)r)) {
			if (!err[0])
				snprintf(err, errlen, "cancelled");
			rc = -1;
			break;
		}
	}
	free(piece);
	tls_close(c);
	return rc;
}

/* ---------------- keep-alive sessions ---------------- */

struct http_session {
	tls_conn *c;
	char      host[160];
	char      carry[RECV_CHUNK];
	size_t    carry_len;
	int       reused_last;   /* the last request rode an open connection */
	unsigned  wait_ms;       /* request sent -> status line (server time) */
	unsigned  body_ms;       /* status line -> body complete (wire + CPU) */
};

http_session *http_session_open(const char *host)
{
	http_session *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	snprintf(s->host, sizeof(s->host), "%s", host);
	return s;   /* the connection is opened lazily by the first request */
}

void http_session_close(http_session *s)
{
	if (!s)
		return;
	if (s->c)
		tls_close(s->c);
	free(s);
}

/* One RECV_CHUNK of carry: parsing never reads past what the wire gave. */
static size_t sess_fill(http_session *s)
{
	if (s->carry_len)
		return s->carry_len;
	int n = tls_read(s->c, s->carry, sizeof(s->carry));
	if (n <= 0)
		return 0;
	s->carry_len = (size_t)n;
	return s->carry_len;
}

/* Reads one CRLF-terminated line, returned without it. -1 on EOF/error. */
static int sess_line(http_session *s, char *out, size_t cap)
{
	size_t n = 0;
	for (;;) {
		if (!sess_fill(s))
			return -1;
		for (size_t i = 0; i < s->carry_len; i++) {
			char ch = s->carry[i];
			if (ch == '\n') {
				memmove(s->carry, s->carry + i + 1,
				        s->carry_len - i - 1);
				s->carry_len -= i + 1;
				if (n && out[n - 1] == '\r')
					n--;
				out[n] = '\0';
				return 0;
			}
			if (n + 1 < cap)
				out[n++] = ch;
		}
		s->carry_len = 0;
	}
}

/* Reads exactly n bytes, carry first, then the wire. -1 on EOF/error. */
static int sess_exact(http_session *s, char *dst, size_t n)
{
	while (n) {
		if (!sess_fill(s))
			return -1;
		size_t take = s->carry_len < n ? s->carry_len : n;
		memcpy(dst, s->carry, take);
		memmove(s->carry, s->carry + take, s->carry_len - take);
		s->carry_len -= take;
		dst += take;
		n -= take;
	}
	return 0;
}

/* Grows *buf to room for extra bytes; 0 ok, -1 over limit / out of memory. */
static int sess_grow(char **buf, size_t *cap, size_t have, size_t extra,
                     size_t limit, char *err, size_t errlen)
{
	/* Sizes come off the wire (Content-Length, chunk sizes): reject before
	 * any addition can wrap a 32-bit size_t into a value that passes. */
	if (have >= limit || extra >= limit - have) {
		snprintf(err, errlen, "response over %u bytes", (unsigned)limit);
		return -1;
	}
	if (have + extra + 1 <= *cap)
		return 0;
	size_t want = *cap ? *cap : 8192;
	while (want < have + extra + 1)
		want *= 2;
	if (want > limit) {
		snprintf(err, errlen, "response over %u bytes", (unsigned)limit);
		return -1;
	}
	char *bigger = realloc(*buf, want);
	if (!bigger) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	*buf = bigger;
	*cap = want;
	return 0;
}

/* One request over the session's connection. stale is set when the failure
 * happened before any reply byte: the server dropped an idle connection and
 * the caller may retry on a fresh one. Any mid-body failure poisons the
 * stream's framing, so the connection is closed either way. */
static int sess_attempt(http_session *s, const char *path, const char *method,
                        const char *bearer, const char *ctype,
                        const char *accept, const void *body, size_t len,
                        size_t limit, http_reply *out, bool *stale)
{
	*stale = false;
	s->reused_last = s->c != NULL;
	if (!s->c) {
		s->carry_len = 0;
		s->c = tls_connect(s->host, 443, 20000, NULL, out->err,
		                   sizeof(out->err));
		if (!s->c)
			return -1;
	}

	size_t headcap = 1024 + (bearer ? strlen(bearer) : 0) + strlen(path);
	char *head = malloc(headcap);
	if (!head) {
		snprintf(out->err, sizeof(out->err), "out of memory");
		return -1;
	}
	int n = snprintf(head, headcap,
	                 "%s %s HTTP/1.1\r\nHost: %s\r\n"
	                 "User-Agent: 3ds-spotify/0.1\r\n"
	                 "%s%s%s"
	                 "%s%s%s"
	                 "%s%s%s"
	                 "Content-Length: %u\r\n\r\n",
	                 method, path, s->host,
	                 bearer ? "Authorization: Bearer " : "", bearer ? bearer : "",
	                 bearer ? "\r\n" : "",
	                 ctype ? "Content-Type: " : "", ctype ? ctype : "",
	                 ctype ? "\r\n" : "",
	                 accept ? "Accept: " : "", accept ? accept : "",
	                 accept ? "\r\n" : "",
	                 (unsigned)len);
	if (n < 0 || (size_t)n >= headcap) {
		snprintf(out->err, sizeof(out->err), "request headers too long");
		free(head);
		return -1;
	}
	bool sent = tls_write(s->c, head, n) == n &&
	            (len == 0 || tls_write(s->c, body, len) == (int)len);
	free(head);
	if (!sent) {
		snprintf(out->err, sizeof(out->err), "write failed");
		*stale = true;
		goto drop;
	}

	char line[512];
	const u64 t_sent = osGetTime();
	if (sess_line(s, line, sizeof(line)) != 0) {
		snprintf(out->err, sizeof(out->err), "connection dropped idle");
		*stale = true;
		goto drop;
	}
	if (sscanf(line, "HTTP/1.%*d %d", &out->status) != 1) {
		snprintf(out->err, sizeof(out->err), "no status line");
		goto drop;
	}

	size_t clen = 0;
	bool have_len = false, chunked = false, conn_close = false;
	for (;;) {
		if (sess_line(s, line, sizeof(line)) != 0) {
			snprintf(out->err, sizeof(out->err), "headers never ended");
			goto drop;
		}
		if (!line[0])
			break;
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			clen = strtoul(line + 15, NULL, 10);
			have_len = true;
		} else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
		           strstr(line + 18, "chunked")) {
			chunked = true;
		} else if (strncasecmp(line, "Connection:", 11) == 0 &&
		           strstr(line + 11, "close")) {
			conn_close = true;
		} else if (strncasecmp(line, "Retry-After:", 12) == 0) {
			out->retry_after = atoi(line + 12);
		}
	}

	const u64 t_head = osGetTime();
	s->wait_ms = (unsigned)(t_head - t_sent);
	char *buf = NULL;
	size_t cap = 0, have = 0;
	if (chunked) {
		for (;;) {
			if (sess_line(s, line, sizeof(line)) != 0) {
				snprintf(out->err, sizeof(out->err), "chunk size missing");
				goto fail;
			}
			unsigned long sz = strtoul(line, NULL, 16);
			if (!sz) {
				/* Trailers, if any, end at a blank line. */
				do {
					if (sess_line(s, line, sizeof(line)) != 0)
						break;
				} while (line[0]);
				break;
			}
			if (sess_grow(&buf, &cap, have, sz, limit, out->err,
			              sizeof(out->err)) != 0)
				goto fail;
			if (sess_exact(s, buf + have, sz) != 0) {
				snprintf(out->err, sizeof(out->err), "chunk cut short");
				goto fail;
			}
			have += sz;
			char crlf[2];
			if (sess_exact(s, crlf, 2) != 0) {
				snprintf(out->err, sizeof(out->err), "chunk cut short");
				goto fail;
			}
		}
	} else if (have_len) {
		if (sess_grow(&buf, &cap, 0, clen, limit, out->err,
		              sizeof(out->err)) != 0)
			goto fail;
		if (sess_exact(s, buf, clen) != 0) {
			snprintf(out->err, sizeof(out->err), "body cut short");
			goto fail;
		}
		have = clen;
	} else {
		/* No framing: the body runs to EOF, which also ends the session. */
		conn_close = true;
		for (;;) {
			if (sess_grow(&buf, &cap, have, RECV_CHUNK, limit, out->err,
			              sizeof(out->err)) != 0)
				goto fail;
			size_t got = 0;
			if (sess_fill(s)) {
				got = s->carry_len;
				memcpy(buf + have, s->carry, got);
				s->carry_len = 0;
			}
			if (!got)
				break;
			have += got;
		}
	}
	if (!buf) {
		buf = malloc(1);
		if (!buf) {
			snprintf(out->err, sizeof(out->err), "out of memory");
			goto fail;
		}
	}
	buf[have] = '\0';
	out->body = buf;
	out->len = have;
	s->body_ms = (unsigned)(osGetTime() - t_head);
	if (conn_close) {
		tls_close(s->c);
		s->c = NULL;
		s->carry_len = 0;
	}
	return 0;

fail:
	free(buf);
drop:
	tls_close(s->c);
	s->c = NULL;
	s->carry_len = 0;
	return -1;
}

int http_session_request(http_session *s, const char *path, const char *method,
                         const char *bearer, const char *ctype,
                         const char *accept, const void *body, size_t len,
                         size_t limit, http_reply *out)
{
	memset(out, 0, sizeof(*out));
	if (limit == 0)
		limit = BODY_MAX;
	bool stale = false;
	if (sess_attempt(s, path, method, bearer, ctype, accept, body, len,
	                 limit, out, &stale) == 0)
		return 0;
	if (!stale)
		return -1;
	/* The server closed an idle keep-alive link: one retry on a fresh
	 * connection, then the error stands. */
	char first[160];
	snprintf(first, sizeof(first), "%s", out->err);
	memset(out, 0, sizeof(*out));
	if (sess_attempt(s, path, method, bearer, ctype, accept, body, len,
	                 limit, out, &stale) == 0)
		return 0;
	return -1;
}

int http_session_last_reused(const http_session *s)
{
	return s ? s->reused_last : 0;
}

void http_session_last_timing(const http_session *s, unsigned *wait_ms,
                              unsigned *body_ms)
{
	*wait_ms = s ? s->wait_ms : 0;
	*body_ms = s ? s->body_ms : 0;
}
