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

#include "ps3_https.h"
#include <sys/thread.h>
#include <lv2/thread.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <stdint.h>

#ifndef PS3_MAPS_URL_DEFAULT
// tools/maps_list.py builds it daily (.github/workflows/maps.yml)
#define PS3_MAPS_URL_DEFAULT "https://raw.githubusercontent.com/daedra000/nzp-ps3/maps-list/maps.json"
#endif

cvar_t ps3_maps_url = {"ps3_maps_url", PS3_MAPS_URL_DEFAULT, false};

// stb_image.h (images.c): raw deflate into a buffer of known size
extern int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

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
static char					recv_buf[RECV_CHUNK];

/*
===================
HTTP(S): ps3_https.c
===================
*/
typedef struct {
	FILE	*fp;
	char	*mem;
	u32		cap, len;
} sink_ctx_t;

static int Sink(void *ctx, const void *data, int len)
{
	sink_ctx_t *s = ctx;
	if (s->fp) {
		if (fwrite(data, 1, len, s->fp) != (size_t)len) {
			snprintf(dl_msg, sizeof(dl_msg), "Disk full?");
			return 0;
		}
		return 1;
	}
	if (s->len + len >= s->cap) {
		snprintf(dl_msg, sizeof(dl_msg), "List too big");
		return 0;
	}
	memcpy(s->mem + s->len, data, len);
	s->len += len;
	s->mem[s->len] = 0;
	return 1;
}

/*
Http_Get: url into fp (a file) or mem (up to memcap-1 bytes, 0-terminated).
Returns true with the whole body; dl_msg says why not.
*/
static qboolean Http_Get(const char *url, FILE *fp, char *mem, u32 memcap, u32 *memlen)
{
	sink_ctx_t s = {fp, mem, memcap, 0};
	char err[128];
	int ok;

	dl_msg[0] = 0;
	ok = PS3_HttpGet(url, Sink, &s, &dl_got, &dl_total, &dl_cancel, err, sizeof(err));
	if (memlen)
		*memlen = s.len;
	if (!ok) {
		if (!dl_msg[0])
			snprintf(dl_msg, sizeof(dl_msg), "%s", err);
		PS3_Log("mapdl: %s: %s", url, dl_msg);
	}
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

// what the last zip put down, for maps/<bsp>.mapdl (the map manager deletes from it)
static char		*installed_files;
static int		installed_len, installed_cap;

static void NoteFile(const char *name)
{
	int len = strlen(name) + 1;
	if (installed_len + len > installed_cap) {
		int cap = installed_cap ? installed_cap * 2 : 16384;
		char *p;
		while (cap < installed_len + len) cap *= 2;
		if (!(p = realloc(installed_files, cap)))
			return;
		installed_files = p;
		installed_cap = cap;
	}
	memcpy(installed_files + installed_len, name, len - 1);
	installed_files[installed_len + len - 1] = '\n';
	installed_len += len;
}

static void WriteManifests(void)
{
	char path[MAX_OSPATH * 2];
	for (int i = 0; i < num_new_bsps; i++) {
		FILE *f;
		snprintf(path, sizeof(path), "%s/maps/%s.mapdl", com_gamedir, new_bsps[i]);
		if ((f = fopen(path, "wb"))) {
			fwrite(installed_files, 1, installed_len, f);
			fclose(f);
		}
	}
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
	installed_len = 0;

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
		NoteFile(name);
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
	WriteManifests();
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

/*
===================
PS3_MapDL_Delete (main thread): removes a map's files. Files a downloaded
zip put down (maps/<bsp>.mapdl) go too, unless another download's list
also has them; for a map copied by hand, its maps/ files and thumbnail.
===================
*/
static char *ReadText(const char *path)
{
	FILE *f = fopen(path, "rb");
	long len;
	char *buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(len + 1);
	if (buf) {
		len = fread(buf, 1, len, f);
		buf[len] = 0;
	}
	fclose(f);
	return buf;
}

static qboolean ListHas(const char *list, const char *line)
{
	int len = strlen(line);
	for (const char *p = list; p && *p; ) {
		const char *nl = strchr(p, '\n');
		int l = nl ? (int)(nl - p) : (int)strlen(p);
		if (l == len && !strncasecmp(p, line, len))
			return true;
		p = nl ? nl + 1 : p + l;
	}
	return false;
}

static int RemoveRel(const char *rel)
{
	char path[MAX_OSPATH * 2];
	snprintf(path, sizeof(path), "%s/%s", com_gamedir, rel);
	return unlink(path) == 0;
}

int PS3_MapDL_Delete(const char *bsp)
{
	static const char *map_exts[] = {"bsp", "txt", "way", "lit", "ent", "mapdl", NULL};
	static const char *img_exts[] = {"png", "tga", "jpg", NULL};
	char path[MAX_OSPATH * 2], rel[MAX_OSPATH], line[MAX_OSPATH];
	char *mine, *others = NULL;
	int removed = 0, others_len = 0, i;
	DIR *dir;
	struct dirent *de;

	if (busy && (dl_state == MAPDL_DOWNLOADING || dl_state == MAPDL_EXTRACTING))
		return -1;

	snprintf(path, sizeof(path), "%s/maps/%s.mapdl", com_gamedir, bsp);
	mine = ReadText(path);
	if (mine) {
		// every other download's file list: what they share stays
		snprintf(path, sizeof(path), "%s/maps", com_gamedir);
		if ((dir = opendir(path))) {
			while ((de = readdir(dir))) {
				int nlen = strlen(de->d_name);
				char *t;
				if (nlen < 7 || strcasecmp(de->d_name + nlen - 6, ".mapdl"))
					continue;
				if (nlen - 6 == (int)strlen(bsp) && !strncasecmp(de->d_name, bsp, nlen - 6))
					continue;
				snprintf(path, sizeof(path), "%s/maps/%s", com_gamedir, de->d_name);
				if (!(t = ReadText(path)))
					continue;
				int tl = strlen(t);
				char *n = realloc(others, others_len + tl + 2);
				if (n) {
					others = n;
					memcpy(others + others_len, t, tl);
					others_len += tl;
					others[others_len++] = '\n';
					others[others_len] = 0;
				}
				free(t);
			}
			closedir(dir);
		}
		for (char *p = mine; *p; ) {
			char *nl = strchr(p, '\n');
			int l = nl ? (int)(nl - p) : (int)strlen(p);
			if (l > 0 && l < (int)sizeof(line)) {
				memcpy(line, p, l);
				line[l] = 0;
				if (!ListHas(others, line))
					removed += RemoveRel(line);
			}
			p = nl ? nl + 1 : p + l;
		}
		free(mine);
		free(others);
	}
	for (i = 0; map_exts[i]; i++) {
		snprintf(rel, sizeof(rel), "maps/%s.%s", bsp, map_exts[i]);
		removed += RemoveRel(rel);
	}
	for (i = 0; img_exts[i]; i++) {
		snprintf(rel, sizeof(rel), "gfx/menu/custom/%s.%s", bsp, img_exts[i]);
		removed += RemoveRel(rel);
	}
	for (i = 0; i < num_entries; i++)
		if (!strcasecmp(entries[i].bsp, bsp))
			entries[i].installed = BspExists(bsp);
	PS3_Log("mapdl: deleted %s (%d files)", bsp, removed);
	return removed;
}
