#include "cast3ds.h"

#include "http3ds.h"
#include "json3ds.h"
#include "proto3ds.h"
#include "tls3ds.h"

#include "mbedtls/md5.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CAST_APP     "CC32E753"   /* Spotify's Cast receiver */
#define NS_CONN      "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER  "urn:x-cast:com.google.cast.receiver"
#define NS_SPOTIFY   "urn:x-cast:com.spotify.chromecast.secure.v1"
#define SENDER       "sender-0"
#define RECEIVER     "receiver-0"
#define CAST_FRAME_MAX (64 * 1024)

/* ---- Finding speakers ----------------------------------------------------
 *
 * One mDNS question for _googlecast._tcp.local, sent from an ordinary port:
 * that makes it a "legacy unicast" query (RFC 6762 6.7), which responders
 * answer straight back to that port, so no multicast membership is needed.
 * Each answer carries the friendly name in TXT fn= and the model in md=;
 * the address is simply where the answer came from.
 *
 * The question goes out three ways, since the console found nothing with
 * multicast alone: to the mDNS group, to the subnet's broadcast address
 * (on a test network every speaker answered that), and to each host of
 * the /24 in turn (some answer only that). */

/* Skips a DNS name at `at` (labels or a compression pointer); the offset
 * after it, or -1 when it runs off the packet. */
static int dns_skip_name(const uint8_t *p, int len, int at)
{
	while (at < len) {
		const uint8_t c = p[at];
		if (c == 0)
			return at + 1;
		if ((c & 0xC0) == 0xC0)
			return at + 2 <= len ? at + 2 : -1;
		at += 1 + c;
	}
	return -1;
}

/* Copies TXT key `key` ("fn=") out of a TXT rdata into out. */
static bool dns_txt_get(const uint8_t *rd, int n, const char *key,
                        char *out, size_t outlen)
{
	const size_t kl = strlen(key);
	for (int i = 0; i < n;) {
		const int sl = rd[i++];
		if (i + sl > n)
			break;
		if ((size_t)sl >= kl && memcmp(rd + i, key, kl) == 0) {
			snprintf(out, outlen, "%.*s", sl - (int)kl,
			         (const char *)rd + i + kl);
			return true;
		}
		i += sl;
	}
	return false;
}

/* Reads one answer: name, model and port of the speaker it describes. */
static void dns_parse(const uint8_t *p, int len, char *fn, size_t fnlen,
                      char *md, size_t mdlen, int *port)
{
	if (len < 12)
		return;
	const int qd = p[4] << 8 | p[5];
	const int rr = (p[6] << 8 | p[7]) + (p[8] << 8 | p[9]) +
	               (p[10] << 8 | p[11]);
	int at = 12;
	for (int i = 0; i < qd && at >= 0; i++) {
		at = dns_skip_name(p, len, at);
		if (at >= 0)
			at += 4;
	}
	for (int i = 0; i < rr && at >= 0 && at < len; i++) {
		at = dns_skip_name(p, len, at);
		if (at < 0 || at + 10 > len)
			return;
		const int type = p[at] << 8 | p[at + 1];
		const int rdlen = p[at + 8] << 8 | p[at + 9];
		at += 10;
		if (at + rdlen > len)
			return;
		if (type == 16) {          /* TXT */
			dns_txt_get(p + at, rdlen, "fn=", fn, fnlen);
			dns_txt_get(p + at, rdlen, "md=", md, mdlen);
		} else if (type == 33 && rdlen >= 6) {   /* SRV */
			*port = p[at + 4] << 8 | p[at + 5];
		}
		at += rdlen;
	}
}

