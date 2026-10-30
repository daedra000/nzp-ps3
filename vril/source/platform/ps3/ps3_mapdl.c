/*
ps3_mapdl.c: the map downloader.

ps3_maps_url points at a maps.json:
  {"maps":[{"name":"Croft Manor","author":"...","version":"1.0",
            "file":"croft.zip","size":12345678,"bsp":"croft",
            "desc":["line one","line two"]}]}
"file" is relative to the list's URL (or a full http(s) URL). "bsp" is the
map's name in maps/ (default: the zip's name), used to show INSTALLED.

Everything network or disk runs on one worker thread; the menu reads the
state fields and calls PS3_MapDL_TakeInstalled to add new maps to its list.
A zip is saved to nzp/mapdl.tmp, unpacked into nzp/ (a single top folder,
e.g. "croft/maps/..." or "nzp/maps/...", is stripped), then deleted.
*/
#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_mapdl.h"
#include "qcvm/cJSON.h"

#include <http/https.h>
#include <http/util.h>
#include <ssl/ssl.h>
#include <net/net.h>
#include <sysmodule/sysmodule.h>
#include <sys/thread.h>
#include <lv2/thread.h>
#include <sys/stat.h>
#include <stdint.h>

#ifndef PS3_MAPS_URL_DEFAULT
#define PS3_MAPS_URL_DEFAULT ""
#endif

cvar_t ps3_maps_url = {"ps3_maps_url", PS3_MAPS_URL_DEFAULT, false};

// stb_image.h (images.c): raw deflate into a buffer of known size
extern int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

#define USER_AGENT		"Mozilla/5.0 (PLAYSTATION 3; 4.90) NZP"
#define LIST_MAX		(512 * 1024)
#define RECV_CHUNK		(64 * 1024)

enum { JOB_LIST, JOB_DOWNLOAD };

static mapdl_entry_t		entries[MAPDL_MAX_MAPS];
static volatile int			num_entries;
static volatile int			list_state = MAPDL_LIST_NONE;

static volatile int			dl_state = MAPDL_IDLE;
static volatile int			dl_index = -1;
static volatile int			dl_cancel;
static volatile unsigned long long dl_got, dl_total;
static char					dl_msg[128];

static char					new_bsps[16][64];
static volatile int			num_new_bsps;
static volatile int			new_taken = 1;

static volatile int			busy;
static int					net_ready;		// 0 not tried, 1 ok, -1 failed
static char					recv_buf[RECV_CHUNK];

/*
===================
HTTP(S)
===================
*/
static void *http_pool, *ssl_pool, *cert_buf;
static httpsData ca_list;

static int Net_Init(void)
{
	s32 ret;
	u32 cert_size = 0;

	if (net_ready)
		return net_ready > 0;
	net_ready = -1;

	if ((ret = sysModuleLoad(SYSMODULE_NET)) < 0) { PS3_Log("mapdl: load NET 0x%08x", (unsigned)ret); return 0; }
	if ((ret = netInitialize()) < 0) { PS3_Log("mapdl: netInitialize 0x%08x", (unsigned)ret); return 0; }
	if ((ret = sysModuleLoad(SYSMODULE_HTTP)) < 0) { PS3_Log("mapdl: load HTTP 0x%08x", (unsigned)ret); return 0; }
	http_pool = malloc(0x10000);
	if (!http_pool || (ret = httpInit(http_pool, 0x10000)) < 0) { PS3_Log("mapdl: httpInit 0x%08x", (unsigned)ret); return 0; }
	net_ready = 1;		// plain http works from here

	if ((ret = sysModuleLoad(SYSMODULE_HTTPS)) < 0 || (ret = sysModuleLoad(SYSMODULE_SSL)) < 0) {
		PS3_Log("mapdl: load HTTPS/SSL 0x%08x (http only)", (unsigned)ret);
		return 1;
	}
	ssl_pool = malloc(0x40000);
	if (!ssl_pool || (ret = sslInit(ssl_pool, 0x40000)) < 0) { PS3_Log("mapdl: sslInit 0x%08x (http only)", (unsigned)ret); return 1; }
	if ((ret = sslCertificateLoader(SSL_LOAD_CERT_ALL, NULL, 0, &cert_size)) < 0 || !(cert_buf = malloc(cert_size))) {
		PS3_Log("mapdl: cert size 0x%08x (http only)", (unsigned)ret);
		return 1;
	}
	if ((ret = sslCertificateLoader(SSL_LOAD_CERT_ALL, cert_buf, cert_size, NULL)) < 0) {
		PS3_Log("mapdl: certs 0x%08x (http only)", (unsigned)ret);
		return 1;
	}
	ca_list.ptr = cert_buf;
	ca_list.size = cert_size;
	if ((ret = httpsInit(1, &ca_list)) < 0)
		PS3_Log("mapdl: httpsInit 0x%08x (http only)", (unsigned)ret);
	return 1;
}

