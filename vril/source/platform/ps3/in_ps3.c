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

// Slot 0 is the first connected pad (player 1); in split screen slot 1, the
// second connected pad, is player 2 (keys.c Key_EventSeat, cl_seats.c).
#define PAD_SLOTS 2
static int      pad_ports[PAD_SLOTS] = { -1, -1 };	// the pad each slot reads, -1 = none
static unsigned pad_held[PAD_SLOTS];					// held this frame
static byte     pad_stick[PAD_SLOTS][4] = { { 128, 128, 128, 128 }, { 128, 128, 128, 128 } };	// RX RY LX LY
static qboolean xmb_open;
static double   rumble_stop_time[PAD_SLOTS];
static qboolean rumble_on[PAD_SLOTS];

#define pad_port pad_ports[0]

void PS3_Input_RumbleSeat(int seat, unsigned short low_frequency, unsigned short high_frequency, unsigned int duration);

static void PS3_PadKey(int slot, int key, qboolean down)
{
	if (slot == 0)
		Key_Event(key, down);
	else
		Key_EventSeat(key, down, slot);
}

static void PS3_ReleaseSlot(int slot)
{
	unsigned i;
	for (i = 0; i < sizeof(ps3_buttons) / sizeof(ps3_buttons[0]); i++)
		if (pad_held[slot] & ps3_buttons[i].bit)
			PS3_PadKey(slot, ps3_buttons[i].key, false);
	pad_held[slot] = 0;
	pad_stick[slot][0] = pad_stick[slot][1] = pad_stick[slot][2] = pad_stick[slot][3] = 128;
}

void PS3_Input_SetXMBOpen(qboolean open)
{
	int s;

	xmb_open = open;
	// Everything is let go while the XMB has the pads.
	if (open)
		for (s = 0; s < PAD_SLOTS; s++)
			PS3_ReleaseSlot(s);
}

static void PS3_SetRumble(int slot, byte small, byte large)
{
	padActParam act;

	if (pad_ports[slot] < 0)
		return;
	memset(&act, 0, sizeof(act));
	act.small_motor = small ? 1 : 0;   // the DS3's small motor is on/off only
	act.large_motor = large;
	ioPadSetActDirect(pad_ports[slot], &act);
	rumble_on[slot] = small || large;
}

static void PS3_PollSlot(int slot, int port)
{
	padData data;
	unsigned now, pressed, released, i;

	if (port != pad_ports[slot]) {
		if (pad_ports[slot] >= 0 || port >= 0)
			PS3_Log("in_ps3: player %d on pad %d", slot + 1, port);
		PS3_ReleaseSlot(slot);
		pad_ports[slot] = port;
	}
	if (port < 0) {
		// and the sticks centred, or the last deflection keeps moving the player
		now = 0;
		pad_stick[slot][0] = pad_stick[slot][1] = pad_stick[slot][2] = pad_stick[slot][3] = 128;
	} else {
		memset(&data, 0, sizeof(data));
		if (ioPadGetData(port, &data) != 0 || data.len <= 0)
			return;   // no new data: the last state stands
		now = ((unsigned)data.button[2] << 8) | (data.button[3] & 0xff);
		pad_stick[slot][0] = (byte)data.button[4];
		pad_stick[slot][1] = (byte)data.button[5];
		pad_stick[slot][2] = (byte)data.button[6];
		pad_stick[slot][3] = (byte)data.button[7];
	}

	pressed = now & ~pad_held[slot];
	released = pad_held[slot] & ~now;
	pad_held[slot] = now;
	if (pressed) {
		IN_SetActiveDevice(IN_DEVICE_GAMEPAD);
		Menu_SetInputDevice(IN_DEVICE_GAMEPAD);
	}
	for (i = 0; i < sizeof(ps3_buttons) / sizeof(ps3_buttons[0]); i++) {
		if (pressed & ps3_buttons[i].bit)
			PS3_PadKey(slot, ps3_buttons[i].key, true);
		else if (released & ps3_buttons[i].bit)
			PS3_PadKey(slot, ps3_buttons[i].key, false);
	}
}

void PS3_Input_Poll(void)
{
	padInfo info;
	int ports[PAD_SLOTS] = { -1, -1 }, n = 0, s;
	unsigned i;

	for (s = 0; s < PAD_SLOTS; s++)
		if (rumble_on[s] && Sys_FloatTime() >= rumble_stop_time[s])
			PS3_SetRumble(s, 0, 0);

	if (xmb_open)
		return;

	if (ioPadGetInfo(&info) != 0)
		return;

	// The first connected pad is player 1's; the next, split screen's player 2.
	for (i = 0; i < MAX_PADS && n < PAD_SLOTS; i++)
		if (info.status[i])
			ports[n++] = (int)i;
	if (cl_numseats < 2)
		ports[1] = -1;

	for (s = 0; s < PAD_SLOTS; s++)
		PS3_PollSlot(s, ports[s]);
}

// controllers connected now (the menu won't start split screen with one)
int PS3_Input_PadCount(void)
{
	padInfo info;
	int i, n = 0;
	if (ioPadGetInfo(&info) != 0)
		return 0;
	for (i = 0; i < MAX_PADS; i++)
		if (info.status[i])
			n++;
	return n;
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
	PS3_SetRumble(0, 0, 0);
	PS3_SetRumble(1, 0, 0);
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
// The sticks of the seat whose move is being made (split screen: CL_SendCmd per seat).
void IN_GetAnalogStick(in_analog_stick_id_t stick, in_analog_stick_t *value)
{
	int slot = (cl_curseat > 0 && cl_curseat < PAD_SLOTS) ? cl_curseat : 0;
	byte *st = pad_stick[slot];

	if (stick == IN_STICK_LEFT) {
		value->x = PS3_StickAxis(st[2]);
		value->y = -PS3_StickAxis(st[3]);
	} else {
		value->x = PS3_StickAxis(st[0]);
		value->y = -PS3_StickAxis(st[1]);
	}
}

void IN_PlatformRumble(unsigned short low_frequency, unsigned short high_frequency, unsigned int duration)
{
	PS3_Input_RumbleSeat(0, low_frequency, high_frequency, duration);
}

// split screen: each player's pad shakes for their own hits
void PS3_Input_RumbleSeat(int seat, unsigned short low_frequency, unsigned short high_frequency, unsigned int duration)
{
	if (seat < 0 || seat >= PAD_SLOTS)
		return;
	PS3_SetRumble(seat, (byte)(high_frequency / 257), (byte)(low_frequency / 257));
	rumble_stop_time[seat] = Sys_FloatTime() + (double)duration / 1000.0;
}
