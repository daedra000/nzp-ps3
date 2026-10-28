/*
 * ps3_local.h -- shared between the PS3 platform files.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */
#pragma once

// The PKG's own folder: game data (nzp/), config, saves and the log.
#ifndef PS3_APP_DIR
#define PS3_APP_DIR "/dev_hdd0/game/NZPP00001/USRDIR"
#endif

extern volatile int ps3_quit_requested;

// in_ps3.c
void PS3_Input_Poll(void);
void PS3_Input_SetXMBOpen(qboolean open);

// snd_ps3.c / music_ps3.c: music is mixed into the same audio port as the effects
void PS3_Music_Mix(float *out, int frames);   // adds frames of 48 kHz stereo

// gl_vidps3.c
void VID_Shutdown(void);
qboolean SNDDMA_Init(void);
void SNDDMA_Shutdown(void);
