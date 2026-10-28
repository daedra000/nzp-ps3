/*
 * Copyright (C) 2026 the Quake2PS3 / TyrQuakeCell contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * =======================================================================
 *
 * PS3 display, Quake2PS3's ps3_video.c (back home: its blit and flip
 * sequence came from TyrQuakeCell's vid_ps3.c 1.x). Put together from the
 * versions proven on hardware:
 *
 *  - RSX/video init, framebuffers and flip handler from Doom64-PS3.
 *  - The scaled blit (rsxSetTransferScaleSurface, fixed function 2D unit,
 *    no shaders) and its flip sequence from TyrQuakeCell.
 *
 * Software renderer (vid_ps3_sw.c): the 8 bit image goes through the palette
 * straight into a 32 bit source image in RSX memory. There is one source
 * image per framebuffer, so the CPU never writes into an image the RSX
 * may still be reading: image N is written again only after the flip that
 * used it three frames ago has completed.
 *
 * OpenGL (vid_ps3_gl.c): the source images are ps3gl's render targets.
 *
 * Rules learned the hard way (handoff notes):
 *  - rsxInit twice leaks the previous context: init is idempotent.
 *  - Shutdown: gcmSetFlipHandler(NULL) FIRST, then free.
 *  - This file is built at -O1 -fno-inline (see ps3/Makefile): PSL1GHT's
 *    syscall stubs don't survive aggressive inlining.
 *  - Never read RSX memory from the CPU (very slow); only write it.
 *
 * =======================================================================
 */

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <lv2/systime.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>

#include "ps3_platform.h"
#include "ps3_video.h"
#include "rsx/ps3gl.h"
#include "ps3gl_shader_data.h"

#define FB_COUNT      3
#define FB_ALIGN      64
#define RSX_CB_SIZE   (1 * 1024 * 1024)     /* command buffer */
#define RSX_IO_SIZE   (32 * 1024 * 1024)    /* IO buffer (Doom64-PS3 value) */

static gcmContextData *ctx;
static void *io_buffer;
static int ready;

static u32 disp_w = 1280, disp_h = 720, disp_pitch;

static u32 *fb[FB_COUNT];
static u32 fb_offset[FB_COUNT];
static int cur;

static volatile u32 flip_queued, flip_completed;

static u32 *src[FB_COUNT];
static u32 src_offset[FB_COUNT];
static int src_w, src_h;
static u32 src_pitch;

/* OpenGL mode: the source images are the RSX's render targets (rows
   padded to 64 bytes) and share one depth/stencil buffer. */
static int gl_mode;
static u32 *depth;
static u32 depth_offset, depth_pitch;

static int clear_frames;     /* framebuffers that still need black borders */
static int perf_log;         /* fps and timings in the log every 10 s */
static int fit_percent = 90;
static int filter_linear = 1;

static volatile u32 vblank_count;
static u32 last_flip_vblank;
static int lock30;

/* profiling, reset every 10 seconds */
static u64 prof_copy_us, prof_wait_us, prof_lock_us;


/* ================================================================ */

static void
FlipHandler(const u32 head)
{
	(void)head;
	flip_completed++;
}

static void
VBlankHandler(const u32 head)
{
	(void)head;
	vblank_count++;
}

/* Blocks only while both framebuffers that are not being built are still
   in flight. Afterwards the one at 'cur' is free: nothing displays it and
   the RSX has finished the blit into it (and read its source image). */
static void
WaitFlips(void)
{
	int waited = 0;

	while ((int)(flip_queued - flip_completed) > FB_COUNT - 2)
	{
		usleep(100);

		if (++waited > 20000)
		{
			PS3_Log("[video] WARNING: flip fence timed out, force-sync");
			flip_completed = flip_queued;
			break;
		}
	}
}

/* Waits until the RSX has done everything queued so far. rsxFinish(ctx, v)
   waits for the RSX's reference register to become v: with the same v
   every time it returns at once from the second call on (the register
   already holds it), and memory still in use gets freed. A new value each
   time. */
