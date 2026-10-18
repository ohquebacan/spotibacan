#include "tls3ds.h"
#include "ca_bundle.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#ifdef MBEDTLS_ERROR_C
#include "mbedtls/error.h"
#endif

/* Spotify's hosts all offer ECDHE-RSA-AES128-GCM-SHA256 and nothing here needs
 * to interoperate with anything else, so the list stays short: a smaller
 * ClientHello, and no CBC or SHA-1 suites to fall back onto. ChaCha20 leads
 * because the ARM11 has no AES instructions, so it is the cheaper choice when
 * a server will take it. */
static const int SUITES[] = {
	MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	0
};

/* X25519 first for the same reason, then the NIST curves Spotify actually
 * selects. */
static const uint16_t GROUPS[] = {
	MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
	MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
	MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1,
	MBEDTLS_SSL_IANA_TLS_GROUP_NONE
};

struct tls_conn {
	mbedtls_ssl_context      ssl;
	mbedtls_ssl_config       conf;
	int                      fd;
	int                      timeout_ms;
	char                     host[128];
	/* Where a connection's time goes: waiting in poll() for the network,
	 * versus everything else (decrypting, parsing). Logged on close. */
	u64                      opened_ms;
	u64                      wait_ms;
	size_t                   bytes_in;
};

#define TLS_CONNECT_TIMEOUT_MS 8000

static void (*s_logger)(const char *line);

void tls_set_logger(void (*logger)(const char *line)) { s_logger = logger; }

static struct {
	bool                     ready;
	mbedtls_entropy_context  entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt         anchors;
	int                      anchor_count;
	/* A small ring of session tickets, keyed by host. Spotify's client talks
	 * to a handful of hosts in bursts; with a single slot the art fetches and
	 * API calls kept evicting each other's ticket, forcing a full ECDHE on
	 * every metadata chunk. Eight cover the working set: spclient, i.scdn.co,
	 * two or three audio CDNs, api, login5, lrclib. */
#define TLS_TICKET_SLOTS 8
	mbedtls_ssl_session      ticket[TLS_TICKET_SLOTS];
	char                     ticket_host[TLS_TICKET_SLOTS][128];
	bool                     have_ticket[TLS_TICKET_SLOTS];
	int                      ticket_next;
	/* Every thread that fetches (player, downloads, art, lister) dials
	 * through here at once, and MBEDTLS_THREADING_C is off: the ticket ring
	 * and the DRBG are shared state. Unlocked, a handshake finishing on one
	 * thread freed a ticket another thread was deep-copying, and the heap
	 * corruption surfaced later as a crash inside poll(). */
	LightLock                lock;
} g;

static int locked_drbg(void *p, unsigned char *out, size_t len)
{
	LightLock_Lock(&g.lock);
	int rc = mbedtls_ctr_drbg_random(p, out, len);
	LightLock_Unlock(&g.lock);
	return rc;
}

/* The matching slot, or the next victim when the host has none yet. */
static int ticket_slot(const char *host, bool *found)
{
	for (int i = 0; i < TLS_TICKET_SLOTS; i++)
		if (g.have_ticket[i] && strcmp(g.ticket_host[i], host) == 0) {
			*found = true;
			return i;
		}
	*found = false;
	int slot = g.ticket_next;
	g.ticket_next = (g.ticket_next + 1) % TLS_TICKET_SLOTS;
	return slot;
}

void tls_strerror(int code, char *out, size_t outlen)
{
#ifdef MBEDTLS_ERROR_C
	mbedtls_strerror(code, out, outlen);
	if (out[0])
		return;
#endif
	snprintf(out, outlen, "mbedtls %d (-0x%04X)", code, (unsigned)-code);
}

int tls_anchor_count(void) { return g.anchor_count; }

