/*
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
// ps3_crash.c -- PPU exception handler, as thug-ps3's (and reVC-PS3's) on hardware.
//
// Without it a crash just ends the log and the console goes back to the XMB. liblv2dbg
// calls the handler on its own thread when a PPU thread faults; the faulting thread is
// stopped, so its registers and stack can still be read. It logs pc, DAR, lr, the
// registers, a backtrace and the last console commands, then exits.
//
// The addresses are resolved with ppu-addr2line against the same commit's ELF: run the
// "PS3" workflow by hand with the addresses as its input.

#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_local.h"

#include <sys/dbg.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <unistd.h>
#include <sysmodule/sysmodule.h>

#define CMD_TRAIL 16

static char cmd_trail[CMD_TRAIL][96];
static unsigned cmd_trail_next;
static uintptr_t game_stack_top;

void PS3_Crash_SetStackTop(void *top) { game_stack_top = (uintptr_t)top; }

/*
===============================================================================

STAGES AND WATCHDOG

A freeze (an endless loop or a deadlock) writes nothing and the crash handler
never runs. The game marks what it is doing (PS3_Stage: console commands, the
parts of Host_Frame, model/sound/map loads); a watchdog thread at a higher
priority than the game wakes every second and, when no stage has changed for
WATCHDOG_SECS, writes the stuck stage and the ones before it to the log.

===============================================================================
*/

#define STAGE_TRAIL    24
#define WATCHDOG_SECS  6

static char stage_trail[STAGE_TRAIL][96];
static double stage_time[STAGE_TRAIL];
static volatile unsigned stage_next;
static volatile unsigned stage_progress;

void PS3_Stage(const char *what)
{
	unsigned n = stage_next;
	char *dst = stage_trail[n % STAGE_TRAIL];

	strncpy(dst, what, sizeof(stage_trail[0]) - 1);
	dst[sizeof(stage_trail[0]) - 1] = 0;
	stage_time[n % STAGE_TRAIL] = Sys_FloatTime();
	__asm__ volatile("lwsync" ::: "memory");
	stage_next = n + 1;
	stage_progress++;
}

// The main loop's beat: progress without a new stage name.
void PS3_Heartbeat(void) { stage_progress++; }

static void DumpStages(const char *why)
{
	unsigned i, last = stage_next;

	PS3_Log("[watchdog] %s; the last stages (oldest first):", why);
	for (i = last > STAGE_TRAIL ? last - STAGE_TRAIL : 0; i < last; i++)
		PS3_Log("[watchdog]   %8.2f  %s", stage_time[i % STAGE_TRAIL], stage_trail[i % STAGE_TRAIL]);
}

static void WatchdogThread(void *arg)
{
	unsigned seen = 0, still = 0;
	int reported = 0;

	(void)arg;
	for (;;) {
		unsigned now;

		usleep(1000 * 1000);
		now = stage_progress;
		if (now != seen) {
			if (reported)
				PS3_Log("[watchdog] moving again after %u s", still);
			seen = now;
			still = 0;
			reported = 0;
			continue;
		}
		still++;
		if (still == WATCHDOG_SECS && reported < 1) {
			char why[64];
			snprintf(why, sizeof(why), "no progress for %d s (game time %.2f)", WATCHDOG_SECS, Sys_FloatTime());
			DumpStages(why);
			reported = 1;
		}
	}
}

void PS3_Watchdog_Start(void)
{
	sys_ppu_thread_t id;
	// lv2: a lower number is a higher priority (the game runs at 1000)
	s32 rc = sysThreadCreate(&id, WatchdogThread, NULL, 900, 64 * 1024, 0, (char *)"nzp_watchdog");
	if (rc != 0)
		PS3_Log("watchdog not started (%d)", (int)rc);
}