/* ================================================================ */
/* Command buffer room                                               */
/* ================================================================ */

/* librsx moves to the next 32 KB piece of the command buffer from inside its
   functions, through an asm call that tells GCC nothing about memory or the
   registers the system's callback changes. Built with GCC 13 the function then
   carries on writing through the pointer it had before the call: the end of the
   command runs into the next piece, over the jump the RSX was to follow, and
   the RSX reads vertex program words as commands and hangs (the [fifo] dump:
   GET stuck 0x5c into a piece that starts with stray program words).

   So librsx must never need to: before each batch of commands there is always
   at least PS3_CMD_ROOM words left, making the move here, through a call that
   declares everything the callback may change. */
#define PS3_CMD_ROOM 2048   /* words; the largest single librsx call is ~100 */

static s32 __attribute__((noinline, noipa))
CallContextCallback(gcmContextData *c, u32 count)
{
	register u64 r3 __asm__("r3") = (u64)(uintptr_t)c;
	register u64 r4 __asm__("r4") = count;
	u32 opd = (u32)(u64)(uintptr_t)c->callback;

	__asm__ __volatile__(
		"stdu   1,-128(1)\n"
		"mr     31,2\n"
		"lwz    0,0(%2)\n"
		"lwz    2,4(%2)\n"
		"mtctr  0\n"
		"bctrl\n"
		"mr     2,31\n"
		"addi   1,1,128\n"
		: "+r"(r3), "+r"(r4)
		: "b"((u64)opd)
		: "r0", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r31",
		  "ctr", "lr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7",
		  "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9",
		  "fr10", "fr11", "fr12", "fr13", "memory");
	return (s32)r3;
}

void
PS3_Cmd_Reserve(void)
{
	static int logged;

	if (!ctx)
	{
		return;
	}

	if (ctx->current + PS3_CMD_ROOM <= ctx->end)
	{
		return;
	}

	if (CallContextCallback(ctx, PS3_CMD_ROOM) != 0 && !logged)
	{
		logged = 1;
		PS3_Log("[video] WARNING: command buffer callback failed");
	}

	if (logged < 2 && ctx->current + PS3_CMD_ROOM > ctx->end)
	{
		logged = 2;
		PS3_Log("[video] WARNING: still no command room after the callback (%p..%p)",
				(void *)ctx->current, (void *)ctx->end);
	}
}

/* After a fence timeout: where the RSX's command reader stopped (GET) against
   what has been handed to it (PUT), and the command words around GET. */
