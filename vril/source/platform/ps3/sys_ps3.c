/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2026 NZ:P PS3 port contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sys_ps3.c -- PS3 (PSL1GHT) system driver: files, time, log, the XMB, main().
//
// The structure follows TyrQuakeCell's sys_ps3.c (a Quake 1 PS3 port that runs on
// real hardware): the game runs on a worker thread with a big stack, the XMB's
// "Quit Game" goes through Host_Shutdown, and a log file in USRDIR is the only
// window into what happened on the console.

#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_local.h"
#include "_build_info.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lv2/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>
#include <sysmodule/sysmodule.h>

#define MAX_HANDLES 32

// Quake's hunk: level data, models, sounds. The Vita runs NZ:P in 64 MB; the PS3 has
// ~213 MB of user memory, and textures live in RSX memory, not here.
#define PS3_HUNK_MB 96

// The default EBOOT main thread has a ~128 KB stack, too small for the renderer's
// local arrays: everything runs on this thread instead.
#define PS3_GAME_STACK (2 * 1024 * 1024)
#define PS3_GAME_PRIO  1000

qboolean isDedicated;
volatile int ps3_quit_requested;

static FILE *sys_handles[MAX_HANDLES];

/*
===============================================================================

LOG

===============================================================================
*/

#define PS3_LOG_PATH     PS3_APP_DIR "/nzp_log.txt"
#define PS3_LOG_OLD_PATH PS3_APP_DIR "/nzp_log.old.txt"

// Capped per run so a chatty frame can't fill the HDD.
#define PS3_LOG_MAX (512 * 1024)

// Until the main loop starts every line goes straight to the file (opened,
// written, closed: a crash right after still leaves it on disk). From then on
// a line only goes into memory and a low-priority thread writes it a moment
// later: an HDD write can take 2 s (the stutter every 30 s in Nacht was the
// [alive] line), and the game must not wait for it.
#define LOG_BUF_SIZE (64 * 1024)

static long ps3_log_written;
static char log_buf[LOG_BUF_SIZE];
static int log_buf_len;
static int log_dropped;
static volatile int log_buf_lock, log_file_lock;
static volatile int log_async;

static void SpinLock(volatile int *lock)
{
	while (!__sync_bool_compare_and_swap(lock, 0, 1))
		usleep(50);
	__asm__ volatile("isync" ::: "memory");
}

static void SpinUnlock(volatile int *lock)
{
	__asm__ volatile("lwsync" ::: "memory");
	*lock = 0;
}

static void LogWriteFile(const char *text, int n)
{
	FILE *f;

	if (n <= 0 || ps3_log_written >= PS3_LOG_MAX)
		return;
	f = fopen(PS3_LOG_PATH, "a");
	if (!f)
		return;
	if (ps3_log_written + n >= PS3_LOG_MAX - 64) {
		fprintf(f, "(log full: nothing more is written this run)\n");
		ps3_log_written = PS3_LOG_MAX;
	} else {
		fwrite(text, 1, n, f);
		ps3_log_written += n;
	}
	fclose(f);
}

// Writes out what is waiting in memory. Any thread; also called before exit.
void PS3_Log_Flush(void)
{
	static char out[LOG_BUF_SIZE + 64];
	int n, dropped;

	SpinLock(&log_file_lock);
	SpinLock(&log_buf_lock);
	n = log_buf_len;
	memcpy(out, log_buf, n);
	log_buf_len = 0;
	dropped = log_dropped;
	log_dropped = 0;
	SpinUnlock(&log_buf_lock);
	if (dropped)
		n += snprintf(out + n, 64, "(%d log lines dropped: buffer full)\n", dropped);
	LogWriteFile(out, n);
	SpinUnlock(&log_file_lock);
}

static void LogAppend(const char *text, int n)
{
	if (n <= 0)
		return;
	if (!log_async) {
		SpinLock(&log_file_lock);
		LogWriteFile(text, n);
		SpinUnlock(&log_file_lock);
		return;
	}
	SpinLock(&log_buf_lock);
	if (log_buf_len + n <= LOG_BUF_SIZE) {
		memcpy(log_buf + log_buf_len, text, n);
		log_buf_len += n;
	} else {
		log_dropped++;
	}
	SpinUnlock(&log_buf_lock);
}

