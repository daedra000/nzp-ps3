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

/* sys_ps3.c: one line in USRDIR/nzp_log.txt */
void PS3_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