int tls_global_init(void)
{
	if (g.ready)
		return 0;

	LightLock_Init(&g.lock);
	mbedtls_entropy_init(&g.entropy);
	mbedtls_ctr_drbg_init(&g.drbg);
	mbedtls_x509_crt_init(&g.anchors);
	for (int i = 0; i < TLS_TICKET_SLOTS; i++)
		mbedtls_ssl_session_init(&g.ticket[i]);

	static const char PERS[] = "3ds-spotify";
	int rc = mbedtls_ctr_drbg_seed(&g.drbg, mbedtls_entropy_func, &g.entropy,
	                               (const unsigned char *)PERS, sizeof(PERS) - 1);
	if (rc)
		return rc;

	/* A partial failure is normal and fine: parse() returns the number of
	 * certificates it had to skip, and only a total failure is fatal. */
	rc = mbedtls_x509_crt_parse(&g.anchors,
	                            (const unsigned char *)spotify_ca_bundle,
	                            spotify_ca_bundle_len);
	if (rc < 0)
		return rc;
	for (const mbedtls_x509_crt *c = &g.anchors; c; c = c->next)
		g.anchor_count++;
	if (g.anchor_count == 0)
		return MBEDTLS_ERR_X509_INVALID_FORMAT;

	g.ready = true;
	return 0;
}

void tls_global_exit(void)
{
	if (!g.ready)
		return;
	for (int i = 0; i < TLS_TICKET_SLOTS; i++)
		mbedtls_ssl_session_free(&g.ticket[i]);
	mbedtls_x509_crt_free(&g.anchors);
	mbedtls_ctr_drbg_free(&g.drbg);
	mbedtls_entropy_free(&g.entropy);
	memset(&g, 0, sizeof(g));
}

/* ------------------------------------------------------------------ sockets */

/* libctru's SOC has no usable SO_RCVTIMEO, so every wait goes through poll().
 * The socket stays blocking: one poll plus one recv is two syscalls, and on
 * this console syscalls are the expensive part. */
static int wait_io(int fd, short events, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = events };
	int n = poll(&p, 1, timeout_ms);
	if (n == 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	if (n < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	return 0;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	tls_conn *c = ctx;
	int rc = wait_io(c->fd, POLLOUT, c->timeout_ms);
	if (rc)
		/* Not WANT_WRITE: the write and handshake loops retry that
		 * forever, and a dead link would spin there at one poll a turn. */
		return rc == MBEDTLS_ERR_SSL_TIMEOUT ? MBEDTLS_ERR_SSL_TIMEOUT
		                                     : MBEDTLS_ERR_NET_SEND_FAILED;
	int n = send(c->fd, buf, len, 0);
	if (n < 0)
		return MBEDTLS_ERR_NET_SEND_FAILED;
	return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	tls_conn *c = ctx;
	const u64 t0 = osGetTime();
	int rc = wait_io(c->fd, POLLIN, c->timeout_ms);
	c->wait_ms += osGetTime() - t0;
	if (rc)
		return rc;
	int n = recv(c->fd, buf, len, 0);
	if (n > 0)
		c->bytes_in += (size_t)n;
	if (n == 0)
		return MBEDTLS_ERR_SSL_CONN_EOF;
	if (n < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	return n;
}

int tls_connect_timeout(int fd, const struct sockaddr *addr,
                        unsigned addrlen, int timeout_ms)
{
	/* A plain blocking connect. The non-blocking + poll version (1.6 test
	 * build) made every connection fail on hardware -- sign-in died with
	 * "no internet connection" -- so the deadline waits for a way that is
	 * proven on the console. */
	(void)timeout_ms;
	return connect(fd, addr, addrlen);
}

static int dial(const char *host, u16 port, int timeout_ms,
                tls_stats *st, char *err, size_t errlen)
{
	char service[8];
	snprintf(service, sizeof(service), "%u", port);

	TickCounter t;
	osTickCounterStart(&t);

	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct addrinfo *res = NULL;
	if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
		snprintf(err, errlen, "DNS failed for %s", host);
		return -1;
	}
	osTickCounterUpdate(&t);
	if (st)
		st->dns_ms = osTickCounterRead(&t);

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		freeaddrinfo(res);
		snprintf(err, errlen, "socket() failed, errno %d", errno);
		return -1;
	}

	osTickCounterStart(&t);
	/* SOC's connect() has no timeout of its own; a wrong port therefore hangs
	 * until the console's stack gives up. Spotify's access point on 443 does
	 * exactly that from some networks, so this is not a hypothetical. */
	if (tls_connect_timeout(fd, res->ai_addr, res->ai_addrlen,
	                        TLS_CONNECT_TIMEOUT_MS) < 0) {
		snprintf(err, errlen, "connect() to %s:%u failed, errno %d",
		         host, port, errno);
		close(fd);
		freeaddrinfo(res);
		return -1;
	}
	osTickCounterUpdate(&t);
	if (st)
		st->connect_ms = osTickCounterRead(&t);

	freeaddrinfo(res);
	(void)timeout_ms;
	return fd;
}

