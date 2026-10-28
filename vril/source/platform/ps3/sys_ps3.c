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

// Every line is a trip to the HDD (opened, written, closed, so a crash right after
// still leaves it on disk): capped per run so a chatty frame can't slow the game.
#define PS3_LOG_MAX (512 * 1024)

static long ps3_log_written;

void PS3_Log(const char *fmt, ...)
{
	va_list argptr;
	FILE *f;
	int n;

	if (ps3_log_written >= PS3_LOG_MAX)
		return;
	f = fopen(PS3_LOG_PATH, "a");
	if (!f)
		return;
	if (ps3_log_written + 256 >= PS3_LOG_MAX) {
		fprintf(f, "(log full: nothing more is written this run)\n");
		fclose(f);
		ps3_log_written = PS3_LOG_MAX;
		return;
	}
	va_start(argptr, fmt);
	n = vfprintf(f, fmt, argptr);
	va_end(argptr);
	fprintf(f, "\n");
	fclose(f);
	ps3_log_written += (n > 0 ? n : 0) + 1;
}

// The engine's console output, as it is printed (no newline added).
static void PS3_LogRaw(const char *text)
{
	FILE *f;
	size_t n = strlen(text);

	if (!n || ps3_log_written + (long)n >= PS3_LOG_MAX - 256)
		return;
	f = fopen(PS3_LOG_PATH, "a");
	if (!f)
		return;
	fwrite(text, 1, n, f);
	fclose(f);
	ps3_log_written += (long)n;
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

void Sys_SendKeyEvents(void)
{
	// Delivers the XMB's events (PS button menu, "Quit Game") to the callback below.
	sysUtilCheckCallback();
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
	oldtime = Sys_FloatTime();
	while (!ps3_quit_requested) {
		static unsigned frame;
		double now = Sys_FloatTime();
		Host_Frame(now - oldtime);
		music_update();
		oldtime = now;
		PS3_Heartbeat();
		// a sign of life every ~30 s, and the frame rate it implies
		if (++frame % 1800 == 0)
			PS3_Log("[alive] frame %u, %.1f s", frame, now);
	}

	// Writes config.cfg, then lets go of the audio port and the RSX (VID_Shutdown):
	// exiting with the RSX's handlers still pointing into this process crashes the
	// whole console.
	PS3_Log("Shutting down");
	Host_Shutdown();
	free(parms.membase);
	PS3_Log("Bye");
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
