#include "ap3ds.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "proto3ds.h"
#include "shannon.h"
#include "tls3ds.h"

#include <3ds.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/bignum.h"
#include "mbedtls/md.h"
#include "mbedtls/rsa.h"

/* --------------------------------------------------------------- constants */

#define SPOTIFY_VERSION      124200290u
#define PLATFORM_LINUX_ARM   0x11
#define PRODUCT_CLIENT       0
#define PRODUCT_FLAG_NONE    0
#define CRYPTO_SUITE_SHANNON 0
#define CPU_ARM              5
#define OS_LINUX             5
#define AUTH_SPOTIFY_TOKEN   3

#define MAC_SIZE 4
#define AP_MAX_PACKET (128 * 1024)

/* RFC 2409 group 1: the 768-bit MODP prime, generator 2. */
static const uint8_t DH_PRIME[96] = {
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc9,0x0f,0xda,0xa2,0x21,0x68,0xc2,0x34,
	0xc4,0xc6,0x62,0x8b,0x80,0xdc,0x1c,0xd1,0x29,0x02,0x4e,0x08,0x8a,0x67,0xcc,0x74,
	0x02,0x0b,0xbe,0xa6,0x3b,0x13,0x9b,0x22,0x51,0x4a,0x08,0x79,0x8e,0x34,0x04,0xdd,
	0xef,0x95,0x19,0xb3,0xcd,0x3a,0x43,0x1b,0x30,0x2b,0x0a,0x6d,0xf2,0x5f,0x14,0x37,
	0x4f,0xe1,0x35,0x6d,0x6d,0x51,0xc2,0x45,0xe4,0x85,0xb5,0x76,0x62,0x5e,0x7e,0xc6,
	0xf4,0x4c,0x42,0xe9,0xa6,0x3a,0x36,0x20,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
};

/* Spotify's AP signing key: the 2048-bit RSA modulus, exponent 65537. The
 * server signs its DH public key with it, and checking that signature is the
 * only thing standing between this client and a man in the middle -- the
 * transport itself is plain TCP. */
static const uint8_t SERVER_KEY[256] = {
	0xac,0xe0,0x46,0x0b,0xff,0xc2,0x30,0xaf,0xf4,0x6b,0xfe,0xc3,0xbf,0xbf,0x86,0x3d,
	0xa1,0x91,0xc6,0xcc,0x33,0x6c,0x93,0xa1,0x4f,0xb3,0xb0,0x16,0x12,0xac,0xac,0x6a,
	0xf1,0x80,0xe7,0xf6,0x14,0xd9,0x42,0x9d,0xbe,0x2e,0x34,0x66,0x43,0xe3,0x62,0xd2,
	0x32,0x7a,0x1a,0x0d,0x92,0x3b,0xae,0xdd,0x14,0x02,0xb1,0x81,0x55,0x05,0x61,0x04,
	0xd5,0x2c,0x96,0xa4,0x4c,0x1e,0xcc,0x02,0x4a,0xd4,0xb2,0x0c,0x00,0x1f,0x17,0xed,
	0xc2,0x2f,0xc4,0x35,0x21,0xc8,0xf0,0xcb,0xae,0xd2,0xad,0xd7,0x2b,0x0f,0x9d,0xb3,
	0xc5,0x32,0x1a,0x2a,0xfe,0x59,0xf3,0x5a,0x0d,0xac,0x68,0xf1,0xfa,0x62,0x1e,0xfb,
	0x2c,0x8d,0x0c,0xb7,0x39,0x2d,0x92,0x47,0xe3,0xd7,0x35,0x1a,0x6d,0xbd,0x24,0xc2,
	0xae,0x25,0x5b,0x88,0xff,0xab,0x73,0x29,0x8a,0x0b,0xcc,0xcd,0x0c,0x58,0x67,0x31,
	0x89,0xe8,0xbd,0x34,0x80,0x78,0x4a,0x5f,0xc9,0x6b,0x89,0x9d,0x95,0x6b,0xfc,0x86,
	0xd7,0x4f,0x33,0xa6,0x78,0x17,0x96,0xc9,0xc3,0x2d,0x0d,0x32,0xa5,0xab,0xcd,0x05,
	0x27,0xe2,0xf7,0x10,0xa3,0x96,0x13,0xc4,0x2f,0x99,0xc0,0x27,0xbf,0xed,0x04,0x9c,
	0x3c,0x27,0x58,0x04,0xb6,0xb2,0x19,0xf9,0xc1,0x2f,0x02,0xe9,0x48,0x63,0xec,0xa1,
	0xb6,0x42,0xa0,0x9d,0x48,0x25,0xf8,0xb3,0x9d,0xd0,0xe8,0x6a,0xf9,0x48,0x4d,0xa1,
	0xc2,0xba,0x86,0x30,0x42,0xea,0x9d,0xb3,0x08,0x6c,0x19,0x0e,0x48,0xb3,0x9d,0x66,
	0xeb,0x00,0x06,0xa2,0x5a,0xee,0xa1,0x1b,0x13,0x87,0x3c,0xd7,0x19,0xe6,0x55,0xbd,
};