void
PS3_Video_DumpFifo(void)
{
	static int dumps;
	gcmControlRegister volatile *ctrl;
	u32 get, put;
	void *addr = NULL;
	int i;

	if (!ctx || dumps++ >= 2)
	{
		return;
	}

	ctrl = gcmGetControlRegister();
	get = ctrl->get;
	put = ctrl->put;
	PS3_Log("[fifo] put 0x%08x get 0x%08x ref 0x%08x; buffer %p..%p, cpu at %p",
			(unsigned)put, (unsigned)get, (unsigned)ctrl->ref,
			(void *)ctx->begin, (void *)ctx->end, (void *)ctx->current);

	if (gcmIoOffsetToAddress(get, &addr) != 0 || !addr)
	{
		PS3_Log("[fifo] GET is not an IO offset");
		return;
	}

	for (i = -32; i < 16; i += 4)
	{
		u32 *w = (u32 *)addr + i;
		PS3_Log("[fifo] %+4d %08x %08x %08x %08x%s", i * 4,
				(unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
				i == 0 ? "  <- GET" : "");
	}
}

void
PS3_Video_Finish(void)
{
	static u32 ref = 0x1000;

	if (!ctx)
	{
		return;
	}

	gcmControlRegister volatile *ctrl;
	int waited = 0;

	ref++;

	if (ref == 0 || ref == 0xffffffffu)
	{
		ref = 0x1000;
	}

	/* rsxFinish(ctx, ref), with a time limit: on the way out after an
	   error the RSX may be the thing that's stuck. */
	PS3_Cmd_Reserve();
	rsxSetReferenceCommand(ctx, ref);
	rsxFlushBuffer(ctx);
	ctrl = gcmGetControlRegister();

	while (ctrl->ref != ref)
	{
		usleep(30);

		if (++waited > 100000)   /* ~3 s */
		{
			PS3_Log("[video] WARNING: the RSX didn't finish in time (ref %u)", (unsigned)ref);
			break;
		}
	}
}

int
PS3_Video_Ready(void)
{
	return ready;
}

int
PS3_Video_Width(void)
{
	return (int)disp_w;
}

int
PS3_Video_Height(void)
{
	return (int)disp_h;
}

int
PS3_Video_Init(void)
{
	s32 ret;
	s32 vid_res = VIDEO_RESOLUTION_720;
	u8 vid_aspect = VIDEO_ASPECT_16_9;
	videoResolution res;
	videoConfiguration vconfig;
	videoState state;
	int i, waited;

	if (ready)
	{
		return 0;
	}

	PS3_Log("[video] init");

	io_buffer = memalign(1024 * 1024, RSX_IO_SIZE);

	if (!io_buffer)
	{
		PS3_Log("[video] FATAL: memalign failed for the %d MB IO buffer", RSX_IO_SIZE >> 20);
		return -1;
	}

	ret = rsxInit(&ctx, RSX_CB_SIZE, RSX_IO_SIZE, io_buffer);

	if (ret != 0 || !ctx)
	{
		PS3_Log("[video] FATAL: rsxInit failed (%d)", (int)ret);
		return -1;
	}

	/* 720p if the TV takes it; otherwise whatever the TV is set to. */
	if (!videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720,
				VIDEO_ASPECT_16_9, 0))
	{
		videoGetState(0, 0, &state);
		vid_res = state.displayMode.resolution;
		vid_aspect = state.displayMode.aspect;
		PS3_Log("[video] 720p not available, using the TV's mode (%d)", (int)vid_res);
	}

	if (videoGetResolution(vid_res, &res) != 0)
	{
		PS3_Log("[video] FATAL: videoGetResolution failed");
		return -1;
	}

	disp_w = res.width;
	disp_h = res.height;
	disp_pitch = disp_w * 4;

	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = vid_res;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = disp_pitch;
	vconfig.aspect = vid_aspect;

	if (videoConfigure(0, &vconfig, NULL, 0) != 0)
	{
		PS3_Log("[video] FATAL: videoConfigure failed");
		return -1;
	}

	/* wait until the video out is no longer busy switching modes */
	waited = 0;

	do
	{
		usleep(10000);

		if (videoGetState(0, 0, &state) != 0)
		{
			break;
		}
	}
	while (state.state == 3 && ++waited < 300);

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	for (i = 0; i < FB_COUNT; i++)
	{
		fb[i] = (u32 *)rsxMemalign(FB_ALIGN, disp_pitch * disp_h);

		if (!fb[i])
		{
			PS3_Log("[video] FATAL: rsxMemalign failed for framebuffer %d", i);
			return -1;
		}

		memset(fb[i], 0, disp_pitch * disp_h);
		rsxAddressToOffset(fb[i], &fb_offset[i]);
		gcmSetDisplayBuffer(i, fb_offset[i], disp_pitch, disp_w, disp_h);
	}

	cur = 0;
	flip_queued = 0;
	flip_completed = 0;
	clear_frames = FB_COUNT;

	gcmSetFlipHandler(FlipHandler);
	gcmSetVBlankHandler(VBlankHandler);

	ready = 1;
	PS3_Log("[video] ready: %ux%u, %d framebuffers", (unsigned)disp_w,
			(unsigned)disp_h, FB_COUNT);

	return 0;
}