// The PS3's root certificates are from ~2017: a newer chain fails to verify.
// The files are maps from the list's own site, so carry on and log it.
static int SslCallback(s32 verErr, sslCert const sslCerts[], int certNum, const char *host, httpSslId id, void *arg)
{
	if (verErr)
		PS3_Log("mapdl: %s: certificate check 0x%08x, continuing", host ? host : "?", (unsigned)verErr);
	return 0;
}

// Spaces (and nothing else) need escaping in the names people give zips.
static void EscapeUrl(const char *in, char *out, int size)
{
	int o = 0;
	for (; *in && o < size - 4; in++) {
		if (*in == ' ') { memcpy(out + o, "%20", 3); o += 3; }
		else out[o++] = *in;
	}
	out[o] = 0;
}

/*
Http_Get: url into fp (a file) or mem (up to memcap-1 bytes, 0-terminated).
Returns true on a 200 with the whole body; dl_msg says why not.
*/
static qboolean Http_Get(const char *url_in, FILE *fp, char *mem, u32 memcap, u32 *memlen)
{
	httpClientId client = 0;
	httpTransId trans = 0;
	httpUri uri;
	void *uri_pool = NULL;
	u32 pool_size = 0, recvd;
	u64 length = 0;
	s32 code = 0, ret;
	qboolean ok = false;
	char url[1024];

	EscapeUrl(url_in, url, sizeof(url));
	dl_got = 0;
	dl_total = 0;
	if (memlen) *memlen = 0;

	if (!Net_Init()) { snprintf(dl_msg, sizeof(dl_msg), "Network unavailable"); return false; }

	if ((ret = httpCreateClient(&client)) < 0) { snprintf(dl_msg, sizeof(dl_msg), "httpCreateClient %08x", (unsigned)ret); goto end; }
	httpClientSetConnTimeout(client, 10 * 1000 * 1000);
	httpClientSetRecvTimeout(client, 30 * 1000 * 1000);
	httpClientSetUserAgent(client, USER_AGENT);
	httpClientSetAutoRedirect(client, 1);
	if (!strncasecmp(url, "https:", 6))
		httpClientSetSslCallback(client, SslCallback, NULL);

	if ((ret = httpUtilParseUri(&uri, url, NULL, 0, &pool_size)) < 0 || !(uri_pool = malloc(pool_size))
	 || (ret = httpUtilParseUri(&uri, url, uri_pool, pool_size, NULL)) < 0) {
		snprintf(dl_msg, sizeof(dl_msg), "Bad URL");
		goto end;
	}
	if ((ret = httpCreateTransaction(&trans, client, HTTP_METHOD_GET, &uri)) < 0
	 || (ret = httpSendRequest(trans, NULL, 0, NULL)) < 0) {
		snprintf(dl_msg, sizeof(dl_msg), "Can't connect (%08x)", (unsigned)ret);
		PS3_Log("mapdl: %s: send 0x%08x", url, (unsigned)ret);
		goto end;
	}
	if ((ret = httpResponseGetStatusCode(trans, &code)) < 0 || code != 200) {
		snprintf(dl_msg, sizeof(dl_msg), "Server said %d", (int)code);
		PS3_Log("mapdl: %s: status %d (0x%08x)", url, (int)code, (unsigned)ret);
		goto end;
	}
	if (httpResponseGetContentLength(trans, &length) >= 0)
		dl_total = length;

	for (;;) {
		recvd = 0;
		if ((ret = httpRecvResponse(trans, recv_buf, sizeof(recv_buf), &recvd)) < 0) {
			snprintf(dl_msg, sizeof(dl_msg), "Connection lost (%08x)", (unsigned)ret);
			PS3_Log("mapdl: %s: recv 0x%08x at %llu", url, (unsigned)ret, dl_got);
			goto end;
		}
		if (recvd == 0)
			break;
		if (dl_cancel) { snprintf(dl_msg, sizeof(dl_msg), "Cancelled"); goto end; }
		if (fp) {
			if (fwrite(recv_buf, 1, recvd, fp) != recvd) { snprintf(dl_msg, sizeof(dl_msg), "Disk full?"); goto end; }
		} else {
			if (*memlen + recvd >= memcap) { snprintf(dl_msg, sizeof(dl_msg), "List too big"); goto end; }
			memcpy(mem + *memlen, recv_buf, recvd);
			*memlen += recvd;
			mem[*memlen] = 0;
		}
		dl_got += recvd;
	}
	if (dl_total && dl_got != dl_total) {
		snprintf(dl_msg, sizeof(dl_msg), "Download cut short");
		goto end;
	}
	ok = true;

end:
	if (trans) httpDestroyTransaction(trans);
	if (client) httpDestroyClient(client);
	free(uri_pool);
	return ok;
}

