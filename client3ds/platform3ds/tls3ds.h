/* TLS 1.2 client for the 3DS: libctru sockets under mbedTLS.
 *
 * The console's own sslc is deliberately unused. We need to pick the cipher
 * list, carry our own trust anchors, and reuse the same crypto for the access
 * point handshake and the AES-CTR audio files later on.
 */
#ifndef TLS3DS_H
#define TLS3DS_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct tls_conn tls_conn;

/* The error text a handshake reports when every certificate is outside its
 * validity window -- almost always a console clock that was never set. The
 * UI matches the "clock:" prefix to show a translated fix instead. */
#define TLS_ERR_CLOCK_PREFIX "clock:"
#define TLS_ERR_CLOCK TLS_ERR_CLOCK_PREFIX \
	" certificate dates invalid, set Date & Time in System Settings"

/* Everything the probe wants to report, filled in even when the connection
 * fails -- the stage that is still zero is the one that broke. */
typedef struct {
	double   dns_ms;
	double   connect_ms;
	double   handshake_ms;
	u32      verify_flags;       /* 0 = chain accepted */
	bool     resumed;            /* a session ticket skipped the key exchange */
	char     version[16];        /* "TLSv1.2" */
	char     ciphersuite[48];
	char     peer_cn[96];
	int      handshake_bytes_in;
	int      handshake_bytes_out;
} tls_stats;

/* Parses the linked CA bundle and seeds the DRBG. socInit() and psInit() must
 * already have succeeded. Returns 0, or a negative mbedTLS error. */
int  tls_global_init(void);
/* Receives one summary line per closed connection that moved data. */
void tls_set_logger(void (*logger)(const char *line));
void tls_global_exit(void);

/* How many anchors tls_global_init() accepted; 0 means every chain will fail. */
int  tls_anchor_count(void);

/* Connects, verifies the chain against the bundle and against `host`, and
 * returns a ready connection, or NULL with `err` set. `stats` may be NULL.
 *
 * A session ticket from an earlier connection to the same host is reused when
 * one is held, which is what makes the second connection cheap. */
tls_conn *tls_connect(const char *host, u16 port, int timeout_ms,
                      tls_stats *stats, char *err, size_t errlen);

/* For a device on the home network, by IP: Cast speakers present a
 * certificate signed by their own device key, which no public anchor
 * vouches for. No chain check, no SNI, no session ticket. Never for
 * anything on the internet. */
tls_conn *tls_connect_lan(const char *ip, u16 port, int timeout_ms,
                          char *err, size_t errlen);

int  tls_write(tls_conn *c, const void *buf, size_t len);   /* writes all of it */
int  tls_read(tls_conn *c, void *buf, size_t len);          /* 0 = closed */
void tls_close(tls_conn *c);

/* Renders an mbedTLS error as text; in the shipping build, as a bare number. */
void tls_strerror(int code, char *out, size_t outlen);

/* connect() for every dial (TLS hosts and the access point). Blocking for
 * now: SOC's connect has no deadline of its own, and the non-blocking
 * attempt at one broke sign-in on hardware. timeout_ms is unused. */
struct sockaddr;
int  tls_connect_timeout(int fd, const struct sockaddr *addr,
                         unsigned addrlen, int timeout_ms);

#endif