void
PS3_Video_FreeSource(void)
{
	int i;

	if (ready)
	{
		PS3_Video_Finish();
	}

	for (i = 0; i < FB_COUNT; i++)
	{
		if (src[i])
		{
			rsxFree(src[i]);
			src[i] = NULL;
		}
	}

	if (depth)
	{
		rsxFree(depth);
		depth = NULL;
	}

	src_w = src_h = 0;
	gl_mode = 0;
}

void
PS3_Video_Shutdown(void)
{
	int i;

	if (!ready)
	{
		return;
	}

	PS3_Log("[video] shutdown");

	/* ORDER MATTERS: unhook the handlers first. */
	gcmSetFlipHandler(NULL);
	gcmSetVBlankHandler(NULL);
	PS3_Video_Finish();

	PS3_Video_FreeSource();

	for (i = 0; i < FB_COUNT; i++)
	{
		if (fb[i])
		{
			rsxFree(fb[i]);
			fb[i] = NULL;
		}
	}

	ready = 0;
}

/* Allocates the source images: w x h, 'pitch' bytes per row, plus the
   depth buffer in OpenGL mode. */
static int
AllocSource(int w, int h, u32 pitch, int with_depth)
{
	int i;

	PS3_Video_FreeSource();

	for (i = 0; i < FB_COUNT; i++)
	{
		src[i] = (u32 *)rsxMemalign(FB_ALIGN, pitch * h);

		if (!src[i])
		{
			PS3_Log("[video] FATAL: rsxMemalign failed for a %dx%d source image", w, h);
			PS3_Video_FreeSource();
			return -1;
		}

		memset(src[i], 0, pitch * h);
		rsxAddressToOffset(src[i], &src_offset[i]);
	}

	if (with_depth)
	{
		depth_pitch = pitch;   /* Z24S8: 4 bytes per pixel too */
		depth = (u32 *)rsxMemalign(FB_ALIGN, depth_pitch * h);

		if (!depth)
		{
			PS3_Log("[video] FATAL: rsxMemalign failed for a %dx%d depth buffer", w, h);
			PS3_Video_FreeSource();
			return -1;
		}

		rsxAddressToOffset(depth, &depth_offset);
	}

	src_w = w;
	src_h = h;
	src_pitch = pitch;
	gl_mode = with_depth;
	clear_frames = FB_COUNT;   /* the picture's size changed: redo borders */

	return 0;
}

int
PS3_Video_SetSource(int w, int h)
{
	if (!ready || w <= 0 || h <= 0)
	{
		return -1;
	}

	if (src[0] && !gl_mode && w == src_w && h == src_h)
	{
		return 0;
	}

	if (AllocSource(w, h, (u32)w * 4, 0) != 0)
	{
		return -1;
	}

	PS3_Log("[video] source image %dx%d (x%d)", w, h, FB_COUNT);

	return 0;
}

int
PS3_Video_SetRenderTarget(int w, int h)
{
	if (!ready || w <= 0 || h <= 0 || w > 2047 || h > 2047)
	{
		return -1;
	}

	if (src[0] && gl_mode && w == src_w && h == src_h)
	{
		return 0;
	}

	/* color surface pitch: a multiple of 64 bytes */
	if (AllocSource(w, h, ((u32)w * 4 + 63) & ~63u, 1) != 0)
	{
		return -1;
	}

	PS3_Log("[video] render targets %dx%d (x%d, pitch %u) + depth/stencil",
			w, h, FB_COUNT, (unsigned)src_pitch);

	return 0;
}

gcmContextData *
PS3_Video_Context(void)
{
	return ready ? ctx : NULL;
}

int
PS3_Video_RenderTargetInfo(uint32_t *offset, uint32_t *pitch)
{
	if (!ready || !gl_mode || !src[cur])
	{
		return -1;
	}

	*offset = src_offset[cur];
	*pitch = src_pitch;

	return 0;
}