/*
===================
List
===================
*/
static void CopyStr(char *dst, int size, cJSON *item)
{
	dst[0] = 0;
	if (cJSON_IsString(item) && item->valuestring)
		snprintf(dst, size, "%s", item->valuestring);
	else if (cJSON_IsNumber(item))
		snprintf(dst, size, "%g", item->valuedouble);
}

// worker thread: no va()/COM_ helpers here, they share static buffers with the game
static qboolean BspExists(const char *bsp)
{
	struct stat st;
	char path[MAX_OSPATH * 2];
	snprintf(path, sizeof(path), "%s/maps/%s.bsp", com_gamedir, bsp);
	return stat(path, &st) == 0;
}

// "dir/name.ext" -> "name"
static void BaseNoExt(const char *in, char *out, int size)
{
	const char *b = strrchr(in, '/'), *dot;
	int len;
	b = b ? b + 1 : in;
	dot = strrchr(b, '.');
	len = dot ? (int)(dot - b) : (int)strlen(b);
	if (len >= size) len = size - 1;
	memcpy(out, b, len);
	out[len] = 0;
}

static void ResolveUrl(const char *list_url, const char *file, char *out, int size)
{
	const char *slash;

	if (!strncasecmp(file, "http://", 7) || !strncasecmp(file, "https://", 8)) {
		snprintf(out, size, "%s", file);
		return;
	}
	slash = strrchr(list_url, '/');
	if (!slash || slash < list_url + 8)		// "https://host" with no path
		snprintf(out, size, "%s/%s", list_url, file);
	else
		snprintf(out, size, "%.*s%s", (int)(slash + 1 - list_url), list_url, file);
}

static void Job_List(void)
{
	char url[512];
	char *buf;
	u32 len = 0;
	cJSON *root, *maps;
	int i, n;

	snprintf(url, sizeof(url), "%s", ps3_maps_url.string);
	buf = malloc(LIST_MAX);
	if (!buf) { snprintf(dl_msg, sizeof(dl_msg), "Out of memory"); list_state = MAPDL_LIST_FAILED; return; }

	if (!Http_Get(url, NULL, buf, LIST_MAX, &len)) {
		free(buf);
		list_state = MAPDL_LIST_FAILED;
		return;
	}
	root = cJSON_Parse(buf);
	free(buf);
	maps = root ? cJSON_GetObjectItem(root, "maps") : NULL;
	if (!cJSON_IsArray(maps)) {
		snprintf(dl_msg, sizeof(dl_msg), "The map list isn't valid");
		cJSON_Delete(root);
		list_state = MAPDL_LIST_FAILED;
		return;
	}

	n = 0;
	for (i = 0; i < cJSON_GetArraySize(maps) && n < MAPDL_MAX_MAPS; i++) {
		cJSON *m = cJSON_GetArrayItem(maps, i), *desc, *size;
		mapdl_entry_t *e = &entries[n];
		char file[256];

		memset(e, 0, sizeof(*e));
		CopyStr(file, sizeof(file), cJSON_GetObjectItem(m, "file"));
		if (!file[0])
			continue;
		ResolveUrl(url, file, e->url, sizeof(e->url));
		CopyStr(e->name, sizeof(e->name), cJSON_GetObjectItem(m, "name"));
		CopyStr(e->author, sizeof(e->author), cJSON_GetObjectItem(m, "author"));
		CopyStr(e->version, sizeof(e->version), cJSON_GetObjectItem(m, "version"));
		CopyStr(e->bsp, sizeof(e->bsp), cJSON_GetObjectItem(m, "bsp"));
		if (!e->bsp[0])
			BaseNoExt(file, e->bsp, sizeof(e->bsp));
		if (!e->name[0])
			snprintf(e->name, sizeof(e->name), "%s", e->bsp);
		size = cJSON_GetObjectItem(m, "size");
		if (cJSON_IsNumber(size) && size->valuedouble > 0)
			e->size = (unsigned)size->valuedouble;
		desc = cJSON_GetObjectItem(m, "desc");
		if (cJSON_IsArray(desc)) {
			for (int j = 0; j < cJSON_GetArraySize(desc) && j < MAPDL_DESC_LINES; j++)
				CopyStr(e->desc[j], sizeof(e->desc[j]), cJSON_GetArrayItem(desc, j));
		} else {
			CopyStr(e->desc[0], sizeof(e->desc[0]), desc);
		}
		e->installed = BspExists(e->bsp);
		n++;
	}
	cJSON_Delete(root);
	num_entries = n;
	PS3_Log("mapdl: list has %d maps", n);
	list_state = MAPDL_LIST_READY;
}

