/*
ps3_board.c: reads the ps3.world leaderboards for the LEADERBOARDS menu.

A worker thread GETs https://ps3.world/api/scores/maps (the map tabs) and
/api/scores?map=..&mode=solo|duo&range=all|week (one board). Boards are kept
for a minute, so going back and forth between tabs doesn't fetch them again.
The menu says which board it wants (PS3_Board_Want) and copies out whatever
there is for it (PS3_Board_Get); it never waits on the network.
*/
#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_https.h"
#include "ps3_board.h"
#include "qcvm/cJSON.h"

#include <sys/thread.h>
#include <sys/mutex.h>
#include <ctype.h>

#define BOARD_API		"https://ps3.world/api/scores"
#define BOARD_CACHE		16
#define BOARD_FRESH		60.0		// seconds a board is shown before it's fetched again

typedef struct {
	char	key[96];		// map|duo|week
	double	when;			// Sys_FloatTime of the answer
	board_t	board;
} cached_t;

static sys_mutex_t		lock;
static int				lock_made;
static volatile int		worker_alive, worker_again;

static board_map_t		maps[BOARD_MAX_MAPS];
static int				num_maps;
static volatile int		maps_state = BOARD_NONE;
static volatile int		maps_wanted;

static cached_t			cache[BOARD_CACHE];
static char				want_key[96];			// the board the menu shows
static char				fail_key[96];			// the last board that failed
static char				loading_key[96];		// the board being fetched
static char				message[128];

static void Lock(void)
{
	if (!lock_made) {
		sys_mutex_attr_t attr;
		sysMutexAttrInitialize(attr);
		if (sysMutexCreate(&lock, &attr) == 0)
			lock_made = 1;
	}
	if (lock_made) sysMutexLock(lock, 0);
}
static void Unlock(void) { if (lock_made) sysMutexUnlock(lock); }

typedef struct { char *buf; int len, cap; } membuf_t;

static int MemSink(void *ctx, const void *data, int len)
{
	membuf_t *m = ctx;
	if (m->len + len >= m->cap)
		return 0;
	memcpy(m->buf + m->len, data, len);
	m->len += len;
	m->buf[m->len] = 0;
	return 1;
}

