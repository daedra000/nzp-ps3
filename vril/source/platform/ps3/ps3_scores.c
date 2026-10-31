/*
ps3_scores.c: the online leaderboard at ps3.world/leaderboards.

At game over (the intermission) the game's result goes into a queue file,
nzp/scores_queue.txt, one JSON line a game. A sender thread hashes the map
file, signs each line with NZP_SCORE_KEY (HMAC-SHA256, the key CI builds in
from the repo's secret) and POSTs it to ps3.world/api/scores. A line that
couldn't be sent (no network, the site busy) stays in the queue for the next
game or the next launch; a line the site refuses is dropped and logged.

ps3_leaderboard 0 turns it off.
*/
#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_https.h"
#include "qcvm/cJSON.h"

#include <sys/thread.h>
#include <lv2/thread.h>
#include <lv2/system.h>
#include <sys/mutex.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>

#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#ifndef NZP_SCORE_KEY
#define NZP_SCORE_KEY ""
#endif
#define SCORES_URL	"https://ps3.world/api/scores"

cvar_t ps3_leaderboard = {"ps3_leaderboard", "1", true};

static sys_mutex_t	queue_lock;
static int			queue_lock_made;
static volatile int	sender_alive;
static volatile int	sender_again;
static char			last_nonce[33];		// this game's line: its answer is shown at game over
static char			result_text[128];
static volatile int	result_seq;

extern char *game_build_date;

/*
===================
Queue file
===================
*/
static void QueuePath(char *out, int size)
{
	snprintf(out, size, "%s/scores_queue.txt", com_gamedir);
}

static void Lock(void) { if (queue_lock_made) sysMutexLock(queue_lock, 0); }
static void Unlock(void) { if (queue_lock_made) sysMutexUnlock(queue_lock); }

static void QueueAppend(const char *line)
{
	char path[MAX_OSPATH * 2];
	FILE *f;

	QueuePath(path, sizeof(path));
	Lock();
	if ((f = fopen(path, "ab"))) {
		fputs(line, f);
		fputc('\n', f);
		fclose(f);
	}
	Unlock();
}

// the whole queue, and the file emptied (what isn't sent is appended again)
static char *QueueTake(void)
{
	char path[MAX_OSPATH * 2];
	FILE *f;
	long len;
	char *buf = NULL;

	QueuePath(path, sizeof(path));
	Lock();
	if ((f = fopen(path, "rb"))) {
		fseek(f, 0, SEEK_END);
		len = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (len > 0 && len < 1024 * 1024 && (buf = malloc(len + 1))) {
			len = fread(buf, 1, len, f);
			buf[len] = 0;
		}
		fclose(f);
		unlink(path);
	}
	Unlock();
	return buf;
}

/*
===================
Sender
===================
*/
static void Hex(const unsigned char *in, int n, char *out)
{
	for (int i = 0; i < n; i++)
		snprintf(out + i * 2, 3, "%02x", in[i]);
}

// the map file's SHA-256 (a custom map's must be the download list's copy)
static void MapSum(const char *map, char *out)
{
	char path[MAX_OSPATH * 2];
	static unsigned char buf[65536];
	unsigned char d[32];
	mbedtls_sha256_context s;
	FILE *f;
	size_t n;

	out[0] = 0;
	snprintf(path, sizeof(path), "%s/maps/%s.bsp", com_gamedir, map);
	if (!(f = fopen(path, "rb")))
		return;
	mbedtls_sha256_init(&s);
	mbedtls_sha256_starts_ret(&s, 0);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		mbedtls_sha256_update_ret(&s, buf, n);
	fclose(f);
	mbedtls_sha256_finish_ret(&s, d);
	mbedtls_sha256_free(&s);
	Hex(d, 32, out);
}

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
	return cJSON_IsNumber(v) ? v->valueint : 0;
}