/*
===================
Zip
===================
*/
static unsigned LE16(const byte *p) { return p[0] | (p[1] << 8); }
static unsigned LE32(const byte *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

static void MakeDirs(char *path)		// every parent of path
{
	char *p;
	for (p = path + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(path, 0777);
			*p = '/';
		}
	}
}

static qboolean KnownTopDir(const char *name, int len)
{
	static const char *dirs[] = {"maps", "gfx", "sounds", "sound", "progs", "models", "textures",
		"tracks", "music", "particles", "scripts", "env", "videos", NULL};
	for (int i = 0; dirs[i]; i++)
		if ((int)strlen(dirs[i]) == len && !strncasecmp(name, dirs[i], len))
			return true;
	return false;
}

// a path from the zip, cleaned: '\' to '/', the top folder stripped, nothing escaping nzp/
static qboolean CleanName(const byte *raw, int len, int strip, char *out, int size)
{
	int i, o = 0;

	if (len <= strip || len >= size)
		return false;
	for (i = strip; i < len; i++)
		out[o++] = raw[i] == '\\' ? '/' : raw[i];
	out[o] = 0;

	if (out[0] == '/' || strchr(out, ':'))
		return false;
	if (!strncmp(out, "__MACOSX/", 9) || strstr(out, ".DS_Store"))
		return false;
	for (char *p = out; (p = strstr(p, "..")); p += 2)
		if ((p == out || p[-1] == '/') && (p[2] == '/' || p[2] == 0))
			return false;
	return true;
}

static void NoteBsp(const char *name)
{
	int len = strlen(name);

	if (strncasecmp(name, "maps/", 5) || strchr(name + 5, '/') || len < 10)
		return;
	if (strcasecmp(name + len - 4, ".bsp") || num_new_bsps >= 16)
		return;
	BaseNoExt(name, new_bsps[num_new_bsps], 64);
	num_new_bsps++;
}