void
PS3_Video_BindRenderTarget(void)
{
	gcmSurface sf;
	int i;

	if (!ready || !gl_mode || !src[cur])
	{
		return;
	}
	PS3_Cmd_Reserve();

	/* the target isn't free until the flip that showed it is done */
	{
		u64 t0 = sysGetSystemTime();

		WaitFlips();
		prof_wait_us += sysGetSystemTime() - t0;
	}

	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = src_offset[cur];
	sf.colorPitch[0] = src_pitch;

	for (i = 1; i < 4; i++)
	{
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = src_offset[cur];
		sf.colorPitch[i] = 64;
	}

	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depth_offset;
	sf.depthPitch = depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = (u16)src_w;
	sf.height = (u16)src_h;
	sf.x = 0;
	sf.y = 0;

	rsxSetSurface(ctx, &sf);
}

uint32_t *
PS3_Video_Source(void)
{
	if (!ready || !src[cur])
	{
		return NULL;
	}

	WaitFlips();

	return src[cur];
}

void
PS3_Video_SetFit(int percent)
{
	if (percent < 50)
	{
		percent = 50;
	}

	if (percent > 100)
	{
		percent = 100;
	}

	if (percent != fit_percent)
	{
		fit_percent = percent;
		clear_frames = FB_COUNT;
	}
}

void
PS3_Video_SetFilter(int linear)
{
	filter_linear = linear ? 1 : 0;
}

void
PS3_Video_SetPerfLog(int on)
{
	perf_log = on ? 1 : 0;
}

void
PS3_Video_SetLock30(int lock)
{
	lock30 = lock ? 1 : 0;
}

void
PS3_Video_ProfileCopy(long long usec)
{
	if (usec > 0)
	{
		prof_copy_us += (u64)usec;
	}
}

/* ================================================================ */
/* Brightness and gamma                                               */
/* ================================================================ */

/*
 * With neutral values the picture goes to the TV with the transfer
 * scaler, as always. Otherwise it is drawn by the 3D unit as one textured
 * quad through a fragment program that applies them (gl/shaders/
 * tq_fp_present.fcg): the same for both renderers, HUD and menus
 * included. The values ride in the quad's second texture coordinate, so
 * they change without patching the program.
 */

static float color_brightness = 1.0f;
static float color_exponent = 1.0f;

static int cp_state;            /* 0 not tried, 1 ready, -1 failed */
static rsxVertexProgram *cp_vp;
static void *cp_vp_ucode;
static rsxProgramConst *cp_mvp;
static rsxFragmentProgram *cp_fp;
static void *cp_fp_ucode;
static u32 cp_fp_size, cp_fp_offset;
static u8 *cp_verts;            /* 4 vertices per framebuffer */
static u32 cp_verts_offset;
static u32 *cp_depth;           /* the surface wants one; never tested */
static u32 cp_depth_offset;

void
PS3_Video_SetColor(float brightness, float exponent)
{
	if (brightness < 0.25f) brightness = 0.25f;
	if (brightness > 4.0f) brightness = 4.0f;
	if (exponent < 0.25f) exponent = 0.25f;
	if (exponent > 4.0f) exponent = 4.0f;

	color_brightness = brightness;
	color_exponent = exponent;
}

static int
ColorPassInit(void)
{
	void *ucode;
	u32 vp_size;

	if (cp_state)
	{
		return cp_state;
	}

	cp_state = -1;

	cp_vp = (rsxVertexProgram *)present_vp_vpo;
	rsxVertexProgramGetUCode(cp_vp, &cp_vp_ucode, &vp_size);
	cp_mvp = rsxVertexProgramGetConst(cp_vp, "mvp");

	cp_fp = (rsxFragmentProgram *)present_fp_fpo;
	rsxFragmentProgramGetUCode(cp_fp, &ucode, &cp_fp_size);
	cp_fp_ucode = rsxMemalign(64, cp_fp_size);
	cp_verts = (u8 *)rsxMemalign(128, FB_COUNT * 4 * PS3GL_VERTEX_SIZE + 128);
	cp_depth = (u32 *)rsxMemalign(FB_ALIGN, disp_pitch * disp_h);

	if (!cp_mvp || !cp_fp_ucode || !cp_verts || !cp_depth)
	{
		PS3_Log("[video] brightness/gamma pass: init failed, staying with the plain blit");
		return cp_state;
	}

	memcpy(cp_fp_ucode, ucode, cp_fp_size);
	rsxAddressToOffset(cp_fp_ucode, &cp_fp_offset);
	rsxAddressToOffset(cp_verts, &cp_verts_offset);
	rsxAddressToOffset(cp_depth, &cp_depth_offset);

	cp_state = 1;
	PS3_Log("[video] brightness/gamma pass ready");

	return cp_state;
}