struct ap_session {
	int      fd;
	shannon  enc, dec;
	uint32_t enc_nonce, dec_nonce;
	uint32_t key_seq;
	int      timeout_ms;
};

const char *ap_login_error_text(int code)
{
	switch (code) {
	case 0x00: return "protocol error";
	case 0x02: return "try another access point";
	case 0x05: return "bad connection id";
	case 0x09: return "travel restriction";
	case 0x0b: return "premium account required";
	case 0x0c: return "bad credentials";
	case 0x0d: return "could not validate credentials";
	case 0x0e: return "account exists";
	case 0x0f: return "extra verification required";
	case 0x10: return "invalid app key";
	case 0x11: return "application banned";
	default:   return "unknown";
	}
}

/* ----------------------------------------------------------------- sockets */

/* As in tls3ds, waits go through poll(): SOC has no usable SO_RCVTIMEO. */
static bool wait_io(int fd, short events, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = events };
	return poll(&p, 1, timeout_ms) == 1;
}

static bool write_all(int fd, const void *buf, size_t len, int timeout_ms)
{
	const uint8_t *p = buf;
	while (len) {
		if (!wait_io(fd, POLLOUT, timeout_ms))
			return false;
		int n = send(fd, p, len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static bool read_exact(int fd, void *buf, size_t len, int timeout_ms)
{
	uint8_t *p = buf;
	while (len) {
		if (!wait_io(fd, POLLIN, timeout_ms))
			return false;
		int n = recv(fd, p, len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static int tcp_connect(const char *host, uint16_t port, char *err, size_t errlen)
{
	char service[8];
	snprintf(service, sizeof(service), "%u", port);
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct addrinfo *res = NULL;
	if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
		snprintf(err, errlen, "DNS failed for %s", host);
		return -1;
	}
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		freeaddrinfo(res);
		snprintf(err, errlen, "socket() failed, errno %d", errno);
		return -1;
	}
	if (tls_connect_timeout(fd, res->ai_addr, res->ai_addrlen, 8000) < 0) {
		snprintf(err, errlen, "connect() to %s:%u failed, errno %d",
		         host, port, errno);
		close(fd);
		freeaddrinfo(res);
		return -1;
	}
	freeaddrinfo(res);
	return fd;
}

/* --------------------------------------------------------------- apresolve */

int ap_resolve_list(ap_candidate *out, int max, char *err, size_t errlen)
{
	http_reply r;
	if (http_get("apresolve.spotify.com", "/?type=accesspoint", NULL, &r) != 0) {
		snprintf(err, errlen, "apresolve: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "apresolve returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	/* The body is {"accesspoint":["host:port", ...]}. json3ds only reads flat
	 * objects, and walking one known array of strings is not worth a parser. */
	int n = 0;
	const char *key = strstr(r.body, "\"accesspoint\"");
	const char *p = key ? strchr(key, '[') : NULL;
	const char *end = p ? strchr(p, ']') : NULL;
	while (p && end && n < max) {
		const char *q1 = strchr(p, '"');
		if (!q1 || q1 > end)
			break;
		const char *q2 = strchr(q1 + 1, '"');
		if (!q2 || q2 > end)
			break;
		char entry[160];
		size_t len = (size_t)(q2 - q1 - 1);
		if (len < sizeof(entry)) {
			memcpy(entry, q1 + 1, len);
			entry[len] = '\0';
			char *colon = strrchr(entry, ':');
			if (colon) {
				*colon = '\0';
				snprintf(out[n].host, sizeof(out[n].host), "%s", entry);
				out[n].port = (uint16_t)atoi(colon + 1);
				n++;
			}
		}
		p = q2 + 1;
	}
	http_reply_free(&r);

	if (n == 0) {
		snprintf(err, errlen, "could not read an access point out of apresolve");
		return -1;
	}
	return n;
}

int ap_resolve(char *host, size_t hostlen, uint16_t *port, char *err, size_t errlen)
{
	ap_candidate one[1];
	if (ap_resolve_list(one, 1, err, errlen) < 1)
		return -1;
	snprintf(host, hostlen, "%s", one[0].host);
	*port = one[0].port;
	return 0;
}

/* -------------------------------------------------------------- device id */

void ap_device_id(char *out, size_t outlen)
{
	/* Kept in its own file rather than in spotify.cfg: that file holds the
	 * refresh token and is rewritten atomically on every renewal, and the
	 * device id has no business sharing that path. */
	char path[320];
	snprintf(path, sizeof(path), "%s", cfg_path());
	char *slash = strrchr(path, '/');
	if (slash)
		slash[1] = '\0';
	else
		path[0] = '\0';
	strncat(path, "device.id", sizeof(path) - strlen(path) - 1);

	FILE *f = fopen(path, "r");
	if (f) {
		char buf[64] = {0};
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			buf[--n] = '\0';
		if (n == 40) {
			snprintf(out, outlen, "%s", buf);
			return;
		}
	}

	/* A stable id matters: a new one every boot would look like a new device
	 * on the account each time. */
	uint8_t raw[20];
	if (R_FAILED(PS_GenerateRandomBytes(raw, sizeof(raw))))
		memset(raw, 0x3d, sizeof(raw));
	char hexs[41];
	for (int i = 0; i < 20; i++)
		snprintf(hexs + i * 2, 3, "%02x", raw[i]);
	snprintf(out, outlen, "%s", hexs);

	f = fopen(path, "w");
	if (f) {
		fputs(hexs, f);
		fclose(f);
	}
}

/* --------------------------------------------------------------- handshake */

static bool hmac_sha1(const uint8_t *key, size_t keylen,
                      const uint8_t *msg, size_t msglen, uint8_t out[20])
{
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	return md && mbedtls_md_hmac(md, key, keylen, msg, msglen, out) == 0;
}

/* Verifies that gs really came from Spotify. */
static bool verify_gs(const uint8_t *gs, size_t gslen,
                      const uint8_t *sig, size_t siglen)
{
	if (siglen != 256)
		return false;

	uint8_t digest[20];
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	if (!md || mbedtls_md(md, gs, gslen, digest) != 0)
		return false;

	mbedtls_rsa_context rsa;
	mbedtls_rsa_init(&rsa);
	bool ok = false;
	mbedtls_mpi n, e;
	mbedtls_mpi_init(&n);
	mbedtls_mpi_init(&e);
	if (mbedtls_mpi_read_binary(&n, SERVER_KEY, sizeof(SERVER_KEY)) == 0 &&
	    mbedtls_mpi_lset(&e, 65537) == 0 &&
	    mbedtls_rsa_import(&rsa, &n, NULL, NULL, NULL, &e) == 0 &&
	    mbedtls_rsa_complete(&rsa) == 0) {
		ok = mbedtls_rsa_pkcs1_verify(&rsa, MBEDTLS_MD_SHA1, 20,
		                              digest, sig) == 0;
	}
	mbedtls_mpi_free(&n);
	mbedtls_mpi_free(&e);
	mbedtls_rsa_free(&rsa);
	return ok;
}

/* The accumulator is every byte sent and received during the handshake, in
 * order, including the framing. Both sides derive the keys from it, so a
 * single byte out of place here fails with nothing more specific than the
 * server hanging up. */
static bool handshake(int fd, int timeout_ms, shannon *enc, shannon *dec,
                      double *ms, char *err, size_t errlen)
{
	TickCounter t;
	osTickCounterStart(&t);

	uint8_t priv_raw[95];
	if (R_FAILED(PS_GenerateRandomBytes(priv_raw, sizeof(priv_raw)))) {
		snprintf(err, errlen, "no entropy for the DH private key");
		return false;
	}
	uint8_t nonce[0x10];
	if (R_FAILED(PS_GenerateRandomBytes(nonce, sizeof(nonce)))) {
		snprintf(err, errlen, "no entropy for the client nonce");
		return false;
	}

	bool ok = false;
	uint8_t *acc = malloc(AP_MAX_PACKET);
	mbedtls_mpi p, g, priv, gc_mpi, gs_mpi, shared_mpi;
	mbedtls_mpi_init(&p);
	mbedtls_mpi_init(&g);
	mbedtls_mpi_init(&priv);
	mbedtls_mpi_init(&gc_mpi);
	mbedtls_mpi_init(&gs_mpi);
	mbedtls_mpi_init(&shared_mpi);
	if (!acc) {
		snprintf(err, errlen, "out of memory");
		goto done;
	}

	/* librespot reads its 95 random bytes little-endian; the value just has to
	 * match on both of our own ends, but staying identical keeps this
	 * comparable against the reference run. */
	if (mbedtls_mpi_read_binary_le(&priv, priv_raw, sizeof(priv_raw)) != 0 ||
	    mbedtls_mpi_read_binary(&p, DH_PRIME, sizeof(DH_PRIME)) != 0 ||
	    mbedtls_mpi_lset(&g, 2) != 0 ||
	    mbedtls_mpi_exp_mod(&gc_mpi, &g, &priv, &p, NULL) != 0) {
		snprintf(err, errlen, "DH key generation failed");
		goto done;
	}
	uint8_t gc[96];
	if (mbedtls_mpi_write_binary(&gc_mpi, gc, sizeof(gc)) != 0) {
		snprintf(err, errlen, "DH public key did not fit");
		goto done;
	}

	/* ClientHello. Field numbers are literal in Spotify's .proto files. */
	uint8_t bi_store[64], dh_store[128], lch_store[160], hello_store[512];
	pb bi, dh_hello, lch, hello;
	pb_init(&bi, bi_store, sizeof(bi_store));
	pb_varint(&bi, 0x0a, PRODUCT_CLIENT);
	pb_varint(&bi, 0x14, PRODUCT_FLAG_NONE);
	pb_varint(&bi, 0x1e, PLATFORM_LINUX_ARM);
	pb_varint(&bi, 0x28, SPOTIFY_VERSION);

	pb_init(&dh_hello, dh_store, sizeof(dh_store));
	pb_bytes(&dh_hello, 0x0a, gc, sizeof(gc));
	pb_varint(&dh_hello, 0x14, 1);              /* server_keys_known */

	pb_init(&lch, lch_store, sizeof(lch_store));
	pb_sub(&lch, 0x0a, &dh_hello);

	pb_init(&hello, hello_store, sizeof(hello_store));
	pb_sub(&hello, 0x0a, &bi);
	pb_varint(&hello, 0x1e, CRYPTO_SUITE_SHANNON);
	pb_sub(&hello, 0x32, &lch);
	pb_bytes(&hello, 0x3c, nonce, sizeof(nonce));
	pb_bytes(&hello, 0x46, "\x1e", 1);
	if (!pb_ok(&hello)) {
		snprintf(err, errlen, "ClientHello did not fit its buffer");
		goto done;
	}

	size_t acc_len = 0;
	uint32_t framed = (uint32_t)(2 + 4 + hello.len);
	acc[acc_len++] = 0x00;
	acc[acc_len++] = 0x04;
	acc[acc_len++] = (uint8_t)(framed >> 24);
	acc[acc_len++] = (uint8_t)(framed >> 16);
	acc[acc_len++] = (uint8_t)(framed >> 8);
	acc[acc_len++] = (uint8_t)framed;
	memcpy(acc + acc_len, hello.p, hello.len);
	acc_len += hello.len;

	if (!write_all(fd, acc, acc_len, timeout_ms)) {
		snprintf(err, errlen, "could not send ClientHello");
		goto done;
	}

	/* APResponseMessage, length-prefixed and counted into the accumulator. */
	if (!read_exact(fd, acc + acc_len, 4, timeout_ms)) {
		snprintf(err, errlen, "no APResponseMessage header");
		goto done;
	}
	uint32_t size = ((uint32_t)acc[acc_len] << 24) | ((uint32_t)acc[acc_len + 1] << 16) |
	                ((uint32_t)acc[acc_len + 2] << 8) | acc[acc_len + 3];
	acc_len += 4;
	if (size < 4 || size - 4 > AP_MAX_PACKET - acc_len) {
		snprintf(err, errlen, "APResponseMessage claims %lu bytes",
		         (unsigned long)size);
		goto done;
	}
	uint8_t *body = acc + acc_len;
	size_t body_len = size - 4;
	if (!read_exact(fd, body, body_len, timeout_ms)) {
		snprintf(err, errlen, "APResponseMessage body truncated");
		goto done;
	}
	acc_len += body_len;

	/* login_failed instead of a challenge: the AP turned us away. */
	pbv failed;
	if (pb_get_bytes(body, body_len, 0x1e, &failed)) {
		uint64_t code = 0;
		pb_get_varint(failed.p, failed.len, 0x0a, &code);
		snprintf(err, errlen, "AP refused the hello: %s (%lu)",
		         ap_login_error_text((int)code), (unsigned long)code);
		goto done;
	}

	static const uint32_t PATH_DH[] = { 0x0a, 0x0a, 0x0a };
	pbv dh;
	if (!pb_dig(body, body_len, PATH_DH, 3, &dh)) {
		snprintf(err, errlen, "no diffie_hellman challenge in APResponseMessage");
		goto done;
	}
	pbv gs, gs_sig;
	if (!pb_get_bytes(dh.p, dh.len, 0x0a, &gs) ||
	    !pb_get_bytes(dh.p, dh.len, 0x1e, &gs_sig)) {
		snprintf(err, errlen, "challenge is missing gs or gs_signature");
		goto done;
	}
	if (!verify_gs(gs.p, gs.len, gs_sig.p, gs_sig.len)) {
		snprintf(err, errlen, "server key verification FAILED -- refusing");
		goto done;
	}

	if (mbedtls_mpi_read_binary(&gs_mpi, gs.p, gs.len) != 0 ||
	    mbedtls_mpi_exp_mod(&shared_mpi, &gs_mpi, &priv, &p, NULL) != 0) {
		snprintf(err, errlen, "shared secret computation failed");
		goto done;
	}
	uint8_t shared[96];
	if (mbedtls_mpi_write_binary(&shared_mpi, shared, sizeof(shared)) != 0) {
		snprintf(err, errlen, "shared secret did not fit");
		goto done;
	}

	/* Five HMAC-SHA1 blocks over the accumulator give 100 bytes: the first 20
	 * key the challenge HMAC, then 32 each for send and receive. */
	uint8_t data[100];
	for (uint8_t i = 1; i <= 5; i++) {
		uint8_t msg_tail = i;
		uint8_t block[20];
		/* HMAC over accumulator || i, without copying the accumulator. */
		const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
		mbedtls_md_context_t ctx;
		mbedtls_md_init(&ctx);
		bool step = md && mbedtls_md_setup(&ctx, md, 1) == 0 &&
		            mbedtls_md_hmac_starts(&ctx, shared, sizeof(shared)) == 0 &&
		            mbedtls_md_hmac_update(&ctx, acc, acc_len) == 0 &&
		            mbedtls_md_hmac_update(&ctx, &msg_tail, 1) == 0 &&
		            mbedtls_md_hmac_finish(&ctx, block) == 0;
		mbedtls_md_free(&ctx);
		if (!step) {
			snprintf(err, errlen, "key derivation failed at block %u", i);
			goto done;
		}
		memcpy(data + (i - 1) * 20, block, 20);
	}

	uint8_t challenge[20];
	if (!hmac_sha1(data, 0x14, acc, acc_len, challenge)) {
		snprintf(err, errlen, "challenge HMAC failed");
		goto done;
	}

	/* ClientResponsePlaintext. pow_response and crypto_response are required
	 * by the schema and sent empty, exactly as librespot does. */
	uint8_t dhr_store[64], lcr_store[96], resp_store[160];
	pb dhr, lcr, resp;
	pb_init(&dhr, dhr_store, sizeof(dhr_store));
	pb_bytes(&dhr, 0x0a, challenge, sizeof(challenge));
	pb_init(&lcr, lcr_store, sizeof(lcr_store));
	pb_sub(&lcr, 0x0a, &dhr);
	pb_init(&resp, resp_store, sizeof(resp_store));
	pb_sub(&resp, 0x0a, &lcr);
	pb_bytes(&resp, 0x14, "", 0);
	pb_bytes(&resp, 0x1e, "", 0);
	if (!pb_ok(&resp)) {
		snprintf(err, errlen, "ClientResponsePlaintext did not fit");
		goto done;
	}

	uint8_t frame[4];
	uint32_t rsize = (uint32_t)(4 + resp.len);
	frame[0] = (uint8_t)(rsize >> 24);
	frame[1] = (uint8_t)(rsize >> 16);
	frame[2] = (uint8_t)(rsize >> 8);
	frame[3] = (uint8_t)rsize;
	if (!write_all(fd, frame, 4, timeout_ms) ||
	    !write_all(fd, resp.p, resp.len, timeout_ms)) {
		snprintf(err, errlen, "could not send ClientResponsePlaintext");
		goto done;
	}

	shannon_init(enc, data + 0x14, 0x20);
	shannon_init(dec, data + 0x34, 0x20);
	ok = true;

done:
	osTickCounterUpdate(&t);
	if (ms)
		*ms = osTickCounterRead(&t);
	free(acc);
	mbedtls_mpi_free(&p);
	mbedtls_mpi_free(&g);
	mbedtls_mpi_free(&priv);
	mbedtls_mpi_free(&gc_mpi);
	mbedtls_mpi_free(&gs_mpi);
	mbedtls_mpi_free(&shared_mpi);
	return ok;
}

/* ------------------------------------------------------------ packet layer */

int ap_send(ap_session *s, uint8_t cmd, const void *payload, size_t len)
{
	/* A dropped session is reported as NULL by its owner; a packet call on
	 * it must fail, not fault. */
	if (!s)
		return -1;
	if (len > 0xFFFF)
		return -1;
	size_t total = 3 + len;
	uint8_t *buf = malloc(total + MAC_SIZE);
	if (!buf)
		return -1;
	buf[0] = cmd;
	buf[1] = (uint8_t)(len >> 8);
	buf[2] = (uint8_t)len;
	if (len)
		memcpy(buf + 3, payload, len);

	shannon_nonce_u32(&s->enc, s->enc_nonce++);
	shannon_encrypt(&s->enc, buf, total);
	shannon_finish(&s->enc, buf + total, MAC_SIZE);

	bool ok = write_all(s->fd, buf, total + MAC_SIZE, s->timeout_ms);
	free(buf);
	return ok ? (int)len : -1;
}

int ap_wait(ap_session *s, int timeout_ms)
{
	if (!s)
		return -1;
	struct pollfd p = { .fd = s->fd, .events = POLLIN };
	int n = poll(&p, 1, timeout_ms);
	if (n == 0)
		return 0;
	if (n < 0)
		return -1;
	/* A peer that hung up is readable too, but reports it as an error or
	 * hangup rather than data; reporting that as "ready" would turn into a
	 * confusing read failure one call later. */
	if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
		return -1;
	return 1;
}

int ap_recv(ap_session *s, uint8_t *cmd, uint8_t *buf, size_t cap)
{
	if (!s)
		return -1;
	uint8_t header[3];
	if (!read_exact(s->fd, header, sizeof(header), s->timeout_ms))
		return -1;

	shannon_nonce_u32(&s->dec, s->dec_nonce++);
	shannon_decrypt(&s->dec, header, sizeof(header));
	*cmd = header[0];
	size_t size = ((size_t)header[1] << 8) | header[2];

	/* Too big for the caller's buffer: read and decrypt it anyway, into a
	 * scratch area, and report it as skipped. Returning early here would
	 * leave the payload in the socket and the Shannon state half-advanced,
	 * which breaks every packet that follows -- and the access point's
	 * SecretBlock is over 4 KB, so this is not hypothetical. */
	bool fits = size <= cap;
	uint8_t *dst = buf;
	uint8_t *scratch = NULL;
	if (!fits) {
		scratch = malloc(size ? size : 1);
		if (!scratch)
			return -1;
		dst = scratch;
	}

	if (size && !read_exact(s->fd, dst, size, s->timeout_ms)) {
		free(scratch);
		return -1;
	}
	if (size)
		shannon_decrypt(&s->dec, dst, size);

	uint8_t mac[MAC_SIZE], expect[MAC_SIZE];
	bool ok = read_exact(s->fd, mac, sizeof(mac), s->timeout_ms);
	if (ok) {
		shannon_finish(&s->dec, expect, sizeof(expect));
		/* A bad MAC means the stream is no longer trustworthy, not that one
		 * packet was damaged: there is no recovering the cipher state. */
		ok = memcmp(mac, expect, sizeof(mac)) == 0;
	}
	free(scratch);
	if (!ok)
		return -1;
	if (!fits)
		return AP_PKT_TOO_BIG;

	return (int)size;
}

int ap_audio_key(ap_session *s, const uint8_t file_id[20],
                 const uint8_t track_gid[16], uint8_t key_out[16],
                 int *refused_code, char *err, size_t errlen)
{
	if (refused_code)
		*refused_code = 0;

	/* file_id, then the track's raw id, then our sequence, then two zero
	 * bytes. 42 bytes, exactly as librespot sends it. */
	uint8_t req[42];
	memcpy(req, file_id, 20);
	memcpy(req + 20, track_gid, 16);
	uint32_t seq = s->key_seq++;
	req[36] = (uint8_t)(seq >> 24);
	req[37] = (uint8_t)(seq >> 16);
	req[38] = (uint8_t)(seq >> 8);
	req[39] = (uint8_t)seq;
	req[40] = 0;
	req[41] = 0;

	if (ap_send(s, AP_PKT_REQUESTKEY, req, sizeof(req)) < 0) {
		snprintf(err, errlen, "could not send the key request");
		return -1;
	}

	/* The answer arrives among whatever else the AP is saying, and the
	 * sequence number is what identifies it. */
	uint8_t buf[4096];
	for (int i = 0; i < 24; i++) {
		uint8_t cmd;
		int n = ap_recv(s, &cmd, buf, sizeof(buf));
		if (n == AP_PKT_TOO_BIG)
			continue;         /* the SecretBlock and friends; not ours */
		if (n < 0) {
			snprintf(err, errlen, "connection lost waiting for the audio key");
			return -1;
		}
		if ((cmd == AP_PKT_AESKEY || cmd == AP_PKT_AESKEYERROR) && n >= 4) {
			uint32_t got = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
			               ((uint32_t)buf[2] << 8) | buf[3];
			if (got != seq)
				continue;          /* an older request's answer */
			if (cmd == AP_PKT_AESKEY) {
				if (n < 20) {
					snprintf(err, errlen, "key payload is %d bytes", n);
					return -1;
				}
				memcpy(key_out, buf + 4, 16);
				return 0;
			}
			int code = (n >= 6) ? ((buf[4] << 8) | buf[5]) : -1;
			if (refused_code)
				*refused_code = code;
			snprintf(err, errlen,
			         "Spotify no permite esta cancion (0x%04X): se "
			         "requiere Premium o no esta disponible", code);
			return AP_KEY_REFUSED;
		}
		if (cmd == AP_PKT_PING)
			ap_handle_ping(s, buf, (size_t)n);
	}
	snprintf(err, errlen, "no answer to the key request");
	return -1;
}

int ap_handle_ping(ap_session *s, const uint8_t *payload, size_t len)
{
	(void)payload;
	(void)len;
	/* The AP only wants to see a Pong; its four bytes are not checked. */
	const uint8_t zero[4] = {0};
	return ap_send(s, AP_PKT_PONG, zero, sizeof(zero));
}

/* ----------------------------------------------------------------- login */

ap_session *ap_login(const char *host, uint16_t port, const char *access_token,
                     const char *device_id, ap_info *info,
                     char *err, size_t errlen)
{
	ap_info local = {0};
	ap_info *nfo = info ? info : &local;
	memset(nfo, 0, sizeof(*nfo));
	snprintf(nfo->ap, sizeof(nfo->ap), "%s:%u", host, port);

	TickCounter t;
	osTickCounterStart(&t);
	int fd = tcp_connect(host, port, err, errlen);
	osTickCounterUpdate(&t);
	nfo->connect_ms = osTickCounterRead(&t);
	if (fd < 0)
		return NULL;

	ap_session *s = calloc(1, sizeof(*s));
	if (!s) {
		snprintf(err, errlen, "out of memory");
		close(fd);
		return NULL;
	}
	s->fd = fd;
	s->timeout_ms = 15000;

	if (!handshake(fd, s->timeout_ms, &s->enc, &s->dec, &nfo->handshake_ms,
	               err, errlen)) {
		close(fd);
		free(s);
		return NULL;
	}

	/* ClientResponseEncrypted: the OAuth access token as the credential. */
	osTickCounterStart(&t);
	size_t token_len = strlen(access_token);
	size_t creds_cap = token_len + 64;
	uint8_t *creds_store = malloc(creds_cap);
	uint8_t sys_store[256];
	uint8_t *login_store = malloc(creds_cap + sizeof(sys_store) + 128);
	if (!creds_store || !login_store) {
		snprintf(err, errlen, "out of memory for the login packet");
		free(creds_store);
		free(login_store);
		ap_close(s);
		return NULL;
	}

	pb creds, sys, login;
	pb_init(&creds, creds_store, creds_cap);
	pb_varint(&creds, 0x14, AUTH_SPOTIFY_TOKEN);
	pb_bytes(&creds, 0x1e, access_token, token_len);

	pb_init(&sys, sys_store, sizeof(sys_store));
	pb_varint(&sys, 0x0a, CPU_ARM);
	pb_varint(&sys, 0x3c, OS_LINUX);
	pb_str(&sys, 0x5a, "3ds-spotify");
	pb_str(&sys, 0x64, device_id);

	pb_init(&login, login_store, creds_cap + sizeof(sys_store) + 128);
	pb_sub(&login, 0x0a, &creds);
	pb_sub(&login, 0x32, &sys);
	pb_str(&login, 0x46, "3ds-spotify 0.1");

	bool built = pb_ok(&login);
	int sent = built ? ap_send(s, AP_PKT_LOGIN, login.p, login.len) : -1;
	free(creds_store);
	free(login_store);
	if (sent < 0) {
		snprintf(err, errlen, built ? "could not send the login packet"
		                            : "login packet did not fit");
		ap_close(s);
		return NULL;
	}

	/* The AP may slip other packets in before the answer. */
	uint8_t *buf = malloc(AP_MAX_PACKET);
	if (!buf) {
		snprintf(err, errlen, "out of memory");
		ap_close(s);
		return NULL;
	}
	bool ok = false;
	for (int i = 0; i < 8; i++) {
		uint8_t cmd;
		int n = ap_recv(s, &cmd, buf, AP_MAX_PACKET);
		if (n == AP_PKT_TOO_BIG)
			continue;         /* nothing we are waiting for is that large */
		if (n < 0) {
			snprintf(err, errlen, "connection lost waiting for APWelcome");
			break;
		}
		if (cmd == AP_PKT_APWELCOME) {
			pbv user;
			if (pb_get_bytes(buf, (size_t)n, 0x0a, &user)) {
				size_t len = user.len < sizeof(nfo->username) - 1
				             ? user.len : sizeof(nfo->username) - 1;
				memcpy(nfo->username, user.p, len);
				nfo->username[len] = '\0';
			}
			uint64_t acct = 0;
			pb_get_varint(buf, (size_t)n, 0x14, &acct);
			nfo->account_type = (int)acct;
			pbv reusable;
			if (pb_get_bytes(buf, (size_t)n, 0x28, &reusable) &&
			    reusable.len <= sizeof(nfo->reusable)) {
				memcpy(nfo->reusable, reusable.p, reusable.len);
				nfo->reusable_len = reusable.len;
			}
			ok = true;
			break;
		}
		if (cmd == AP_PKT_AUTHFAILURE) {
			uint64_t code = 0;
			pb_get_varint(buf, (size_t)n, 0x0a, &code);
			nfo->login_error = (int)code;
			snprintf(err, errlen, "login refused: %s (%lu)",
			         ap_login_error_text((int)code), (unsigned long)code);
			break;
		}
		if (cmd == AP_PKT_PING)
			ap_handle_ping(s, buf, (size_t)n);
	}
	free(buf);
	osTickCounterUpdate(&t);
	nfo->login_ms = osTickCounterRead(&t);

	if (!ok) {
		ap_close(s);
		return NULL;
	}
	return s;
}

ap_session *ap_login_any(const ap_candidate *cands, int n, const char *access_token,
                         const char *device_id, ap_info *info, int *tried,
                         char *err, size_t errlen)
{
	char last[256] = "no candidates";
	for (int i = 0; i < n; i++) {
		if (tried)
			*tried = i + 1;
		ap_session *s = ap_login(cands[i].host, cands[i].port, access_token,
		                         device_id, info, err, errlen);
		if (s)
			return s;
		snprintf(last, sizeof(last), "%s", err);
		/* A refused login is the account's answer and will be the same
		 * everywhere, so only connection-level failures are worth retrying
		 * against another access point. */
		if (info && info->login_error)
			break;
	}
	snprintf(err, errlen, "%s", last);
	return NULL;
}

void ap_close(ap_session *s)
{
	if (!s)
		return;
	close(s->fd);
	free(s);
}