/* ---------------------------------------------------------------- handshake */

static tls_conn *connect_impl(const char *host, u16 port, int timeout_ms,
                              bool verify, tls_stats *stats, char *err,
                              size_t errlen)
{
	tls_stats local = {0};
	tls_stats *st = stats ? stats : &local;
	memset(st, 0, sizeof(*st));
	if (errlen)
		err[0] = '\0';

	if (!g.ready) {
		snprintf(err, errlen, "tls_global_init() was not called");
		return NULL;
	}

	tls_conn *c = calloc(1, sizeof(*c));
	if (!c) {
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	c->timeout_ms = timeout_ms;
	snprintf(c->host, sizeof(c->host), "%s", host);
	c->opened_ms = osGetTime();

	c->fd = dial(host, port, timeout_ms, st, err, errlen);
	if (c->fd < 0) {
		free(c);
		return NULL;
	}

	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);

	int rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
	                                     MBEDTLS_SSL_TRANSPORT_STREAM,
	                                     MBEDTLS_SSL_PRESET_DEFAULT);
	if (rc)
		goto fail;

	mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_authmode(&c->conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED
	                                           : MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_ca_chain(&c->conf, &g.anchors, NULL);
	mbedtls_ssl_conf_rng(&c->conf, locked_drbg, &g.drbg);
	mbedtls_ssl_conf_ciphersuites(&c->conf, SUITES);
	mbedtls_ssl_conf_groups(&c->conf, GROUPS);
	mbedtls_ssl_conf_session_tickets(&c->conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
	/* 16 KB in, 4 KB out, as configured; asking for more would be refused. */
	mbedtls_ssl_conf_read_timeout(&c->conf, timeout_ms);

	if ((rc = mbedtls_ssl_setup(&c->ssl, &c->conf)))
		goto fail;
	/* Without SNI, Spotify's edge serves the wrong certificate; this call also
	 * arms the hostname check against the leaf. */
	if (verify && (rc = mbedtls_ssl_set_hostname(&c->ssl, host)))
		goto fail;
	bool found = false;
	int slot = -1;
	if (verify) {
		LightLock_Lock(&g.lock);
		slot = ticket_slot(host, &found);
		if (found)
			mbedtls_ssl_set_session(&c->ssl, &g.ticket[slot]);
		LightLock_Unlock(&g.lock);
	}

	mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);

	TickCounter t;
	osTickCounterStart(&t);
	while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
			break;
	}
	osTickCounterUpdate(&t);
	st->handshake_ms = osTickCounterRead(&t);

	if (rc) {
		/* A chain that did not verify is the interesting case, so report the
		 * flags rather than just "handshake failed". A console with an unset
		 * clock (fresh battery, never configured) fails every certificate
		 * with FUTURE/EXPIRED: say so plainly, it is the most common
		 * setup mistake and "handshake error" sends people nowhere. */
		st->verify_flags = mbedtls_ssl_get_verify_result(&c->ssl);
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		if (st->verify_flags & (MBEDTLS_X509_BADCERT_FUTURE |
		                        MBEDTLS_X509_BADCERT_EXPIRED))
			snprintf(err, errlen, TLS_ERR_CLOCK);
		else
			snprintf(err, errlen, "handshake: %s (verify 0x%08lX)",
			         reason, (unsigned long)st->verify_flags);
		goto drop;
	}

	st->verify_flags = mbedtls_ssl_get_verify_result(&c->ssl);
	snprintf(st->version, sizeof(st->version), "%s",
	         mbedtls_ssl_get_version(&c->ssl));
	snprintf(st->ciphersuite, sizeof(st->ciphersuite), "%s",
	         mbedtls_ssl_get_ciphersuite(&c->ssl));
	st->resumed = mbedtls_ssl_is_handshake_over(&c->ssl) && found;

	const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);
	if (peer)
		mbedtls_x509_dn_gets(st->peer_cn, sizeof(st->peer_cn), &peer->subject);

	/* Keep the ticket for the next connection to this host. The slot lookup
	 * already ran before the handshake: a match is refreshed in place, a miss
	 * lands on the ring's next victim. */
	if (slot < 0)
		return c;
	LightLock_Lock(&g.lock);
	/* Another thread may have handed this slot to a different host while
	 * the handshake ran; storing still just overwrites one cache entry. */
	mbedtls_ssl_session_free(&g.ticket[slot]);
	mbedtls_ssl_session_init(&g.ticket[slot]);
	if (mbedtls_ssl_get_session(&c->ssl, &g.ticket[slot]) == 0) {
		snprintf(g.ticket_host[slot], sizeof(g.ticket_host[slot]), "%s", host);
		g.have_ticket[slot] = true;
	} else {
		g.have_ticket[slot] = false;
	}
	LightLock_Unlock(&g.lock);

	return c;