static void
ColorPass(int off_x, int off_y, int out_w, int out_h)
{
	static const float identity[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1
	};
	static const float corner[4][4] = {
		/* x, y, u, v: top left, top right, bottom right, bottom left */
		{-1.0f, 1.0f, 0.0f, 0.0f},
		{1.0f, 1.0f, 1.0f, 0.0f},
		{1.0f, -1.0f, 1.0f, 1.0f},
		{-1.0f, -1.0f, 0.0f, 1.0f},
	};
	gcmSurface sf;
	gcmTexture tex;
	ps3gl_vertex_t *v;
	u32 voff;
	float vscale[4], voffset[4];
	int i;

	/* the screen as the target */
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = fb_offset[cur];
	sf.colorPitch[0] = disp_pitch;

	for (i = 1; i < 4; i++)
	{
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = fb_offset[cur];
		sf.colorPitch[i] = 64;
	}

	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = cp_depth_offset;
	sf.depthPitch = disp_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = (u16)disp_w;
	sf.height = (u16)disp_h;

	/* the image must be finished (3D unit, or the CPU) and freshly read */
	__asm__ volatile("sync" ::: "memory");
	rsxSetWaitForIdle(ctx);

	rsxSetSurface(ctx, &sf);

	/* plain state */
	rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
			GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetBlendEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
	rsxSetStencilTestEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetPolygonOffsetFillEnable(ctx, GCM_FALSE);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);

	/* Black borders. The clear is clipped by the viewport as well as the
	   scissor, and the viewport still is the renderer's (smaller in 4:3
	   modes): the whole screen first, or the bars keep old frames. */
	vscale[0] = disp_w * 0.5f;
	vscale[1] = disp_h * -0.5f;
	vscale[2] = 0.5f;
	vscale[3] = 0.0f;
	voffset[0] = disp_w * 0.5f;
	voffset[1] = disp_h * 0.5f;
	voffset[2] = 0.5f;
	voffset[3] = 0.0f;
	rsxSetViewport(ctx, 0, 0, (u16)disp_w, (u16)disp_h, 0.0f, 1.0f, vscale, voffset);
	rsxSetViewportClip(ctx, 0, disp_w, disp_h);
	rsxSetScissor(ctx, 0, 0, (u16)disp_w, (u16)disp_h);
	rsxSetClearColor(ctx, 0xff000000u);
	rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

	/* the picture's rectangle */
	vscale[0] = out_w * 0.5f;
	vscale[1] = out_h * -0.5f;
	vscale[2] = 0.5f;
	vscale[3] = 0.0f;
	voffset[0] = off_x + out_w * 0.5f;
	voffset[1] = off_y + out_h * 0.5f;
	voffset[2] = 0.5f;
	voffset[3] = 0.0f;
	rsxSetViewport(ctx, (u16)off_x, (u16)off_y, (u16)out_w, (u16)out_h,
			0.0f, 1.0f, vscale, voffset);
	rsxSetViewportClip(ctx, 0, disp_w, disp_h);

	/* the image as a linear texture */
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);

	memset(&tex, 0, sizeof(tex));
	tex.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = 0x00AAE4;   /* identity, as in ps3gl */
	tex.width = (u16)src_w;
	tex.height = (u16)src_h;
	tex.depth = 1;
	tex.location = GCM_LOCATION_RSX;
	tex.pitch = src_pitch;
	tex.offset = src_offset[cur];

	rsxLoadTexture(ctx, 0, &tex);
	rsxTextureControl(ctx, 0, GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, 0, 0,
			filter_linear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
			filter_linear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
			GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, 0, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
			GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
			GCM_TEXTURE_ZFUNC_NEVER, 0);
	rsxTextureControl(ctx, 1, GCM_FALSE, 0, 0, 0);

	/* programs */
	rsxLoadVertexProgram(ctx, cp_vp, cp_vp_ucode);
	rsxSetVertexProgramParameter(ctx, cp_vp, cp_mvp, identity);
	rsxLoadFragmentProgramLocation(ctx, cp_fp, cp_fp_offset, GCM_LOCATION_RSX);

	/* the quad, in this framebuffer's slot (its last use is done) */
	voff = cp_verts_offset + (u32)cur * 4 * PS3GL_VERTEX_SIZE;
	v = (ps3gl_vertex_t *)(cp_verts + (u32)cur * 4 * PS3GL_VERTEX_SIZE);

	for (i = 0; i < 4; i++)
	{
		v[i].x = corner[i][0];
		v[i].y = corner[i][1];
		v[i].z = 0.0f;
		v[i].w = 1.0f;
		v[i].u0 = corner[i][2];
		v[i].v0 = corner[i][3];
		v[i].u1 = color_brightness;
		v[i].v1 = color_exponent;
		v[i].color = 0xffffffffu;
	}

	__asm__ volatile("sync" ::: "memory");

	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, voff + PS3GL_VATTR_POS_OFF,
			PS3GL_VERTEX_SIZE, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, voff + PS3GL_VATTR_TC0_OFF,
			PS3GL_VERTEX_SIZE, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, voff + PS3GL_VATTR_TC1_OFF,
			PS3GL_VERTEX_SIZE, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, voff + PS3GL_VATTR_COLOR_OFF,
			PS3GL_VERTEX_SIZE, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);

	rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_FAN, 0, 4);
}