static qboolean Zip_Extract(const char *zip_path)
{
	FILE *f, *out = NULL;
	byte *tail = NULL, *cd = NULL, *comp = NULL, *raw = NULL;
	long size, tail_len, i;
	unsigned count, cd_size, cd_off, e, pos;
	int strip = -1;
	qboolean ok = false;
	char name[MAX_OSPATH], path[MAX_OSPATH * 2];

	f = fopen(zip_path, "rb");
	if (!f) { snprintf(dl_msg, sizeof(dl_msg), "Can't open the zip"); return false; }
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	tail_len = size < 65557 ? size : 65557;
	tail = malloc(tail_len);
	if (!tail || fseek(f, size - tail_len, SEEK_SET) || fread(tail, 1, tail_len, f) != (size_t)tail_len)
		goto bad;
	for (i = tail_len - 22; i >= 0; i--)
		if (LE32(tail + i) == 0x06054b50)
			break;
	if (i < 0)
		goto bad;
	count = LE16(tail + i + 10);
	cd_size = LE32(tail + i + 12);
	cd_off = LE32(tail + i + 16);
	if (cd_off + (long)cd_size > size || !(cd = malloc(cd_size)) || fseek(f, cd_off, SEEK_SET)
	 || fread(cd, 1, cd_size, f) != cd_size)
		goto bad;

	// one top folder over everything that isn't a game folder: strip it
	{
		const byte *first = NULL;
		for (e = 0, pos = 0; e < count && strip != 0; e++) {
			unsigned nlen;
			const byte *nm, *slash;
			if (pos + 46 > cd_size || LE32(cd + pos) != 0x02014b50)
				goto bad;
			nlen = LE16(cd + pos + 28);
			nm = cd + pos + 46;
			pos += 46 + nlen + LE16(cd + pos + 30) + LE16(cd + pos + 32);
			if (pos > cd_size)
				goto bad;
			if (nlen >= 9 && !strncmp((const char *)nm, "__MACOSX/", 9))
				continue;
			slash = memchr(nm, '/', nlen);
			if (!slash) slash = memchr(nm, '\\', nlen);
			if (!slash || KnownTopDir((const char *)nm, slash - nm))
				strip = 0;							// already laid out like nzp/
			else if (!first)
				first = nm, strip = slash - nm + 1;
			else if (slash - nm + 1 != strip || memcmp(nm, first, strip))
				strip = 0;							// several top folders: keep them
		}
		if (strip < 0)
			strip = 0;
	}

	dl_state = MAPDL_EXTRACTING;
	dl_got = 0;
	dl_total = count;
	num_new_bsps = 0;

	for (e = 0, pos = 0; e < count; e++, dl_got = e) {
		unsigned flags = LE16(cd + pos + 8), method = LE16(cd + pos + 10);
		unsigned csize = LE32(cd + pos + 20), usize = LE32(cd + pos + 24);
		unsigned nlen = LE16(cd + pos + 28), lho = LE32(cd + pos + 42);
		byte lh[30];
		const byte *nm = cd + pos + 46;
		int len;

		if (pos + 46 > cd_size || LE32(cd + pos) != 0x02014b50)
			goto bad;
		pos += 46 + nlen + LE16(cd + pos + 30) + LE16(cd + pos + 32);
		if (dl_cancel) { snprintf(dl_msg, sizeof(dl_msg), "Cancelled"); goto fail; }
		if (!CleanName(nm, nlen, strip, name, sizeof(name) - 6))
			continue;
		len = strlen(name);
		if (!strchr(name, '/') && len > 4) {
			// loose map files at the zip's root belong in maps/
			const char *ext = name + len - 4;
			if (!strcasecmp(ext, ".bsp") || !strcasecmp(ext, ".txt") || !strcasecmp(ext, ".way")
			 || !strcasecmp(ext, ".lit") || !strcasecmp(ext, ".ent")) {
				memmove(name + 5, name, len + 1);
				memcpy(name, "maps/", 5);
				len += 5;
			}
		}
		snprintf(path, sizeof(path), "%s/%s", com_gamedir, name);
		MakeDirs(path);
		if (name[len - 1] == '/')
			continue;
		if (flags & 1) { snprintf(dl_msg, sizeof(dl_msg), "Zip is encrypted"); goto fail; }
		if (method != 0 && method != 8) { snprintf(dl_msg, sizeof(dl_msg), "Zip method %u unsupported", method); goto fail; }
		if (fseek(f, lho, SEEK_SET) || fread(lh, 1, 30, f) != 30 || LE32(lh) != 0x04034b50
		 || fseek(f, lho + 30 + LE16(lh + 26) + LE16(lh + 28), SEEK_SET))
			goto bad;

		out = fopen(path, "wb");
		if (!out) { snprintf(dl_msg, sizeof(dl_msg), "Can't write %.80s", name); goto fail; }
		if (method == 0) {
			unsigned left = csize;
			while (left) {
				unsigned n = left < sizeof(recv_buf) ? left : sizeof(recv_buf);
				if (fread(recv_buf, 1, n, f) != n || fwrite(recv_buf, 1, n, out) != n)
					goto bad;
				left -= n;
			}
		} else {
			comp = malloc(csize ? csize : 1);
			raw = malloc(usize ? usize : 1);
			if (!comp || !raw) { snprintf(dl_msg, sizeof(dl_msg), "Not enough memory for %.70s", name); goto fail; }
			if (fread(comp, 1, csize, f) != csize)
				goto bad;
			if (usize && stbi_zlib_decode_noheader_buffer((char *)raw, usize, (const char *)comp, csize) != (int)usize) {
				snprintf(dl_msg, sizeof(dl_msg), "Corrupt file in zip: %.70s", name);
				goto fail;
			}
			if (fwrite(raw, 1, usize, out) != usize) { snprintf(dl_msg, sizeof(dl_msg), "Disk full?"); goto fail; }
			free(comp); comp = NULL;
			free(raw); raw = NULL;
		}
		fclose(out);
		out = NULL;
		NoteBsp(name);
	}
	ok = true;
	goto done;

bad:
	snprintf(dl_msg, sizeof(dl_msg), "The zip is damaged");
fail:
	ok = false;
done:
	if (out) fclose(out);
	fclose(f);
	free(tail); free(cd); free(comp); free(raw);
	return ok;
}