static void LogWriterThread(void *arg)
{
	(void)arg;
	for (;;) {
		usleep(250 * 1000);
		if (log_buf_len || log_dropped)
			PS3_Log_Flush();
	}
}

static void PS3_Log_StartAsync(void)
{
	sys_ppu_thread_t id;
	// below the game (1000): it only runs when the game waits
	if (sysThreadCreate(&id, LogWriterThread, NULL, 1500, 32 * 1024, 0, (char *)"nzp_log") == 0)
		log_async = 1;
	else
		PS3_Log("log writer thread not started: the log stays synchronous");
}

void PS3_Log(const char *fmt, ...)
{
	char line[1024];
	va_list argptr;
	int n;

	va_start(argptr, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, argptr);
	va_end(argptr);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	LogAppend(line, n);
}

// The engine's console output, as it is printed (no newline added).
static void PS3_LogRaw(const char *text)
{
	LogAppend(text, (int)strlen(text));
}

void Sys_DebugLog(char *file, char *fmt, ...) { (void)file; (void)fmt; }

/*
===============================================================================

FILE IO

===============================================================================
*/

static int Sys_FindHandle(void)
{
	int i;
	for (i = 1; i < MAX_HANDLES; ++i)
		if (!sys_handles[i]) return i;
	Sys_Error("out of handles");
	return -1;
}

static int Sys_FileLength(FILE *file)
{
	long position = ftell(file);
	long length;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, position, SEEK_SET);
	return (int)length;
}

int Sys_FileOpenRead(char *path, int *handle)
{
	FILE *file = fopen(path, "rb");
	int index;
	if (!file) { *handle = -1; return -1; }
	index = Sys_FindHandle();
	sys_handles[index] = file;
	*handle = index;
	return Sys_FileLength(file);
}

int Sys_FileOpenWrite(char *path)
{
	FILE *file = fopen(path, "wb");
	int index;
	if (!file) Sys_Error("Error opening %s: %s", path, strerror(errno));
	index = Sys_FindHandle();
	sys_handles[index] = file;
	return index;
}

void Sys_FileClose(int handle) { fclose(sys_handles[handle]); sys_handles[handle] = NULL; }
void Sys_FileSeek(int handle, int position) { fseek(sys_handles[handle], position, SEEK_SET); }
int Sys_FileRead(int handle, void *dest, int count) { return (int)fread(dest, 1, count, sys_handles[handle]); }
int Sys_FileWrite(int handle, void *data, int count) { return (int)fwrite(data, 1, count, sys_handles[handle]); }

// Only "does it exist" matters to the engine (-1 = no). Not stat()'s st_mtime:
// TyrQuakeCell (on hardware) also opens the file instead, and a modification time
// that happens to read as -1 would make a file that is there count as missing.
int Sys_FileTime(char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	fclose(f);
	return 1;
}

void Sys_mkdir(char *path) { mkdir(path, 0777); }

void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length) { (void)startaddr; (void)length; }

/*
===============================================================================

SYSTEM

===============================================================================
*/

void Sys_PrintSystemInfo(void) { Con_Printf("Vril Engine PS3 (PSL1GHT, RSX)\n"); }