// 1 sent (or refused for good: dropped), 0 keep it for later
static int SendLine(const char *line)
{
	cJSON *o = cJSON_Parse(line), *reply;
	char text[1024], sig[65], mapsum[65], err[128], *body = NULL;
	unsigned char mac[32];
	static char answer[2048];
	membuf_t m = {answer, 0, sizeof(answer)};
	volatile unsigned long long got, total;
	volatile int no_cancel = 0;
	int status = 0, ok, done = 1;
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

	if (!o)
		return 1;		// a damaged line: nothing to keep
	answer[0] = 0;

	snprintf(mapsum, sizeof(mapsum), "%s", Str(o, "mapsum"));
	if (!mapsum[0]) {
		MapSum(Str(o, "map"), mapsum);
		cJSON_DeleteItemFromObject(o, "mapsum");
		cJSON_AddStringToObject(o, "mapsum", mapsum);
	}
	// what ps3.world signs too: game|map|mapsum|value|value2|seconds|players|name|build|ts|nonce
	snprintf(text, sizeof(text), "%s|%s|%s|%d|%d|%d|%d|%s|%s|%d|%s",
		Str(o, "game"), Str(o, "map"), mapsum, Int(o, "value"), Int(o, "value2"), Int(o, "seconds"),
		Int(o, "players"), Str(o, "name"), Str(o, "build"), Int(o, "ts"), Str(o, "nonce"));
	if (!md || mbedtls_md_hmac(md, (const unsigned char *)NZP_SCORE_KEY, strlen(NZP_SCORE_KEY),
			(const unsigned char *)text, strlen(text), mac) != 0) {
		cJSON_Delete(o);
		return 0;
	}
	Hex(mac, 32, sig);
	cJSON_DeleteItemFromObject(o, "sig");
	cJSON_AddStringToObject(o, "sig", sig);
	body = cJSON_PrintUnformatted(o);

	ok = body && PS3_HttpRequest("POST", SCORES_URL, body, strlen(body), "application/json",
		MemSink, &m, &got, &total, &no_cancel, err, sizeof(err), &status);
	reply = cJSON_Parse(answer);

	if (ok && status == 200 && reply) {
		PS3_Log("scores: %s round %d sent: #%d on %s", Str(o, "map"), Int(o, "value"),
			Int(reply, "rank"), Str(reply, "map_name"));
		if (!strcmp(Str(o, "nonce"), last_nonce)) {
			snprintf(result_text, sizeof(result_text), "You placed #%d on %s", Int(reply, "rank"), Str(reply, "map_name"));
			result_seq++;
		}
	} else if (ok && status >= 400 && status < 500 && status != 429) {
		// refused for good (not a map with a board, a bad build...): dropped
		PS3_Log("scores: %s round %d refused (%d): %s", Str(o, "map"), Int(o, "value"), status,
			reply ? Str(reply, "error") : answer);
		if (!strcmp(Str(o, "nonce"), last_nonce)) {
			snprintf(result_text, sizeof(result_text), "Not on the leaderboard: %.80s", reply ? Str(reply, "error") : "refused");
			result_seq++;
		}
	} else {
		// no network, the site busy or down: try again later
		PS3_Log("scores: %s round %d not sent yet (%d %s)", Str(o, "map"), Int(o, "value"), status, ok ? "" : err);
		if (!strcmp(Str(o, "nonce"), last_nonce)) {
			snprintf(result_text, sizeof(result_text), "Score saved: it'll be sent when ps3.world can be reached");
			result_seq++;
		}
		done = 0;
	}
	cJSON_Delete(reply);
	cJSON_Delete(o);
	free(body);
	return done;
}

static void Sender(void *arg)
{
	do {
		char *all, *p;
		int keep_rest = 0;

		sender_again = 0;
		all = QueueTake();
		for (p = all; p && *p; ) {
			char *nl = strchr(p, '\n');
			if (nl) *nl = 0;
			if (*p) {
				if (keep_rest || !SendLine(p)) {
					QueueAppend(p);
					keep_rest = 1;		// the site's not reachable: the rest waits too
				}
			}
			p = nl ? nl + 1 : p + strlen(p);
		}
		free(all);
	} while (sender_again);
	sender_alive = 0;
	sysThreadExit(0);
}