int cast_discover(cast_found *out, int max, int wait_ms, char *err,
                  size_t errlen)
{
	const int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		snprintf(err, errlen, "no socket");
		return -1;
	}
	struct sockaddr_in me;
	memset(&me, 0, sizeof(me));
	me.sin_family = AF_INET;
	/* A port of our own choosing, not 0 (whether the console's sockets
	 * pick one for 0 is not something to bet the replies on), and not
	 * 5353: that is what makes the speakers answer us directly. */
	me.sin_addr.s_addr = INADDR_ANY;
	int bound = -1, bind_errno = 0;
	for (int k = 0; k < 4 && bound != 0; k++) {
		me.sin_port = htons(49152 + (uint16_t)((osGetTime() + k * 977) % 16000));
		bound = bind(s, (struct sockaddr *)&me, sizeof(me));
		if (bound != 0)
			bind_errno = errno;
	}
	/* Unbound, a send still picks a port on most stacks: try that rather
	 * than give up, and say so in the log. */
	if (bound != 0)
		me.sin_port = 0;

	static const uint8_t query[] = {
		0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,   /* one question */
		11, '_', 'g', 'o', 'o', 'g', 'l', 'e', 'c', 'a', 's', 't',
		4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
		0, 12, 0, 1,                                /* PTR, IN */
	};
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(5353);

	/* Our address and subnet, for the broadcast and the sweep. */
	struct in_addr ip = { 0 }, mask = { 0 }, bcast = { 0 };
	if (SOCU_GetIPInfo(&ip, &mask, &bcast) != 0 || !ip.s_addr)
		ip.s_addr = (in_addr_t)gethostid();
	if (!bcast.s_addr)
		bcast.s_addr = ip.s_addr | htonl(0xFF);
	int sent_mc = 0, sent_bc = 0, sent_uc = 0, replies = 0;

	int n = 0;
	static uint8_t buf[2048];
	const u64 end = osGetTime() + (u64)wait_ms;
	u64 next_ask = 0;
	for (int asked = 0;;) {
		u64 now = osGetTime();
		if (now >= end)
			break;
		/* Asked twice, a beat apart: a dropped packet is likely on wifi. */
		if (asked < 2 && now >= next_ask) {
			to.sin_addr.s_addr = inet_addr("224.0.0.251");
			sent_mc += sendto(s, query, sizeof(query), 0,
			                  (struct sockaddr *)&to, sizeof(to)) > 0;
			to.sin_addr.s_addr = bcast.s_addr;
			sent_bc += sendto(s, query, sizeof(query), 0,
			                  (struct sockaddr *)&to, sizeof(to)) > 0;
			to.sin_addr.s_addr = INADDR_BROADCAST;
			sent_bc += sendto(s, query, sizeof(query), 0,
			                  (struct sockaddr *)&to, sizeof(to)) > 0;
			if (asked == 0 && ip.s_addr) {
				const uint32_t net = ntohl(ip.s_addr) & 0xFFFFFF00u;
				for (uint32_t h = 1; h < 255; h++) {
					if ((net | h) == ntohl(ip.s_addr))
						continue;
					to.sin_addr.s_addr = htonl(net | h);
					sent_uc += sendto(s, query, sizeof(query), 0,
					                  (struct sockaddr *)&to,
					                  sizeof(to)) > 0;
				}
			}
			asked++;
			next_ask = now + 700;
		}
		struct pollfd pf = { .fd = s, .events = POLLIN };
		int left = (int)(end - now);
		if (poll(&pf, 1, left < 200 ? left : 200) <= 0)
			continue;
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		const int got = recvfrom(s, buf, sizeof(buf), 0,
		                         (struct sockaddr *)&from, &fl);
		if (got <= 0)
			continue;
		replies++;
		char fn[64] = "", md[64] = "";
		int port = 8009;
		dns_parse(buf, got, fn, sizeof(fn), md, sizeof(md), &port);
		/* A group is played through its leader on another port, with a
		 * different sign-in: left out until that is supported. */
		if (!fn[0] || port != 8009 || strcmp(md, "Google Cast Group") == 0)
			continue;
		bool dup = false;
		for (int i = 0; i < n; i++)
			if (strcmp(out[i].name, fn) == 0)
				dup = true;
		if (dup || n >= max)
			continue;
		snprintf(out[n].name, sizeof(out[n].name), "%s", fn);
		snprintf(out[n].model, sizeof(out[n].model), "%s", md);
		inet_ntop(AF_INET, &from.sin_addr, out[n].ip, sizeof(out[n].ip));
		n++;
	}
	close(s);
	/* What went out, for the log: tells a blocked send from silence. */
	char me_s[16] = "?";
	inet_ntop(AF_INET, &ip, me_s, sizeof(me_s));
	snprintf(err, errlen, "from %s:%u%s: multicast %d, broadcast %d, hosts "
	         "%d sent, %d replies", me_s, (unsigned)ntohs(me.sin_port),
	         bound != 0 ? " (not bound)" : "",
	         sent_mc, sent_bc, sent_uc, replies);
	if (bound != 0)
		snprintf(err + strlen(err), errlen - strlen(err), ", bind errno %d",
		         bind_errno);
	return n;
}