void Sys_Printf(char *fmt, ...)
{
	char text[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	fputs(text, stdout);
	PS3_LogRaw(text);
}

// Reached from Sys_Error (system.c) with the formatted report.
void Sys_SystemError(char *error)
{
	static int in_error;

	PS3_Log("=== Sys_Error ===");
	PS3_Log("%s", error);
	PS3_Log_Flush();
	printf("Vril Engine: %s\n", error);

	// Let go of the audio port and the RSX before exit(): leaving with the RSX's
	// flip/vblank handlers pointing into this process takes the console down.
	// Not Host_Shutdown (config.cfg could be half initialised). Only once, in case
	// one of these is what failed.
	if (!in_error++) {
		SNDDMA_Shutdown();
		VID_Shutdown();
	}
	exit(1);
}

void Sys_Quit(void)
{
	// Host_Quit_f -> here: the main loop below sees it and shuts down cleanly.
	ps3_quit_requested = 1;
}

double Sys_FloatTime(void)
{
	static uint64_t start;
	uint64_t now = sysGetSystemTime();   // microseconds
	if (!start) start = now;
	return (double)(now - start) / 1000000.0;
}

char *Sys_ConsoleInput(void) { return NULL; }
void Sys_Sleep(void) { usleep(1000); }
void Sys_HighFPPrecision(void) {}
void Sys_LowFPPrecision(void) {}
void Sys_SetFPCW(void) {}

void Sys_CaptureScreenshot(void)
{
	Con_Printf("Screenshots are not supported on PS3.\n");
}

static volatile int ps3_xmb_pause;

/* Settings to the HDD now (APPLY, leaving a settings menu), not only on a clean
   quit: a crash or the power button would lose them. The log says so only
   when the file changed. */
extern void Host_WriteConfiguration(void);

void PS3_SaveSettings(void)
{
	static char last[16384];
	char path[MAX_OSPATH];
	FILE *f;
	long n;
	static char now[16384];

	Host_WriteConfiguration();
	snprintf(path, sizeof(path), "%s/config.cfg", com_gamedir);
	f = fopen(path, "rb");
	if (!f) {
		PS3_Log("settings: couldn't write %s", path);
		return;
	}
	n = (long)fread(now, 1, sizeof(now) - 1, f);
	fclose(f);
	now[n > 0 ? n : 0] = 0;
	if (strcmp(now, last)) {
		PS3_Log("settings saved (%ld bytes)", n);
		memcpy(last, now, sizeof(last));
	}
}

void Sys_SendKeyEvents(void)
{
	// Delivers the XMB's events (PS button menu, "Quit Game") to the callback below.
	sysUtilCheckCallback();

	// PS button menu opened during a match: the game's own pause menu, as Start.
	if (ps3_xmb_pause) {
		ps3_xmb_pause = 0;
		if (key_dest == key_game && cls.state == ca_connected && cls.signon == SIGNONS) {
			extern void Menu_ToggleMenu_f(void);
			PS3_Log("XMB opened: game paused");
			Menu_ToggleMenu_f();
		}
	}
	PS3_Input_Poll();
}

static void PS3_SysutilCallback(u64 status, u64 param, void *userdata)
{
	(void)param;
	(void)userdata;
	switch (status) {
	case SYSUTIL_EXIT_GAME:
		PS3_Log("XMB: Quit Game");
		ps3_quit_requested = 1;
		break;
	case SYSUTIL_MENU_OPEN:
		PS3_Input_SetXMBOpen(true);
		ps3_xmb_pause = 1;
		break;
	case SYSUTIL_MENU_CLOSE:
		PS3_Input_SetXMBOpen(false);
		break;
	default:
		break;
	}
}

/*
===============================================================================

MAIN

===============================================================================
*/

// "Welcome, <name>" on the main menu and the in-game name: the signed-in
// PS3 user's name, unless the player has chosen one of their own. The name
// last taken from the PS3 is kept (ps3_autoname) so that, after switching
// PS3 user, the new user's name replaces it: config.cfg had saved the old one.
cvar_t ps3_autoname = {"ps3_autoname", "", true};

static void PS3_SetPlayerName(void)
{
	extern cvar_t cl_name;
	// As ioquake3-ps3: the local user name (0x131, 64 bytes, works offline),
	// then the PSN nickname (0x113, 128 bytes); the buffer sizes matter.
	char user[SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE + 1];
	s32 rc, rc_nick = 0;

	// a name the player typed is kept (with no ps3_autoname yet, the config
	// is from a build before it, whose name came from the PS3 too)
	if (Q_strcasecmp(cl_name.string, "player") && cl_name.string[0] &&
		ps3_autoname.string[0] && strcmp(cl_name.string, ps3_autoname.string))
		return;

	memset(user, 0, sizeof(user));
	rc = sysUtilGetSystemParamString(SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME, user,
		SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE);
	user[SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE - 1] = 0;
	if (rc != 0 || !user[0]) {
		memset(user, 0, sizeof(user));
		rc_nick = sysUtilGetSystemParamString(SYSUTIL_SYSTEMPARAM_ID_NICKNAME, user,
			SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE);
		user[SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE - 1] = 0;
	}
	if (!user[0] || (rc != 0 && rc_nick != 0)) {
		PS3_Log("PS3 user name not available (0x%08x, nickname 0x%08x)", (unsigned)rc, (unsigned)rc_nick);
		return;
	}
	Cvar_Set("_cl_name", user);
	Cvar_Set("ps3_autoname", user);
	PS3_Log("player name: %s (the PS3 user)", user);
}

static void PS3_GameThread(void *arg)
{
	static char *argv[16];
	int argc = 0;
	quakeparms_t parms;
	double oldtime;

	(void)arg;
	PS3_Crash_SetStackTop(&parms);

	if (sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, PS3_SysutilCallback, NULL) != 0)
		PS3_Log("WARNING: sysUtilRegisterCallback failed, the XMB's Quit Game won't be clean");

	memset(&parms, 0, sizeof(parms));
	parms.memsize = PS3_HUNK_MB * 1024 * 1024;
	parms.membase = malloc(parms.memsize);
	if (!parms.membase) {
		PS3_Log("FATAL: no memory for the %d MB hunk", PS3_HUNK_MB);
		sysThreadExit(1);
	}
	// Game data in USRDIR/nzp, the PKG's own folder (config and saves go there too).
	parms.basedir = PS3_APP_DIR;

	argv[argc++] = "nzportable";
	COM_InitArgv(argc, argv);
	parms.argc = com_argc;
	parms.argv = com_argv;

	PS3_Log("Host_Init (basedir %s, %d MB hunk)", parms.basedir, PS3_HUNK_MB);
	Host_Init(&parms);
	PS3_Log("Host_Init done, entering the main loop");

	PS3_Watchdog_Start();
	PS3_Log_StartAsync();
	oldtime = Sys_FloatTime();
	while (!ps3_quit_requested) {
		static double next_alive = 30.0;
		static int last_frames;
		int frames_before = host_framecount;
		double now = Sys_FloatTime();
		Host_Frame(now - oldtime);
		music_update();
		oldtime = now;
		PS3_Heartbeat();
		// The PS3 account's name instead of the default "player", once the
		// configs (run in the first frames) have set theirs.
		{
			static int name_set;
			if (host_framecount >= 10 && !name_set) {
				name_set = 1;
				PS3_SetPlayerName();
			}
		}
		// Host_Frame returns at once until a frame's time has gone by: don't
		// spin the PPU (the audio thread shares it) while waiting
		if (host_framecount == frames_before)
			usleep(500);
		// a sign of life every 30 s, with the frame rate
		if (now >= next_alive) {
			char phases_ms[256];
			int frames = host_framecount - last_frames;
			double audio = PS3_Audio_TakeBusyUs() / 1e6;
			PS3_Stage_Report(phases_ms, sizeof(phases_ms), frames);
			char audio_state[96];
			PS3_Audio_Status(audio_state, sizeof(audio_state));
			PS3_Log("[alive] %.0f s, frame %d, %.1f fps; ms per frame: %s; audio thread busy %.0f%%, %s",
				now, host_framecount, frames / 30.0, phases_ms, audio * 100.0 / 30.0, audio_state);
			last_frames = host_framecount;
			next_alive = now + 30.0;
		}
	}

	// Writes config.cfg, then lets go of the audio port and the RSX (VID_Shutdown):
	// exiting with the RSX's handlers still pointing into this process crashes the
	// whole console.
	PS3_Log("Shutting down");
	PS3_Log_Flush();
	Host_Shutdown();
	free(parms.membase);
	PS3_Log("Bye");
	PS3_Log_Flush();
	sysThreadExit(0);
}

int main(int argc, char **argv)
{
	sys_ppu_thread_t thread_id;
	u64 exit_code = 0;
	s32 rc;

	(void)argc;
	(void)argv;

	// A fresh log each run, the previous one kept.
	remove(PS3_LOG_OLD_PATH);
	rename(PS3_LOG_PATH, PS3_LOG_OLD_PATH);
	PS3_Log("=== Nazi Zombies: Portable, PS3 (%s), build %s %s ===", PS3_APP_DIR, GIT_HASH, BUILD_DATE);

	// From the main thread before any other (as thug-ps3 does): from the game
	// thread lv2 refused it (0x8001042c).
	PS3_Crash_Install();

	rc = sysThreadCreate(&thread_id, PS3_GameThread, NULL, PS3_GAME_PRIO, PS3_GAME_STACK,
		THREAD_JOINABLE, (char *)"nzp_game");
	if (rc != 0) {
		PS3_Log("FATAL: sysThreadCreate failed: %d", (int)rc);
		return 1;
	}
	sysThreadJoin(thread_id, &exit_code);
	sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	return (int)exit_code;
}