// Called from Cmd_ExecuteString: the commands that ran just before a crash.
void PS3_Crash_NoteCommand(const char *text)
{
	char *dst = cmd_trail[cmd_trail_next % CMD_TRAIL];
	char stage[110];

	strncpy(dst, text, sizeof(cmd_trail[0]) - 1);
	dst[sizeof(cmd_trail[0]) - 1] = 0;
	cmd_trail_next++;
	snprintf(stage, sizeof(stage), "cmd: %s", dst);
	PS3_Stage(stage);
}

static void PS3_CrashHandler(u64 cause, sys_ppu_thread_t tid, u64 dar)
{
	static sys_dbg_ppu_thread_context_t ctx;   // big: not on the handler's stack
	unsigned i;
	s32 ret;

	PS3_Log("[crash] PPU exception: cause 0x%llx, thread 0x%llx, DAR 0x%08llx",
		(unsigned long long)cause, (unsigned long long)tid, (unsigned long long)dar);

	memset(&ctx, 0, sizeof(ctx));
	ret = sysDbgReadPPUThreadContext(tid, &ctx);
	if (ret != 0) {
		PS3_Log("[crash] (registers unreadable: 0x%08x)", (unsigned)ret);
	} else {
		int r;
		u64 sp, top;

		PS3_Log("[crash] pc 0x%08llx", (unsigned long long)ctx.pc);
		PS3_Log("[crash] lr 0x%08llx", (unsigned long long)ctx.lr);
		PS3_Log("[crash] ctr %08llx cr %08x xer %08llx",
			(unsigned long long)ctx.ctr, (unsigned)ctx.cr, (unsigned long long)ctx.xer);
		for (r = 0; r < 32; r += 4)
			PS3_Log("[crash] r%-2d %016llx %016llx %016llx %016llx", r,
				(unsigned long long)ctx.gpr[r], (unsigned long long)ctx.gpr[r + 1],
				(unsigned long long)ctx.gpr[r + 2], (unsigned long long)ctx.gpr[r + 3]);

		// PPC64 ELFv1 frames: 0(sp) holds the caller's frame, 16(frame) the saved lr.
		sp = ctx.gpr[1];
		top = sp + 256 * 1024;
		if (game_stack_top > sp && game_stack_top - sp < 4 * 1024 * 1024)
			top = game_stack_top;
		for (r = 0; r < 48; r++) {
			u64 next, ra;
			if (sp == 0 || (sp & 15) || sp >= top)
				break;
			next = *(volatile u64 *)(uintptr_t)sp;
			if (next <= sp || next >= top)
				break;
			ra = *(volatile u64 *)(uintptr_t)(next + 16);
			PS3_Log("[crash] stack %2d 0x%08llx", r, (unsigned long long)ra);
			sp = next;
		}
	}

	PS3_Log("[crash] last commands (oldest first):");
	for (i = cmd_trail_next > CMD_TRAIL ? cmd_trail_next - CMD_TRAIL : 0; i < cmd_trail_next; i++)
		PS3_Log("[crash]   %s", cmd_trail[i % CMD_TRAIL]);
	PS3_Log("[crash] exiting");
	sysProcessExit(1);
}

void PS3_Crash_Install(void)
{
	s32 ret = sysModuleLoad(SYSMODULE_LV2DBG);
	if (ret != 0) {
		PS3_Log("crash handler not available (liblv2dbg: 0x%08x)", (unsigned)ret);
		return;
	}
	ret = sysDbgSetStacksizePPUExceptionHandler(64 * 1024);
	if (ret != 0)
		PS3_Log("crash handler: SetStacksize 0x%08x", (unsigned)ret);
	ret = sysDbgInitializePPUExceptionHandler(100);
	if (ret != 0) {
		PS3_Log("crash handler not available (Initialize 0x%08x)", (unsigned)ret);
		return;
	}
	// Called through a 32-bit function descriptor, 16 bytes into the 64-bit one.
	ret = sysDbgRegisterPPUExceptionHandler(
		(dbg_exception_handler_t)((uintptr_t)(void *)PS3_CrashHandler + 16), 0);
	if (ret != 0)
		PS3_Log("crash handler not available (Register 0x%08x)", (unsigned)ret);
	else
		PS3_Log("crash handler ready");
}