static void Kick(void)
{
	sys_ppu_thread_t tid;

	sender_again = 1;
	__sync_synchronize();
	if (!__sync_bool_compare_and_swap(&sender_alive, 0, 1))
		return;		// running: it goes round again
	// low priority: the game keeps its frame rate; 256 KB for TLS
	if (sysThreadCreate(&tid, Sender, NULL, 2100, 256 * 1024, 0, (char *)"nzp_scores") < 0)
		sender_alive = 0;
}

/*
===================
Game side
===================
*/
void PS3_Scores_Init(void)
{
	sys_mutex_attr_t attr;

	Cvar_RegisterVariable(&ps3_leaderboard);
	sysMutexAttrInitialize(attr);
	if (sysMutexCreate(&queue_lock, &attr) == 0)
		queue_lock_made = 1;
}

// the main loop, once the game's settings are read: what didn't send last time goes now
void PS3_Scores_Retry(void)
{
	static int tried;
	if (tried || realtime < 15 || !NZP_SCORE_KEY[0] || !ps3_leaderboard.value)
		return;		// (15 s in: config.cfg has been read, the menu is up)
	tried = 1;
	Kick();
}

// Only classic games from round 1 count. Noted as each game starts
// (SV_SpawnServer, SV_RestartServer), so changing a setting mid-game doesn't count.
static int game_ranked;
void PS3_Scores_GameStart(void)
{
	extern cvar_t sv_gamemode, sv_startround;
	game_ranked = (int)sv_gamemode.value == GAMEMODE_CLASSIC && (int)sv_startround.value <= 1;
}

// cl_parse.c, at svc_intermission (seat 0): the game is over
void PS3_Scores_GameOver(void)
{
	cJSON *o;
	char nonce_raw[16], *line, build[64], *nl;
	int kills = 0, i;

	result_text[0] = 0;
	if (!sv.active || cls.demoplayback || !ps3_leaderboard.value)
		return;
	if (!game_ranked) {
		PS3_Log("scores: not sent (only classic games from round 1 count)");
		snprintf(result_text, sizeof(result_text), "Not on the leaderboard: classic mode from round 1 only");
		result_seq++;
		return;
	}
	if (!NZP_SCORE_KEY[0]) {
		PS3_Log("scores: this build has no leaderboard key");
		return;
	}
	for (i = 0; i < cl.maxclients && cl.scores; i++)
		kills += cl.scores[i].kills;

	sysGetRandomNumber(nonce_raw, sizeof(nonce_raw));
	Hex((unsigned char *)nonce_raw, 16, last_nonce);
	snprintf(build, sizeof(build), "%s", game_build_date ? game_build_date : "");
	if ((nl = strpbrk(build, "\r\n"))) *nl = 0;

	o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "game", "nzp");
	{
		char map[64];
		snprintf(map, sizeof(map), "%s", sv.name);
		for (char *c = map; *c; c++) *c = tolower((unsigned char)*c);
		cJSON_AddStringToObject(o, "map", map);
	}
	cJSON_AddNumberToObject(o, "value", (int)pr_global_struct->rounds);
	cJSON_AddNumberToObject(o, "value2", kills);
	cJSON_AddNumberToObject(o, "seconds", (int)sv.time);
	cJSON_AddNumberToObject(o, "players", cl_numseats > 1 ? cl_numseats : 1);
	cJSON_AddStringToObject(o, "name", cl_name.string);
	cJSON_AddStringToObject(o, "build", build);
	cJSON_AddNumberToObject(o, "ts", (double)time(NULL));
	cJSON_AddStringToObject(o, "nonce", last_nonce);
	line = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (!line)
		return;
	PS3_Log("scores: game over on %s, round %d, %d kills: queued", sv.name, (int)pr_global_struct->rounds, kills);
	snprintf(result_text, sizeof(result_text), "Sending your score to ps3.world...");
	result_seq++;
	QueueAppend(line);
	free(line);
	Kick();
}

// shown on the game over screen
const char *PS3_Scores_Result(void)
{
	return result_text;
}
