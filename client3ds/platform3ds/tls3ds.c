#include "tls3ds.h"
#include "ca_bundle.h"
#include "http3ds.h"

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
};

static struct {
	bool                     ready;
	mbedtls_entropy_context  entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt         anchors;
	int                      anchor_count;
	/* A small ring of session tickets, keyed by host. Spotify's client talks
	 * to a handful of hosts in bursts; with a single slot the art fetches and
	 * API calls kept evicting each other's ticket, forcing a full ECDHE on
	 * every metadata chunk. Four slots cover the working set. */
#define TLS_TICKET_SLOTS 4
	mbedtls_ssl_session      ticket[TLS_TICKET_SLOTS];
	char                     ticket_host[TLS_TICKET_SLOTS][128];
	bool                     have_ticket[TLS_TICKET_SLOTS];
	int                      ticket_next;
	/* The ring and the DRBG below are shared by every network thread --
	 * player, metadata fetch, download queue, art worker -- and mbedTLS
	 * contexts are not thread-safe: one handshake freeing or rewriting a
	 * slot while another was copying it surfaced as a data abort in
	 * poll() when tracks were skipped quickly. One lock covers both; no
	 * critical section nests inside another. */
	LightLock                lock;
} g;

/* The matching slot, or the next victim when the host has none yet.
 * g.lock must be held: the ring is shared by all network threads. */
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

/* newlib's resolver is not reentrant; see dial(). Lives here because dial
 * does, not in the globals struct: tls_global_exit() memsets that one. */
static LightLock dns_lock;

int tls_global_init(void)
{
	/* (Re)created on every init: tls_global_exit() memsets the whole
	 * globals struct, lock included. */
	LightLock_Init(&g.lock);
	LightLock_Init(&dns_lock);
	http3ds_locks_init();

	if (g.ready)
		return 0;

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
		return rc == MBEDTLS_ERR_SSL_TIMEOUT ? MBEDTLS_ERR_SSL_WANT_WRITE : rc;
	int n = send(c->fd, buf, len, 0);
	if (n < 0)
		return MBEDTLS_ERR_NET_SEND_FAILED;
	return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	tls_conn *c = ctx;
	int rc = wait_io(c->fd, POLLIN, c->timeout_ms);
	if (rc)
		return rc;
	int n = recv(c->fd, buf, len, 0);
	if (n == 0)
		return MBEDTLS_ERR_SSL_CONN_EOF;
	if (n < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	return n;
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
	/* newlib's resolver keeps static state and is not reentrant: two threads
	 * dialing at once (player stream + a control request) can hand back
	 * corrupted results, so the lookup itself serializes on dns_lock. */
	LightLock_Lock(&dns_lock);
	int gai = getaddrinfo(host, service, &hints, &res);
	LightLock_Unlock(&dns_lock);
	if (gai != 0 || !res) {
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
	if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
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

/* The DRBG is as shared as the ring: every handshake draws random from the
 * same context, and ctr_drbg keeps its reseed state between calls. Same
 * lock as the ticket ring; the RNG callback never runs while the caller
 * holds it (ticket lookups and saves happen outside the handshake). */
static int drbg_random(void *ctx, unsigned char *buf, size_t len)
{
	LightLock_Lock(&g.lock);
	int rc = mbedtls_ctr_drbg_random(ctx, buf, len);
	LightLock_Unlock(&g.lock);
	return rc;
}

tls_conn *tls_connect(const char *host, u16 port, int timeout_ms,
                      tls_stats *stats, char *err, size_t errlen)
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
	mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&c->conf, &g.anchors, NULL);
	mbedtls_ssl_conf_rng(&c->conf, drbg_random, &g.drbg);
	mbedtls_ssl_conf_ciphersuites(&c->conf, SUITES);
	mbedtls_ssl_conf_groups(&c->conf, GROUPS);
	mbedtls_ssl_conf_session_tickets(&c->conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
	/* 16 KB in, 4 KB out, as configured; asking for more would be refused. */
	mbedtls_ssl_conf_read_timeout(&c->conf, timeout_ms);

	if ((rc = mbedtls_ssl_setup(&c->ssl, &c->conf)))
		goto fail;
	/* Without SNI, Spotify's edge serves the wrong certificate; this call also
	 * arms the hostname check against the leaf. */
	if ((rc = mbedtls_ssl_set_hostname(&c->ssl, host)))
		goto fail;
	bool found = false;
	int slot;
	LightLock_Lock(&g.lock);
	slot = ticket_slot(host, &found);
	if (found)
		mbedtls_ssl_set_session(&c->ssl, &g.ticket[slot]);
	LightLock_Unlock(&g.lock);

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
			snprintf(err, errlen,
			         "certificado fuera de fecha: revisa la FECHA y HORA "
			         "de la consola en Configuracion");
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
	 * lands on the ring's next victim. Held again here so the free+copy is
	 * atomic against other threads copying the same slot. */
	LightLock_Lock(&g.lock);
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
	mbedtls_ssl_close_notify(&c->ssl);
	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	close(c->fd);
	free(c);
}
