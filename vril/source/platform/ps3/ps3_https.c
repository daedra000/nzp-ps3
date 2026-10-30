/*
ps3_https.c: a small HTTP/1.1 GET client over the PS3's sockets, with TLS
from mbedTLS (built by CI, see .github/workflows/ps3.yml).

The system's own HTTPS (cellHttps) only speaks the TLS of ~2015: GitHub,
archive.org and most hosts refuse it (0x80710a06). mbedTLS does TLS 1.2 with
the Mozilla root certificates (ca_bundle.h, made by CI from the runner's
ca-certificates), and checks them.

One caller at a time (the map downloader's worker thread).
*/
#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_https.h"

#include <net/net.h>
#include <net/netdb.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sysmodule/sysmodule.h>
#include <lv2/system.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"	// the MBEDTLS_ERR_NET_ codes (NET_C itself is off)

#if __has_include("ca_bundle.h")
#include "ca_bundle.h"		// static const char ca_bundle_pem[]
#define HAVE_CA_BUNDLE 1
#endif

#define USER_AGENT		"NZP-PS3/2.0 (PLAYSTATION 3)"
#define MAX_REDIRECTS	8
#define IO_TIMEOUT_S	30

static int						tls_state;		// 0 not tried, 1 ok, -1 failed
static mbedtls_entropy_context	entropy;
static mbedtls_ctr_drbg_context	drbg;
static mbedtls_x509_crt			cacert;
static mbedtls_ssl_config		conf;
static int						num_roots;

typedef struct {
	int					fd;
	int					tls;
	mbedtls_ssl_context	ssl;
	unsigned char		buf[16384];
	int					pos, len;
	volatile int		*cancel;
} conn_t;

static void SetErr(char *err, int size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, size, fmt, ap);
	va_end(ap);
}

static int PS3Entropy(void *data, unsigned char *out, size_t len, size_t *olen)
{
	// lv2 hands out at most 0x1000 bytes a call
	size_t done = 0;
	while (done < len) {
		size_t n = len - done > 0x1000 ? 0x1000 : len - done;
		if (sysGetRandomNumber(out + done, n) != 0)
			return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
		done += n;
	}
	*olen = len;
	return 0;
}

static int Net_Up(void)
{
	static int net_state;
	s32 ret;

	if (net_state)
		return net_state > 0;
	net_state = -1;
	if ((ret = sysModuleLoad(SYSMODULE_NET)) < 0) { PS3_Log("https: load NET 0x%08x", (unsigned)ret); return 0; }
	if ((ret = netInitialize()) < 0) { PS3_Log("https: netInitialize 0x%08x", (unsigned)ret); return 0; }
	net_state = 1;
	return 1;
}

static int Tls_Up(void)
{
	int ret;

	if (tls_state)
		return tls_state > 0;
	tls_state = -1;

	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&cacert);
	mbedtls_ssl_config_init(&conf);

	mbedtls_entropy_add_source(&entropy, PS3Entropy, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
	if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"nzp-ps3", 7)) != 0) {
		PS3_Log("https: drbg seed -0x%04x", -ret);
		return 0;
	}
#ifdef HAVE_CA_BUNDLE
	ret = mbedtls_x509_crt_parse(&cacert, (const unsigned char *)ca_bundle_pem, sizeof(ca_bundle_pem));
	// >0: that many certificates didn't parse (odd algorithms): the rest are fine
	for (mbedtls_x509_crt *c = &cacert; c && c->raw.len; c = c->next)
		num_roots++;
	PS3_Log("https: %d root certificates (%d skipped)", num_roots, ret > 0 ? ret : 0);
#endif
	if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
			MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
		PS3_Log("https: config -0x%04x", -ret);
		return 0;
	}
	mbedtls_ssl_conf_authmode(&conf, num_roots ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	tls_state = 1;
	return 1;
}

/*
===================
Connection
===================
*/
static int BioSend(void *ctx, const unsigned char *buf, size_t len)
{
	conn_t *c = ctx;
	int n = send(c->fd, buf, len, 0);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
	return n;
}

static int BioRecv(void *ctx, unsigned char *buf, size_t len)
{
	conn_t *c = ctx;
	int n = recv(c->fd, buf, len, 0);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
	return n;
}

