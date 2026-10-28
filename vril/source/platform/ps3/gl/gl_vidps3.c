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
// gl_vidps3.c -- Vril's GL video driver on the RSX.
//
// ps3_video.c (from TyrQuakeCell / Quake2PS3) owns the RSX: init, the display buffers,
// flips and the final pass to the TV. The game draws into a render target the size of
// the TV mode (720p, or what the TV is set to) through the GL layer in rsx/, and each
// GL_EndRendering hands the frame to PS3_Video_Present.

#include "../../../nzportable_def.h"
#include "../ps3_platform.h"
#include "../ps3_local.h"
#include "../ps3_video.h"
#include "../rsx/ps3gl.h"

unsigned d_8to24table[256];
unsigned char d_15to8table[65536];
int texture_mode = GL_LINEAR;
double gldepthmin, gldepthmax;
cvar_t gl_ztrick = {"gl_ztrick", "0"};
qboolean isPermedia = true;

static qboolean vid_initialized;

void GL_Init(void)
{
	glClearDepth(1.0);
	glClearColor(16.0f / 255.0f, 32.0f / 255.0f, 64.0f / 255.0f, 1.0f);
	glCullFace(GL_FRONT);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.666f);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glShadeModel(GL_FLAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glClear(GL_COLOR_BUFFER_BIT);
}

void GL_BeginRendering(int *x, int *y, int *width, int *height)
{
	// waits for a free render target and makes it the RSX's surface
	PS3GL_BeginFrame();
	*x = *y = 0;
	*width = vid.width;
	*height = vid.height;
}

void GL_EndRendering(void)
{
	static int frames;

	Draw_Flush();
	PS3GL_EndFrame();
	PS3_Video_Present();
	if (frames < 3)
		PS3_Log("GL_EndRendering: frame %d presented", frames);
	frames++;
}

// Entries hold R, G, B, A in memory order (what the texture uploads read as
// GL_RGBA bytes); the PPU is big-endian, so R is the top byte of the value.
void VID_SetPalette(unsigned char *palette)
{
	unsigned i;
	for (i = 0; i < 256; ++i) {
		byte *c = (byte *)&d_8to24table[i];
		c[0] = palette[i * 3];
		c[1] = palette[i * 3 + 1];
		c[2] = palette[i * 3 + 2];
		c[3] = i == 255 ? 0 : 255;
	}
}

void VID_ShiftPalette(unsigned char *palette) { (void)palette; }

// Same curve as the SDL build (default 0.7, -gamma on the command line).
static void Check_Gamma(unsigned char *pal)
{
	unsigned char adjusted[768];
	int i, parameter = COM_CheckParm("-gamma");
	float vid_gamma = parameter && parameter + 1 < com_argc ? Q_atof(com_argv[parameter + 1]) : 0.7f;

	for (i = 0; i < 768; ++i) {
		float value = powf((pal[i] + 1) / 256.0f, vid_gamma) * 255.0f + 0.5f;
		adjusted[i] = (unsigned char)(value < 0 ? 0 : value > 255 ? 255 : value);
	}
	memcpy(pal, adjusted, sizeof(adjusted));
}

void VID_Init(unsigned char *palette)
{
	int width, height;

	PS3_Log("VID_Init");
	if (PS3_Video_Init() != 0)
		Sys_Error("PS3 video: RSX init failed (see nzp_log.txt)");

	// The render target is the TV mode: 1280x720 on an HD TV.
	width = PS3_Video_Width();
	height = PS3_Video_Height();
	if (PS3_Video_SetRenderTarget(width, height) != 0) {
		PS3_Log("VID: no %dx%d render target, trying 640x480", width, height);
		width = 640;
		height = 480;
		if (PS3_Video_SetRenderTarget(width, height) != 0)
			Sys_Error("PS3 video: no render target");
	}
	if (PS3GL_Init(width, height) != 0)
		Sys_Error("PS3 video: the GL layer failed to start (see nzp_log.txt)");
	PS3_Video_SetFit(100);
	PS3_Video_SetFilter(1);
	PS3_Video_SetColor(1.0f, 1.0f);

	Cvar_RegisterVariable(&gl_ztrick);
	vid.maxwarpwidth = vid.width = width;
	vid.maxwarpheight = vid.height = height;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
	vid.conwidth = 640;
	vid.conheight = 480;
	vid.aspect = ((float)vid.height / vid.width) * (320.0f / 240.0f);
	vid.scale = vid.height / STD_UI_HEIGHT;
	if (vid.scale < 1) vid.scale = 1;
	vid.numpages = 3;

	GL_Init();
	Check_Gamma(palette);
	VID_SetPalette(palette);
	vid_initialized = true;
	Con_SafePrintf("PS3 RSX video %dx%d initialized.\n", width, height);
	PS3_Log("VID_Init: %dx%d", width, height);
}

void VID_Shutdown(void)
{
	if (!vid_initialized)
		return;
	vid_initialized = false;
	PS3_Video_Shutdown();
}