void cast_device_id(const char *name, char out[CAST_DEV_LEN])
{
	unsigned char d[16];
	mbedtls_md5((const unsigned char *)name, strlen(name), d);
	for (int i = 0; i < 16; i++)
		snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* CastMessage: 1 protocol_version (0), 2 source_id, 3 destination_id,
 * 4 namespace, 5 payload_type (0 = string), 6 payload_utf8; each frame
 * goes out behind a 4-byte big-endian length. */
static int cast_send(tls_conn *c, const char *dst, const char *ns,
                     const char *json)
{
	const size_t cap = strlen(json) + strlen(dst) + strlen(ns) + 64;
	uint8_t *buf = malloc(cap + 4);
	if (!buf)
		return -1;
	pb m;
	pb_init(&m, buf + 4, cap);
	pb_varint(&m, 1, 0);
	pb_str(&m, 2, SENDER);
	pb_str(&m, 3, dst);
	pb_str(&m, 4, ns);
	pb_varint(&m, 5, 0);
	pb_str(&m, 6, json);
	int rc = -1;
	if (pb_ok(&m)) {
		buf[0] = (uint8_t)(m.len >> 24);
		buf[1] = (uint8_t)(m.len >> 16);
		buf[2] = (uint8_t)(m.len >> 8);
		buf[3] = (uint8_t)m.len;
		rc = tls_write(c, buf, m.len + 4) < 0 ? -1 : 0;
	}
	free(buf);
	return rc;
}

static int read_full(tls_conn *c, uint8_t *p, size_t n)
{
	while (n) {
		int got = tls_read(c, p, n);
		if (got <= 0)
			return -1;
		p += got;
		n -= (size_t)got;
	}
	return 0;
}

static void copy_field(const uint8_t *buf, size_t len, uint32_t field,
                       char *out, size_t cap)
{
	pbv v;
	out[0] = '\0';
	if (cap && pb_get_bytes(buf, len, field, &v)) {
		const size_t n = v.len < cap - 1 ? v.len : cap - 1;
		memcpy(out, v.p, n);
		out[n] = '\0';
	}
}

/* Reads frames until one on `ns` carries a "type" listed in `types`
 * (space separated), answering heartbeat pings on the way. The payload is
 * left in `out`. */
static int cast_wait(tls_conn *c, const char *ns, const char *types,
                     char *out, size_t cap, int timeout_ms, char *err,
                     size_t errlen)
{
	const u64 end = osGetTime() + (u64)timeout_ms;
	uint8_t *body = malloc(CAST_FRAME_MAX);
	if (!body) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	int rc = -1;
	while (osGetTime() < end) {
		uint8_t hdr[4];
		if (read_full(c, hdr, 4) != 0) {
			snprintf(err, errlen, "the speaker closed the connection");
			break;
		}
		const size_t n = (size_t)hdr[0] << 24 | (size_t)hdr[1] << 16 |
		                 (size_t)hdr[2] << 8 | hdr[3];
		if (n > CAST_FRAME_MAX || read_full(c, body, n) != 0) {
			snprintf(err, errlen, "bad frame from the speaker");
			break;
		}
		char fns[64], from[64], type[40];
		copy_field(body, n, 4, fns, sizeof(fns));
		copy_field(body, n, 2, from, sizeof(from));
		copy_field(body, n, 6, out, cap);
		json_str(out, "type", type, sizeof(type));
		if (strcmp(fns, NS_HEARTBEAT) == 0 && strcmp(type, "PING") == 0) {
			cast_send(c, from, NS_HEARTBEAT, "{\"type\":\"PONG\"}");
			continue;
		}
		if (strcmp(fns, ns) != 0 || !type[0])
			continue;
		/* Whole-word match against the wanted list. */
		const char *hit = strstr(types, type);
		const size_t tl = strlen(type);
		if (hit && (hit == types || hit[-1] == ' ') &&
		    (hit[tl] == '\0' || hit[tl] == ' ')) {
			rc = 0;
			break;
		}
	}
	if (rc != 0 && !err[0])
		snprintf(err, errlen, "the speaker did not answer");
	free(body);
	return rc;
}

/* The first float after "key": in a JSON text; -1 when there is none. */
static float json_float(const char *json, const char *key)
{
	char pat[40];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(json, pat);
	return p ? strtof(p + strlen(pat), NULL) : -1.0f;
}

/* device-auth: our token in, a token for the speaker's client out. */
static int speaker_token(const char *sp_host, const char *bearer,
                         const char *client_id, const char *dev,
                         char *out, size_t cap, char *err, size_t errlen)
{
	char body[160];
	snprintf(body, sizeof(body), "{\"clientId\":\"%s\",\"deviceId\":\"%s\"}",
	         client_id, dev);
	http_reply r;
	memset(&r, 0, sizeof(r));
	if (http_request(sp_host, "/device-auth/v1/refresh", "POST", bearer,
	                 "text/plain;charset=UTF-8", "application/json",
	                 body, strlen(body), 0, &r) != 0) {
		snprintf(err, errlen, "device-auth: %s", r.err);
		return -1;
	}
	int rc = -1;
	if (r.status == 200 && json_str(r.body, "accessToken", out, cap))
		rc = 0;
	else
		snprintf(err, errlen, "device-auth: HTTP %d", r.status);
	http_reply_free(&r);
	return rc;
}

int cast_wake(const char *ip, const char *name, const char *sp_host,
              const char *bearer, const char *fallback,
              char dev[CAST_DEV_LEN], float *volume, char *err, size_t errlen)
{
	err[0] = '\0';
	*volume = -1.0f;
	cast_device_id(name, dev);
	tls_conn *c = tls_connect_lan(ip, 8009, 8000, err, errlen);
	if (!c)
		return -1;

	enum { CAP = 16384 };   /* a receiver status lists icons and namespaces */
	char *msg = malloc(CAP);
	char *tok = malloc(1024);
	int rc = -1;
	if (!msg || !tok) {
		snprintf(err, errlen, "out of memory");
		goto out;
	}

	if (cast_send(c, RECEIVER, NS_CONN, "{\"type\":\"CONNECT\"}") != 0 ||
	    cast_send(c, RECEIVER, NS_RECEIVER,
	              "{\"type\":\"LAUNCH\",\"appId\":\"" CAST_APP
	              "\",\"requestId\":1}") != 0) {
		snprintf(err, errlen, "could not talk to the speaker");
		goto out;
	}
	/* Status updates arrive until one lists Spotify with its transport. */
	char transport[64] = "";
	const u64 until = osGetTime() + 20000;
	while (!transport[0] && osGetTime() < until) {
		if (cast_wait(c, NS_RECEIVER,
		              "RECEIVER_STATUS LAUNCH_ERROR INVALID_REQUEST",
		              msg, CAP, 20000, err, errlen) != 0)
			goto out;
		char type[32];
		json_str(msg, "type", type, sizeof(type));
		if (strcmp(type, "RECEIVER_STATUS") != 0) {
			snprintf(err, errlen, "the speaker would not start Spotify");
			goto out;
		}
		const float lv = json_float(msg, "level");
		if (lv >= 0.0f)
			*volume = lv;
		const char *app = strstr(msg, "\"" CAST_APP "\"");
		if (app)
			json_str(app, "transportId", transport, sizeof(transport));
	}
	if (!transport[0]) {
		snprintf(err, errlen, "Spotify did not start on the speaker");
		goto out;
	}

	char line[256];
	snprintf(line, sizeof(line),
	         "{\"type\":\"getInfo\",\"payload\":{\"remoteName\":\"%s\","
	         "\"deviceID\":\"%s\",\"deviceAPI_isGroup\":false}}", name, dev);
	if (cast_send(c, transport, NS_CONN, "{\"type\":\"CONNECT\"}") != 0 ||
	    cast_send(c, transport, NS_SPOTIFY, line) != 0 ||
	    cast_wait(c, NS_SPOTIFY, "getInfoResponse getInfoError", msg, CAP,
	              10000, err, errlen) != 0)
		goto out;
	char client_id[48] = "";
	if (!json_str(msg, "clientID", client_id, sizeof(client_id))) {
		snprintf(err, errlen, "the speaker gave no client id");
		goto out;
	}
	/* The speaker reports the id it will register under; trust it. */
	char their_dev[CAST_DEV_LEN];
	if (json_str(msg, "deviceID", their_dev, sizeof(their_dev)) &&
	    strlen(their_dev) == CAST_DEV_LEN - 1)
		memcpy(dev, their_dev, CAST_DEV_LEN);

	char aerr[96] = "";
	if (speaker_token(sp_host, bearer, client_id, dev, tok, 1024, aerr,
	                  sizeof(aerr)) != 0 &&
	    (!fallback || !fallback[0] ||
	     speaker_token(sp_host, fallback, client_id, dev, tok, 1024, aerr,
	                   sizeof(aerr)) != 0)) {
		snprintf(err, errlen, "%s", aerr);
		goto out;
	}

	const size_t ulen = strlen(tok) + 96;
	char *add = malloc(ulen);
	if (!add) {
		snprintf(err, errlen, "out of memory");
		goto out;
	}
	snprintf(add, ulen, "{\"type\":\"addUser\",\"payload\":{\"blob\":\"%s\","
	         "\"tokenType\":\"accesstoken\"}}", tok);
	const int sent = cast_send(c, transport, NS_SPOTIFY, add);
	free(add);
	if (sent != 0 ||
	    cast_wait(c, NS_SPOTIFY, "addUserResponse addUserError", msg, CAP,
	              15000, err, errlen) != 0)
		goto out;
	long status = 0;
	if (strstr(msg, "addUserError") || !json_int(msg, "status", &status) ||
	    status != 101) {
		snprintf(err, errlen, "the speaker refused the sign-in (%ld)",
		         status);
		goto out;
	}
	rc = 0;
	/* The receiver keeps playing without a sender; leaving cleanly keeps
	 * a speaker from piling up dead connections until it refuses more. */
	cast_send(c, transport, NS_CONN, "{\"type\":\"CLOSE\"}");
	cast_send(c, RECEIVER, NS_CONN, "{\"type\":\"CLOSE\"}");
out:
	if (tok)
		memset(tok, 0, 1024);
	free(tok);
	free(msg);
	tls_close(c);
	return rc;
}

/* Commands go over one kept-open link when the caller set one: a fresh
 * TLS handshake per pause costs this console a second or more. */
static http_session *cmd_sess;

void cast_use_session(http_session *s)
{
	cmd_sess = s;
}

static int connect_state(const char *sp_host, const char *method,
                         const char *path, const char *bearer,
                         const char *body, char *err, size_t errlen)
{
	http_reply r;
	memset(&r, 0, sizeof(r));
	const int rc = cmd_sess
	    ? http_session_request(cmd_sess, path, method, bearer,
	                           "application/json", "application/json",
	                           body, strlen(body), 0, &r)
	    : http_request(sp_host, path, method, bearer, "application/json",
	                   "application/json", body, strlen(body), 0, &r);
	if (rc != 0) {
		snprintf(err, errlen, "%s", r.err);
		return -1;
	}
	const int st = r.status;
	if (st != 200)
		snprintf(err, errlen, "connect-state: HTTP %d", st);
	http_reply_free(&r);
	return st;
}

int cast_command(const char *sp_host, const char *bearer, const char *dev,
                 const char *command_json, char *err, size_t errlen)
{
	char path[128];
	snprintf(path, sizeof(path),
	         "/connect-state/v1/player/command/from/%s/to/%s", dev, dev);
	const size_t n = strlen(command_json) + 16;
	char *body = malloc(n);
	if (!body) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	snprintf(body, n, "{\"command\":%s}", command_json);
	const int st = connect_state(sp_host, "POST", path, bearer, body, err,
	                             errlen);
	free(body);
	return st;
}

int cast_play(const char *sp_host, const char *bearer, const char *dev,
              const char *context, const char *track, long pos_ms,
              char *err, size_t errlen)
{
	char skip[160] = "{}";
	if (track && track[0])
		snprintf(skip, sizeof(skip), "{\"track_uri\":\"%s\"}", track);
	char cmd[640];
	snprintf(cmd, sizeof(cmd),
	         "{\"endpoint\":\"play\","
	         "\"context\":{\"uri\":\"%s\",\"url\":\"context://%s\","
	         "\"metadata\":{}},"
	         "\"play_origin\":{\"feature_identifier\":\"spotibacan\"},"
	         "\"options\":{\"license\":\"on-demand\",\"skip_to\":%s,"
	         "\"seek_to\":%ld,\"player_options_override\":{}}}",
	         context, context, skip, pos_ms > 0 ? pos_ms : 0L);
	return cast_command(sp_host, bearer, dev, cmd, err, errlen);
}

int cast_set_volume(const char *sp_host, const char *bearer, const char *dev,
                    float level, char *err, size_t errlen)
{
	if (level < 0.0f)
		level = 0.0f;
	if (level > 1.0f)
		level = 1.0f;
	char path[128], body[40];
	snprintf(path, sizeof(path),
	         "/connect-state/v1/connect/volume/from/%s/to/%s", dev, dev);
	snprintf(body, sizeof(body), "{\"volume\":%d}", (int)(level * 65535.0f));
	return connect_state(sp_host, "PUT", path, bearer, body, err, errlen);
}