static void Conn_Close(conn_t *c)
{
	if (c->tls) {
		mbedtls_ssl_close_notify(&c->ssl);
		mbedtls_ssl_free(&c->ssl);
		c->tls = 0;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

static int Conn_Open(conn_t *c, const char *host, int port, int tls, char *err, int errsize)
{
	struct hostent *he;
	struct sockaddr_in sa;
	struct timeval tv;
	int ret;

	c->fd = -1;
	c->tls = 0;
	c->pos = c->len = 0;

	he = gethostbyname(host);
	if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
		SetErr(err, errsize, "Can't find %s (is the PS3 online?)", host);
		return 0;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));

	if ((c->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		SetErr(err, errsize, "socket() failed (%d)", errno);
		return 0;
	}
	tv.tv_sec = IO_TIMEOUT_S;
	tv.tv_usec = 0;
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		SetErr(err, errsize, "Can't connect to %s (%d)", host, errno);
		Conn_Close(c);
		return 0;
	}
	if (!tls)
		return 1;

	if (!Tls_Up()) {
		SetErr(err, errsize, "TLS unavailable");
		Conn_Close(c);
		return 0;
	}
	mbedtls_ssl_init(&c->ssl);
	c->tls = 1;
	if ((ret = mbedtls_ssl_setup(&c->ssl, &conf)) != 0 || (ret = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0) {
		SetErr(err, errsize, "TLS setup -0x%04x", -ret);
		Conn_Close(c);
		return 0;
	}
	mbedtls_ssl_set_bio(&c->ssl, c, BioSend, BioRecv, NULL);
	while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			char info[256];
			mbedtls_x509_crt_verify_info(info, sizeof(info), "", mbedtls_ssl_get_verify_result(&c->ssl));
			for (char *p = info; *p; p++) if (*p == '\n') *p = ' ';
			PS3_Log("https: %s: certificate rejected: %s", host, info);
			SetErr(err, errsize, "%s's certificate was rejected (PS3 clock right?)", host);
		} else {
			char e[96];
			mbedtls_strerror(ret, e, sizeof(e));
			PS3_Log("https: %s: handshake -0x%04x %s", host, -ret, e);
			SetErr(err, errsize, "Secure connection to %s failed (-0x%04x)", host, -ret);
		}
		Conn_Close(c);
		return 0;
	}
	return 1;
}

static int Conn_Write(conn_t *c, const char *buf, int len)
{
	while (len > 0) {
		int n = c->tls ? mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf, len) : send(c->fd, buf, len, 0);
		if (c->tls && (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE))
			continue;
		if (n <= 0)
			return 0;
		buf += n;
		len -= n;
	}
	return 1;
}

// refills c->buf: >0 bytes, 0 at the end of the stream, <0 an error
static int Conn_Fill(conn_t *c)
{
	int n;
	for (;;) {
		if (c->cancel && *c->cancel)
			return -2;
		n = c->tls ? mbedtls_ssl_read(&c->ssl, c->buf, sizeof(c->buf)) : recv(c->fd, c->buf, sizeof(c->buf), 0);
		if (c->tls) {
			if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
			if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
				n = 0;
		}
		break;
	}
	c->pos = 0;
	c->len = n > 0 ? n : 0;
	return n;
}

// one header line, without its CRLF
static int Conn_Line(conn_t *c, char *out, int size)
{
	int o = 0;
	for (;;) {
		if (c->pos >= c->len && Conn_Fill(c) <= 0)
			return 0;
		char ch = c->buf[c->pos++];
		if (ch == '\n') {
			if (o && out[o - 1] == '\r') o--;
			out[o] = 0;
			return 1;
		}
		if (o < size - 1)
			out[o++] = ch;
	}
}

/*
===================
URL
===================
*/
typedef struct {
	int		tls;
	int		port;
	char	host[256];
	char	path[2048];
} url_t;

static int Url_Parse(const char *in, url_t *u)
{
	const char *p, *slash, *colon;
	int hlen, o = 0;

	if (!strncasecmp(in, "https://", 8)) { u->tls = 1; u->port = 443; p = in + 8; }
	else if (!strncasecmp(in, "http://", 7)) { u->tls = 0; u->port = 80; p = in + 7; }
	else return 0;

	slash = strchr(p, '/');
	if (!slash) slash = p + strlen(p);
	colon = memchr(p, ':', slash - p);
	hlen = (colon ? colon : slash) - p;
	if (hlen <= 0 || hlen >= (int)sizeof(u->host))
		return 0;
	memcpy(u->host, p, hlen);
	u->host[hlen] = 0;
	if (colon)
		u->port = atoi(colon + 1);
	// the path, spaces escaped (zip names have them)
	if (!*slash) slash = "/";
	for (; *slash && o < (int)sizeof(u->path) - 4; slash++) {
		if (*slash == ' ') { memcpy(u->path + o, "%20", 3); o += 3; }
		else u->path[o++] = *slash;
	}
	u->path[o] = 0;
	return 1;
}

