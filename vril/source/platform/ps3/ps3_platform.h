/*
 * ps3_platform.h -- what the PS3 layer files (ps3_video.c, rsx/ps3gl_*.c, taken
 * from TyrQuakeCell / Quake2PS3) need from the rest of the Vril PS3 platform.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */
#ifndef PS3_PLATFORM_H
#define PS3_PLATFORM_H

/* ps3_crash.c: PPU exception handler, logs pc/registers/backtrace/last commands */
void PS3_Crash_Install(void);
void PS3_Crash_SetStackTop(void *top);
void PS3_Crash_NoteCommand(const char *text);
/* what the game is doing, for the watchdog (a freeze names its stage) */
void PS3_Stage(const char *what);
void PS3_Heartbeat(void);
void PS3_Stage_Report(char *out, int size, int frames);
/* snd_ps3.c: microseconds the audio thread spent mixing since the last call */
unsigned long long PS3_Audio_TakeBusyUs(void);
void PS3_Audio_Status(char *out, int size);
void PS3_Watchdog_Start(void);

/* sys_ps3.c: write out what the log holds in memory (before an exit, after a dump) */
void PS3_Log_Flush(void);

/* sys_ps3.c: one line in USRDIR/nzp_log.txt */
void PS3_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