void
PS3_Video_Present(void)
{
	static int frames;
	gcmTransferScale scale;
	gcmTransferSurface surface;
	int avail_w, avail_h, out_w, out_h, off_x, off_y;
	int color_pass;

	u64 t0;

	if (!ready || !src[cur])
	{
		return;
	}

	t0 = sysGetSystemTime();
	WaitFlips();
	prof_wait_us += sysGetSystemTime() - t0;

	/* Keep the picture's aspect (square pixels), inside the screen fit
	   area, centered. 4:3 modes get black bars on a 16:9 TV. */
	avail_w = (int)disp_w * fit_percent / 100;
	avail_h = (int)disp_h * fit_percent / 100;
	out_h = avail_h;
	out_w = out_h * src_w / src_h;

	if (out_w > avail_w)
	{
		out_w = avail_w;
		out_h = out_w * src_h / src_w;
	}

	off_x = ((int)disp_w - out_w) / 2;
	off_y = ((int)disp_h - out_h) / 2;

	color_pass = (color_brightness != 1.0f || color_exponent != 1.0f) && ColorPassInit() == 1;

	PS3_Cmd_Reserve();
	if (color_pass)
	{
		ColorPass(off_x, off_y, out_w, out_h);
		goto flip;
	}

	if (clear_frames > 0)
	{
		memset(fb[cur], 0, disp_pitch * disp_h);
		clear_frames--;
	}

	memset(&scale, 0, sizeof(scale));
	scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
	scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
	scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
	scale.clipX = off_x;
	scale.clipY = off_y;
	scale.clipW = out_w;
	scale.clipH = out_h;
	scale.outX = off_x;
	scale.outY = off_y;
	scale.outW = out_w;
	scale.outH = out_h;
	scale.ratioX = rsxGetFixedSint32((float)src_w / (float)out_w);
	scale.ratioY = rsxGetFixedSint32((float)src_h / (float)out_h);
	scale.inW = src_w;
	scale.inH = src_h;
	scale.pitch = src_pitch;
	scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
	scale.interp = filter_linear ? GCM_TRANSFER_INTERPOLATOR_LINEAR :
		GCM_TRANSFER_INTERPOLATOR_NEAREST;
	scale.offset = src_offset[cur];
	scale.inX = 0;
	scale.inY = 0;

	memset(&surface, 0, sizeof(surface));
	surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surface.pitch = disp_pitch;
	surface.offset = fb_offset[cur];

	PS3_Cmd_Reserve();
	/* The renderer's writes must be in memory before the RSX reads them. */
	__asm__ volatile("sync" ::: "memory");

	/* OpenGL: the 3D unit drew the image; the scaler (another unit) must
	   not start reading it before the drawing is finished. */
	if (gl_mode)
	{
		rsxSetWaitForIdle(ctx);
	}

	rsxSetTransferScaleSurface(ctx, &scale, &surface);

flip:

	/* Locked 30: flip every second vblank. The flip below happens on the
	   vblank after it is queued, so queue it once 2 have gone by since the
	   previous one. */
	if (lock30)
	{
		int waited = 0;

		t0 = sysGetSystemTime();

		while ((u32)(vblank_count - last_flip_vblank) < 2 && ++waited < 1000)
		{
			usleep(100);
		}

		prof_lock_us += sysGetSystemTime() - t0;
	}

	last_flip_vblank = vblank_count;

	PS3_Cmd_Reserve();
	/* TyrQuakeCell's sequence: flip, flush, wait-flip. */
	gcmSetFlip(ctx, cur);
	rsxFlushBuffer(ctx);
	gcmSetWaitFlip(ctx);

	flip_queued++;
	cur = (cur + 1) % FB_COUNT;

	if (frames < 3)
	{
		PS3_Log("[video] frame %d presented: %dx%d -> %dx%d at %d,%d", frames,
				src_w, src_h, out_w, out_h, off_x, off_y);
	}

	frames++;

	/* Frame rate in the log every 10 seconds. Not more often: every write
	   to the HDD flashes the system's activity icon. */
	{
		static u64 window_start;
		static int window_frames;
		u64 now = sysGetSystemTime();

		if (window_start == 0)
		{
			window_start = now;
		}

		window_frames++;

		if (now - window_start >= 10000000ull && !perf_log)
		{
			/* off (vid_ps3_perflog 0): just start a new window */
#ifdef GLQUAKE
			if (gl_mode)
			{
				PS3GL_LogStats(0);
			}
#endif

			window_start = now;
			window_frames = 0;
			prof_copy_us = prof_wait_us = prof_lock_us = 0;
		}
		else if (now - window_start >= 10000000ull)
		{
			double ms = (double)(now - window_start) / 1000.0 / window_frames;

			/* frame = everything; copy = palette -> RSX memory; wait =
			   waiting for a free framebuffer; lock = 30 fps pacing. The
			   rest of the frame is game + client + software rendering. */
			if (gl_mode)
			{
				/* wait = the RSX is behind (GPU bound) */
				PS3_Log("[video] %.1f fps (%dx%d, opengl%s%s): frame %.1f ms, wait %.1f, lock %.1f",
						1000.0 / ms, src_w, src_h, lock30 ? ", locked 30" : "",
						color_pass ? ", color pass" : "", ms,
						prof_wait_us / 1000.0 / window_frames,
						prof_lock_us / 1000.0 / window_frames);
#ifdef GLQUAKE
				PS3GL_LogStats(window_frames);
#endif
			}
			else
			{
				PS3_Log("[video] %.1f fps (%dx%d%s%s): frame %.1f ms, copy %.1f, wait %.1f, lock %.1f",
						1000.0 / ms, src_w, src_h, lock30 ? ", locked 30" : "",
						color_pass ? ", color pass" : "", ms,
						prof_copy_us / 1000.0 / window_frames,
						prof_wait_us / 1000.0 / window_frames,
						prof_lock_us / 1000.0 / window_frames);
			}

			window_start = now;
			window_frames = 0;
			prof_copy_us = prof_wait_us = prof_lock_us = 0;
		}
	}
}
