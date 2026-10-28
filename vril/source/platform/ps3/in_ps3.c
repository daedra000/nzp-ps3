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
// in_ps3.c -- DualShock 3 / DualShock 4 (PSL1GHT ioPad): buttons, sticks, rumble.
//
// The buttons become Vril's generic gamepad keys (K_BOTTOMFACE = Cross, ...), the
// names the PC data's config.cfg binds, and the sticks feed input.c's IN_Move.
// Button bits and stick indices of padData.button[] as in TyrQuakeCell's in_ps3.c
// (hardware-tested); a read with len == 0 means "no new data" and keeps the last
// state (IoQuake3-PS3: treating it as "nothing held" freezes menus).

#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_local.h"
#include "../../menu/menu_defs.h"

#include <io/pad.h>

#define MAX_PADS 7

#define BTN_L2       0x0001
#define BTN_R2       0x0002
#define BTN_L1       0x0004
#define BTN_R1       0x0008
#define BTN_TRIANGLE 0x0010
#define BTN_CIRCLE   0x0020
#define BTN_CROSS    0x0040
#define BTN_SQUARE   0x0080
#define BTN_SELECT   0x0100
#define BTN_L3       0x0200
#define BTN_R3       0x0400
#define BTN_START    0x0800
#define BTN_UP       0x1000
#define BTN_RIGHT    0x2000
#define BTN_DOWN     0x4000
#define BTN_LEFT     0x8000

static const struct { unsigned bit; int key; } ps3_buttons[] = {
	{ BTN_CROSS,    K_BOTTOMFACE },
	{ BTN_CIRCLE,   K_RIGHTFACE },
	{ BTN_SQUARE,   K_LEFTFACE },
	{ BTN_TRIANGLE, K_TOPFACE },
	{ BTN_L1,       K_LTRIGGER },
	{ BTN_R1,       K_RTRIGGER },
	{ BTN_L2,       K_ZLTRIGGER },
	{ BTN_R2,       K_ZRTRIGGER },
	{ BTN_L3,       K_LTHUMB },
	{ BTN_R3,       K_RTHUMB },
	{ BTN_START,    K_START },
	{ BTN_SELECT,   K_SELECT },
	{ BTN_UP,       K_DPAD_UP },
	{ BTN_DOWN,     K_DPAD_DOWN },
	{ BTN_LEFT,     K_DPAD_LEFT },
	{ BTN_RIGHT,    K_DPAD_RIGHT },
};

static int      pad_port = -1;           // the pad being read, -1 = none
static unsigned pad_buttons;             // held this frame
static byte     pad_sticks[4] = { 128, 128, 128, 128 };   // RX RY LX LY
static qboolean xmb_open;
static double   rumble_stop_time;
static qboolean rumble_on;

void PS3_Input_SetXMBOpen(qboolean open)
{
	unsigned i;

	xmb_open = open;
	// Everything is let go while the XMB has the pad.
	if (open) {
		for (i = 0; i < sizeof(ps3_buttons) / sizeof(ps3_buttons[0]); i++)
			if (pad_buttons & ps3_buttons[i].bit)
				Key_Event(ps3_buttons[i].key, false);
		pad_buttons = 0;
		pad_sticks[0] = pad_sticks[1] = pad_sticks[2] = pad_sticks[3] = 128;
	}
}

static void PS3_SetRumble(byte small, byte large)
{
	padActParam act;

	if (pad_port < 0)
		return;
	memset(&act, 0, sizeof(act));
	act.small_motor = small ? 1 : 0;   // the DS3's small motor is on/off only
	act.large_motor = large;
	ioPadSetActDirect(pad_port, &act);
	rumble_on = small || large;
}

void PS3_Input_Poll(void)
{
	padInfo info;
	padData data;
	unsigned now, pressed, released, i;
	int port;

	if (rumble_on && Sys_FloatTime() >= rumble_stop_time)
		PS3_SetRumble(0, 0);

	if (xmb_open)
		return;

	if (ioPadGetInfo(&info) != 0)
		return;

	// The first connected pad drives the game.
	port = -1;
	for (i = 0; i < MAX_PADS; i++) {
		if (info.status[i]) {
			port = (int)i;
			break;
		}
	}
	if (port != pad_port) {
		pad_port = port;
		PS3_Log("in_ps3: reading pad %d", port);
	}
	if (port < 0) {
		now = 0;
	} else {
		memset(&data, 0, sizeof(data));
		if (ioPadGetData(port, &data) != 0 || data.len <= 0)
			return;   // no new data: the last state stands
		now = ((unsigned)data.button[2] << 8) | (data.button[3] & 0xff);
		pad_sticks[0] = (byte)data.button[4];
		pad_sticks[1] = (byte)data.button[5];
		pad_sticks[2] = (byte)data.button[6];
		pad_sticks[3] = (byte)data.button[7];
	}

	pressed = now & ~pad_buttons;
	released = pad_buttons & ~now;
	pad_buttons = now;
	if (pressed) {
		IN_SetActiveDevice(IN_DEVICE_GAMEPAD);
		Menu_SetInputDevice(IN_DEVICE_GAMEPAD);
	}
	for (i = 0; i < sizeof(ps3_buttons) / sizeof(ps3_buttons[0]); i++) {
		if (pressed & ps3_buttons[i].bit)
			Key_Event(ps3_buttons[i].key, true);
		else if (released & ps3_buttons[i].bit)
			Key_Event(ps3_buttons[i].key, false);
	}
}

qboolean IN_PlatformHasMouse(void) { return false; }
qboolean IN_PlatformHasGamepad(void) { return true; }
void IN_SetMouseToRelative(bool relative) { (void)relative; }
void IN_PlatformClearPendingInput(void) {}
void IN_PlatformMouseMove(usercmd_t *cmd) { (void)cmd; }

void IN_PlatformInit(void)
{
	s32 rc = ioPadInit(MAX_PADS);
	PS3_Log("in_ps3: ioPadInit = %d", (int)rc);
}

void IN_PlatformShutdown(void)
{
	PS3_SetRumble(0, 0);
	ioPadEnd();
}

void IN_PlatformCommands(void) {}
void IN_PlatformMove(usercmd_t *cmd) { (void)cmd; }

static float PS3_StickAxis(byte v)
{
	float f = ((float)v - 127.5f) / 127.5f;
	return f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
}

// Both axes in [-1, 1], positive Y up (input.h).
void IN_GetAnalogStick(in_analog_stick_id_t stick, in_analog_stick_t *value)
{
	if (stick == IN_STICK_LEFT) {
		value->x = PS3_StickAxis(pad_sticks[2]);
		value->y = -PS3_StickAxis(pad_sticks[3]);
	} else {
		value->x = PS3_StickAxis(pad_sticks[0]);
		value->y = -PS3_StickAxis(pad_sticks[1]);
	}
}

void IN_PlatformRumble(unsigned short low_frequency, unsigned short high_frequency, unsigned int duration)
{
	PS3_SetRumble((byte)(high_frequency / 257), (byte)(low_frequency / 257));
	rumble_stop_time = Sys_FloatTime() + (double)duration / 1000.0;
}