static const char *Str(cJSON *o, const char *k)
{
	cJSON *v = cJSON_GetObjectItem(o, k);
	return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static int Int(cJSON *o, const char *k)
{
	cJSON *v = cJSON_GetObjectItem(o, k);
	return cJSON_IsNumber(v) ? (int)v->valuedouble : (cJSON_IsTrue(v) ? 1 : 0);
}

// the site's names are ASCII already (it cleans them); this keeps the font safe anyway
static void Copy(char *dst, int size, const char *s)
{
	int n = 0;
	for (; *s && n < size - 1; s++)
		if ((unsigned char)*s >= 32 && (unsigned char)*s < 127)
			dst[n++] = *s;
	dst[n] = 0;
}

static void UrlEncode(char *out, int size, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	int n = 0;
	for (; *s && n < size - 4; s++) {
		unsigned char c = *s;
		if (isalnum(c) || c == '_' || c == '-' || c == '.')
			out[n++] = c;
		else {
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		}
	}
	out[n] = 0;
}

// GET url into buf; returns the parsed JSON or NULL (message says why)
static cJSON *Fetch(const char *url, char *buf, int cap)
{
	membuf_t m = {buf, 0, cap};
	volatile unsigned long long got = 0, total = 0;
	volatile int no_cancel = 0;
	char err[128];
	int status = 0;
	cJSON *j;

	buf[0] = 0;
	if (!PS3_HttpRequest("GET", url, NULL, 0, NULL, MemSink, &m, &got, &total, &no_cancel,
			err, sizeof(err), &status)) {
		Lock();
		snprintf(message, sizeof(message), "%s", err[0] ? err : "no connection");
		Unlock();
		PS3_Log("board: %s: %s", url, err);
		return NULL;
	}
	if (status != 200 || !(j = cJSON_Parse(buf))) {
		Lock();
		snprintf(message, sizeof(message), "ps3.world answered %d", status);
		Unlock();
		PS3_Log("board: %s: HTTP %d", url, status);
		return NULL;
	}
	return j;
}

static void FetchMaps(char *buf, int cap)
{
	cJSON *j = Fetch(BOARD_API "/maps?game=nzp", buf, cap);
	board_map_t got[BOARD_MAX_MAPS];
	int n = 0;

	if (j) {
		const char *lists[2] = {"stock", "custom"};
		for (int l = 0; l < 2; l++) {
			cJSON *arr = cJSON_GetObjectItem(j, lists[l]), *m;
			cJSON_ArrayForEach(m, arr) {
				if (n >= BOARD_MAX_MAPS)
					break;
				Copy(got[n].id, sizeof(got[n].id), Str(m, "id"));
				Copy(got[n].name, sizeof(got[n].name), Str(m, "name"));
				got[n].games = Int(m, "games");
				got[n].best = Int(m, "best");
				got[n].custom = l;
				if (got[n].id[0])
					n++;
			}
		}
		cJSON_Delete(j);
	}
	Lock();
	if (j) {
		memcpy(maps, got, sizeof(got[0]) * n);
		num_maps = n;
	}
	maps_state = j ? BOARD_READY : BOARD_FAILED;
	Unlock();
	PS3_Log("board: %d maps", n);
}

static void FetchBoard(const char *key, char *buf, int cap)
{
	char map[64], enc[200], url[320];
	int duo = 0, week = 0, slot = 0;
	board_t b;
	cJSON *j, *rows, *r;
	const char *bar = strchr(key, '|');

	memset(&b, 0, sizeof(b));
	snprintf(map, sizeof(map), "%.*s", bar ? (int)(bar - key) : (int)strlen(key), key);
	if (bar) sscanf(bar + 1, "%d|%d", &duo, &week);
	UrlEncode(enc, sizeof(enc), map);
	snprintf(url, sizeof(url), BOARD_API "?game=nzp&map=%s&range=%s&mode=%s",
		enc, week ? "week" : "all", duo ? "duo" : "solo");

	j = Fetch(url, buf, cap);
	if (!j) {
		Lock();
		snprintf(fail_key, sizeof(fail_key), "%s", key);
		Unlock();
		return;
	}
	b.state = BOARD_READY;
	{
		cJSON *st = cJSON_GetObjectItem(j, "stats");
		b.games = Int(st, "games");
		b.players = Int(st, "players");
		b.best = Int(st, "best");
	}
	rows = cJSON_GetObjectItem(j, "rows");
	cJSON_ArrayForEach(r, rows) {
		board_row_t *o;
		if (b.num_rows >= BOARD_MAX_ROWS)
			break;
		o = &b.rows[b.num_rows++];
		Copy(o->name, sizeof(o->name), Str(r, "name"));
		Copy(o->name2, sizeof(o->name2), Str(r, "name2"));
		o->value = Int(r, "value");
		o->value2 = Int(r, "value2");
		o->seconds = Int(r, "seconds");
		o->players = Int(r, "players");
		o->record = Int(r, "record");
	}
	cJSON_Delete(j);

	// into the cache: the same board's slot, or the oldest
	Lock();
	for (int i = 0; i < BOARD_CACHE; i++) {
		if (!strcmp(cache[i].key, key)) { slot = i; break; }
		if (cache[i].when < cache[slot].when) slot = i;
	}
	snprintf(cache[slot].key, sizeof(cache[slot].key), "%s", key);
	cache[slot].when = Sys_FloatTime();
	cache[slot].board = b;
	if (!strcmp(fail_key, key))
		fail_key[0] = 0;
	Unlock();
}

static cached_t *Cached(const char *key)
{
	for (int i = 0; i < BOARD_CACHE; i++)
		if (cache[i].key[0] && !strcmp(cache[i].key, key))
			return &cache[i];
	return NULL;
}

static void Worker(void *arg)
{
	static char buf[32 * 1024];
	(void)arg;

	do {
		char key[96];
		cached_t *c;

		worker_again = 0;
		if (maps_wanted) {
			maps_wanted = 0;
			FetchMaps(buf, sizeof(buf));
		}
		// the board the menu wants now (it may have changed while the last loaded)
		Lock();
		snprintf(key, sizeof(key), "%s", want_key);
		c = key[0] ? Cached(key) : NULL;
		if (key[0] && (!c || Sys_FloatTime() - c->when > BOARD_FRESH) && strcmp(fail_key, key))
			snprintf(loading_key, sizeof(loading_key), "%s", key);
		else
			key[0] = 0;
		Unlock();
		if (key[0]) {
			FetchBoard(key, buf, sizeof(buf));
			worker_again = 1;		// see if the menu moved on meanwhile
		}
		Lock();
		loading_key[0] = 0;
		Unlock();
	} while (worker_again || maps_wanted);
	worker_alive = 0;
	sysThreadExit(0);
}

static void Kick(void)
{
	sys_ppu_thread_t tid;

	worker_again = 1;
	__sync_synchronize();
	if (!__sync_bool_compare_and_swap(&worker_alive, 0, 1))
		return;
	if (sysThreadCreate(&tid, Worker, NULL, 2100, 256 * 1024, 0, (char *)"nzp_board") < 0)
		worker_alive = 0;
}

void PS3_Board_FetchMaps(void)
{
	Lock(); Unlock();		// the lock is made on the main thread, before the worker runs
	maps_state = BOARD_LOADING;
	maps_wanted = 1;
	Kick();
}

int PS3_Board_MapsState(void) { return maps_state; }
int PS3_Board_NumMaps(void) { return num_maps; }

const board_map_t *PS3_Board_Map(int i)
{
	return (i >= 0 && i < num_maps) ? &maps[i] : NULL;
}

void PS3_Board_Want(const char *map, int duo, int week)
{
	char key[96];
	cached_t *c;
	int fetch;

	snprintf(key, sizeof(key), "%s|%d|%d", map, duo ? 1 : 0, week ? 1 : 0);
	Lock();
	if (strcmp(want_key, key)) {
		snprintf(want_key, sizeof(want_key), "%s", key);
		fail_key[0] = 0;		// a new choice: try again even if it failed before
	}
	c = Cached(key);
	fetch = (!c || Sys_FloatTime() - c->when > BOARD_FRESH) && strcmp(fail_key, key)
		&& strcmp(loading_key, key);
	Unlock();
	if (fetch)
		Kick();
}

void PS3_Board_Get(board_t *out)
{
	cached_t *c;

	Lock();
	c = want_key[0] ? Cached(want_key) : NULL;
	if (c) {
		*out = c->board;
	} else {
		memset(out, 0, sizeof(*out));
		out->state = !strcmp(fail_key, want_key) && want_key[0] ? BOARD_FAILED : BOARD_LOADING;
	}
	Unlock();
}

const char *PS3_Board_Message(void)
{
	return message;
}