/*
===================
PS3_HttpGet
===================
*/
int PS3_HttpGet(const char *url_in, http_sink_t sink, void *ctx, volatile unsigned long long *got,
	volatile unsigned long long *total, volatile int *cancel, char *err, int errsize)
{
	static conn_t c;		// 16 KB buffer: off the worker's stack
	static url_t u;
	char line[2048], location[2048];
	int redirects = 0, status, chunked, ok = 0;
	long long length;

	*got = 0;
	*total = 0;
	err[0] = 0;
	snprintf(location, sizeof(location), "%s", url_in);

	if (!Net_Up()) {
		SetErr(err, errsize, "Network unavailable");
		return 0;
	}

again:
	if (!Url_Parse(location, &u)) {
		SetErr(err, errsize, "Bad URL");
		return 0;
	}
	c.cancel = cancel;
	if (!Conn_Open(&c, u.host, u.port, u.tls, err, errsize))
		return 0;

	snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT
		"\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", u.path, u.host);
	if (!Conn_Write(&c, line, strlen(line))) {
		SetErr(err, errsize, "Request to %s failed", u.host);
		goto done;
	}

	// status line + headers
	if (!Conn_Line(&c, line, sizeof(line)) || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
		SetErr(err, errsize, "No reply from %s", u.host);
		goto done;
	}
	length = -1;
	chunked = 0;
	location[0] = 0;
	for (;;) {
		if (!Conn_Line(&c, line, sizeof(line))) {
			SetErr(err, errsize, "Reply from %s cut short", u.host);
			goto done;
		}
		if (!line[0])
			break;
		if (!strncasecmp(line, "Content-Length:", 15))
			length = strtoll(line + 15, NULL, 10);
		else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strstr(line + 18, "chunked"))
			chunked = 1;
		else if (!strncasecmp(line, "Location:", 9)) {
			const char *v = line + 9;
			while (*v == ' ') v++;
			if (*v == '/')		// same host
				snprintf(location, sizeof(location), "%s://%s%s", u.tls ? "https" : "http", u.host, v);
			else
				snprintf(location, sizeof(location), "%s", v);
		}
	}

	if (status >= 300 && status < 400 && location[0]) {
		Conn_Close(&c);
		if (++redirects > MAX_REDIRECTS) {
			SetErr(err, errsize, "Too many redirects");
			return 0;
		}
		goto again;
	}
	if (status != 200) {
		SetErr(err, errsize, "%s said %d", u.host, status);
		PS3_Log("https: %s%s: status %d", u.host, u.path, status);
		goto done;
	}
	if (length >= 0)
		*total = length;

	// body
	for (;;) {
		long long left = chunked ? 0 : (length >= 0 ? length - (long long)*got : -1);
		if (chunked) {
			if (!Conn_Line(&c, line, sizeof(line))) { SetErr(err, errsize, "Download cut short"); goto done; }
			left = strtoll(line, NULL, 16);
			if (left == 0) { ok = 1; break; }
		}
		if (left == 0) { ok = 1; break; }
		while (left != 0) {
			int n;
			if (c.pos >= c.len) {
				int r = Conn_Fill(&c);
				if (r == -2) { SetErr(err, errsize, "Cancelled"); goto done; }
				if (r <= 0) {
					if (left < 0 && r == 0) { ok = 1; goto done; }	// no length: the end is the close
					SetErr(err, errsize, "Download cut short");
					goto done;
				}
			}
			n = c.len - c.pos;
			if (left > 0 && n > left)
				n = left;
			if (!sink(ctx, c.buf + c.pos, n)) {
				if (!err[0]) SetErr(err, errsize, "Couldn't save the download");
				goto done;
			}
			c.pos += n;
			*got += n;
			if (left > 0)
				left -= n;
		}
		if (!chunked) { ok = 1; break; }
		Conn_Line(&c, line, sizeof(line));		// the CRLF after a chunk
	}

done:
	Conn_Close(&c);
	return ok;
}