fail: {
	char reason[128];
	tls_strerror(rc, reason, sizeof(reason));
	snprintf(err, errlen, "setup: %s", reason);
}
drop:
	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	close(c->fd);
	free(c);
	return NULL;
}

int tls_write(tls_conn *c, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t sent = 0;
	while (sent < len) {
		int n = mbedtls_ssl_write(&c->ssl, p + sent, len - sent);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (n <= 0)
			return n;
		sent += n;
	}
	return (int)sent;
}

int tls_read(tls_conn *c, void *buf, size_t len)
{
	for (;;) {
		int n = mbedtls_ssl_read(&c->ssl, buf, len);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		return n;
	}
}

void tls_close(tls_conn *c)
{
	if (!c)
		return;
	/* Only connections that moved real data: the tiny API calls would
	 * drown the log. */
	if (s_logger && c->bytes_in >= 32 * 1024) {
		const u64 total = osGetTime() - c->opened_ms;
		char line[200];
		snprintf(line, sizeof(line),
		         "TLS: %s %u KB in %llu ms, %llu ms waiting on the network, "
		         "%llu ms working\n",
		         c->host, (unsigned)(c->bytes_in / 1024),
		         (unsigned long long)total, (unsigned long long)c->wait_ms,
		         (unsigned long long)(total > c->wait_ms ? total - c->wait_ms : 0));
		s_logger(line);
	}
	mbedtls_ssl_close_notify(&c->ssl);
	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	close(c->fd);
	free(c);
}

tls_conn *tls_connect(const char *host, u16 port, int timeout_ms,
                      tls_stats *stats, char *err, size_t errlen)
{
	return connect_impl(host, port, timeout_ms, true, stats, err, errlen);
}

tls_conn *tls_connect_lan(const char *ip, u16 port, int timeout_ms,
                          char *err, size_t errlen)
{
	return connect_impl(ip, port, timeout_ms, false, NULL, err, errlen);
}