/*
===================
Download
===================
*/
static void Job_Download(int idx)
{
	mapdl_entry_t *e = &entries[idx];
	char tmp[MAX_OSPATH];
	FILE *fp;
	qboolean ok;

	snprintf(tmp, sizeof(tmp), "%s/mapdl.tmp", com_gamedir);
	fp = fopen(tmp, "wb");
	if (!fp) {
		snprintf(dl_msg, sizeof(dl_msg), "Can't write to the game folder");
		dl_state = MAPDL_FAILED;
		return;
	}
	PS3_Log("mapdl: downloading %s", e->url);
	ok = Http_Get(e->url, fp, NULL, 0, NULL);
	fclose(fp);
	if (ok) {
		PS3_Log("mapdl: %llu bytes, extracting", dl_got);
		ok = Zip_Extract(tmp);
	}
	unlink(tmp);
	if (!ok) {
		PS3_Log("mapdl: %s failed: %s", e->name, dl_msg);
		dl_state = MAPDL_FAILED;
		return;
	}
	e->installed = BspExists(e->bsp) || num_new_bsps > 0;
	snprintf(dl_msg, sizeof(dl_msg), "Installed");
	PS3_Log("mapdl: %s installed (%d maps)", e->name, num_new_bsps);
	new_taken = 0;
	dl_state = MAPDL_DONE;
}

/*
===================
Worker
===================
*/
static void Worker(void *arg)
{
	int job = (int)(intptr_t)arg;

	if (job == JOB_LIST)
		Job_List();
	else
		Job_Download(dl_index);
	busy = 0;
	sysThreadExit(0);
}

static qboolean StartJob(int job)
{
	sys_ppu_thread_t tid;
	s32 ret;

	if (busy)
		return false;
	busy = 1;
	dl_cancel = 0;
	// below the game (1000) and audio: the menu keeps its frame rate
	ret = sysThreadCreate(&tid, Worker, (void *)(intptr_t)job, 2000, 256 * 1024, 0, (char *)"nzp_mapdl");
	if (ret < 0) {
		PS3_Log("mapdl: sysThreadCreate 0x%08x", (unsigned)ret);
		snprintf(dl_msg, sizeof(dl_msg), "Couldn't start");
		busy = 0;
		return false;
	}
	return true;
}

/*
===================
Menu side
===================
*/
void PS3_MapDL_Init(void)
{
	Cvar_RegisterVariable(&ps3_maps_url);
}

void PS3_MapDL_FetchList(void)
{
	if (busy)
		return;
	if (!ps3_maps_url.string[0]) {
		snprintf(dl_msg, sizeof(dl_msg), "No map list set (ps3_maps_url)");
		list_state = MAPDL_LIST_FAILED;
		return;
	}
	list_state = MAPDL_LIST_LOADING;
	if (!StartJob(JOB_LIST))
		list_state = MAPDL_LIST_FAILED;
}

int PS3_MapDL_ListState(void) { return list_state; }
int PS3_MapDL_Count(void) { return list_state == MAPDL_LIST_READY ? num_entries : 0; }
mapdl_entry_t *PS3_MapDL_Entry(int i) { return (i >= 0 && i < num_entries) ? &entries[i] : NULL; }

void PS3_MapDL_Download(int i)
{
	if (busy || i < 0 || i >= num_entries)
		return;
	dl_index = i;
	dl_msg[0] = 0;
	dl_got = dl_total = 0;
	dl_state = MAPDL_DOWNLOADING;
	if (!StartJob(JOB_DOWNLOAD))
		dl_state = MAPDL_FAILED;
}

void PS3_MapDL_Cancel(void) { if (busy) dl_cancel = 1; }
int PS3_MapDL_State(void) { return dl_state; }
int PS3_MapDL_Active(void) { return dl_index; }
const char *PS3_MapDL_Message(void) { return dl_msg; }

void PS3_MapDL_Progress(unsigned long long *got, unsigned long long *total)
{
	*got = dl_got;
	*total = dl_total;
}

int PS3_MapDL_TakeInstalled(char names[][64], int max)
{
	int i, n;

	if (dl_state != MAPDL_DONE || new_taken)
		return 0;
	n = num_new_bsps < max ? num_new_bsps : max;
	for (i = 0; i < n; i++)
		snprintf(names[i], 64, "%s", new_bsps[i]);
	new_taken = 1;
	return n;
}
