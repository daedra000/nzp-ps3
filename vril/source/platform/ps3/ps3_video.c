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
/* Tile regions (render targets in the RSX's tiled layout: blending and depth
   go much faster than in plain rows). One per source image, one for depth. */
#define TILE_ALIGN    0x10000
static int tiles_bound;
static void UnbindTiles(void);
static u32 *gm_buf;             /* gamma: the 3D view graded into this, then back */
static u32 gm_offset;

static int clear_frames;     /* framebuffers that still need black borders */
static int perf_log = 1;     /* fps and timings in the log every 10 s (bring-up: on) */
static int fit_percent = 90;
static int filter_linear = 1;

static volatile u32 vblank_count;
static u32 last_flip_vblank;
static int lock30;

/* profiling, reset every 10 seconds */
static u64 prof_copy_us, prof_wait_us, prof_lock_us;
static int frames_presented;


/* ================================================================ */

static void
FlipHandler(const u32 head)
{
	(void)head;
	/* after a forced sync (WaitFlips gave up on a held flip) the held flips
	   still complete: don't count past what was queued, or WaitFlips stops
	   waiting and a buffer on screen gets drawn over */
	if (flip_completed != flip_queued)
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
	PS3_Cmd_ReserveWords(PS3_CMD_ROOM);
}

/* Room for 'words' (at most a few thousand: the pieces are 32 KB). */
void
PS3_Cmd_ReserveWords(unsigned int words)
{
	static int logged;

	if (!ctx)
	{
		return;
	}

	if (words < PS3_CMD_ROOM)
	{
		words = PS3_CMD_ROOM;
	}

	if (ctx->current + words <= ctx->end)
	{
		return;
	}

	if (CallContextCallback(ctx, words) != 0 && !logged)
	{
		logged = 1;
		PS3_Log("[video] WARNING: command buffer callback failed");
	}

	if (logged < 2 && ctx->current + words > ctx->end)
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
		/* On 1 MB boundaries: the XMB's screenshot reads the buffer on screen
		   straight out of RSX memory, and with 64-byte-aligned buffers it held
		   the flip for good and the console froze (Grid Siege v2.6.9, the fix
		   confirmed on HEN; tiny3d's buffers start at offset 0). */
		fb[i] = (u32 *)rsxMemalign(1024 * 1024, disp_pitch * disp_h);

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
	UnbindTiles();

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

	if (gm_buf)
	{
		rsxFree(gm_buf);
		gm_buf = NULL;
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

/* The pitches the RSX can tile, smallest first. */
static u32
TiledPitch(u32 bytes)
{
	static const u32 pitches[] = {
		0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0xA00, 0xC00, 0xD00,
		0xE00, 0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000,
		0x3400, 0x3800, 0x4000
	};
	unsigned i;

	for (i = 0; i < sizeof(pitches) / sizeof(pitches[0]); i++)
	{
		if (pitches[i] >= bytes)
		{
			return pitches[i];
		}
	}
	return 0;
}

static void
UnbindTiles(void)
{
	int i;

	for (i = 0; i < tiles_bound; i++)
	{
		gcmUnbindTile((u8)i);
	}
	tiles_bound = 0;
}

/* Tiles a buffer (in RSX memory, TILE_ALIGN-aligned, 'size' a multiple of it). */
static void
TileBuffer(u32 offset, u32 size, u32 pitch)
{
	s32 rc = gcmSetTileInfo((u8)tiles_bound, GCM_LOCATION_RSX, offset, size, pitch,
			GCM_COMPMODE_DISABLED, 0, 0);

	if (rc == 0)
	{
		rc = gcmBindTile((u8)tiles_bound);
	}
	if (rc != 0)
	{
		PS3_Log("[video] tile %d not set (0x%08x): that buffer stays untiled", tiles_bound, (unsigned)rc);
		return;
	}
	tiles_bound++;
}

/* Allocates the source images: w x h, 'pitch' bytes per row, plus the
   depth buffer in OpenGL mode. */
static int
AllocSource(int w, int h, u32 pitch, int with_depth)
{
	int i;
	u32 align = FB_ALIGN, size;

	PS3_Video_FreeSource();

	/* OpenGL: tiled (the CPU never draws into these) */
	if (with_depth && TiledPitch(pitch))
	{
		pitch = TiledPitch(pitch);
		align = TILE_ALIGN;
	}
	size = pitch * h;
	if (align == TILE_ALIGN)
	{
		size = pitch * (((u32)h + 31) & ~31u);
		size = (size + TILE_ALIGN - 1) & ~(TILE_ALIGN - 1);
	}

	for (i = 0; i < FB_COUNT; i++)
	{
		src[i] = (u32 *)rsxMemalign(align, size);

		if (!src[i])
		{
			PS3_Log("[video] FATAL: rsxMemalign failed for a %dx%d source image", w, h);
			PS3_Video_FreeSource();
			return -1;
		}

		memset(src[i], 0, pitch * h);
		rsxAddressToOffset(src[i], &src_offset[i]);
		if (align == TILE_ALIGN)
		{
			TileBuffer(src_offset[i], size, pitch);
		}
	}

	if (with_depth)
	{
		depth_pitch = pitch;   /* Z24S8: 4 bytes per pixel too */
		depth = (u32 *)rsxMemalign(align, align == TILE_ALIGN ? size : depth_pitch * h);

		if (!depth)
		{
			PS3_Log("[video] FATAL: rsxMemalign failed for a %dx%d depth buffer", w, h);
			PS3_Video_FreeSource();
			return -1;
		}

		rsxAddressToOffset(depth, &depth_offset);
		if (align == TILE_ALIGN)
		{
			TileBuffer(depth_offset, size, depth_pitch);
		}
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

	PS3_Log("[video] render targets %dx%d (x%d, pitch %u) + depth/stencil, %d tiled",
			w, h, FB_COUNT, (unsigned)src_pitch, tiles_bound);

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


/* the source image (and depth) as the render target */
static void
SetMainSurface(void)
{
	gcmSurface sf;
	int i;

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

/*
 * Dynamic resolution (Video Options > DYNAMIC RES), as Halo CE PS3's: when the
 * RSX can't keep up with 60 fps (late rounds' explosions and fire), the 3D view
 * is drawn smaller (7/8, 6/8, then 5/8 of the render target) and scaled back up
 * before the HUD, which stays sharp. The RSX counts as the limit when it's
 * still on the frame before when the next one is handed over; frames slow
 * because of the CPU (a big map) change nothing, a smaller view wouldn't help.
 * One step every half second at most; back up an eighth after a few seconds
 * of headroom, and longer before the next try if going up was too soon.
 */
static int dyn_enabled = 1;
static int dyn_k = 8, dyn_frame_k = 8;   /* eighths of the base view */
/* The 3D view's size: RESOLUTION's (the render target stays the screen's,
   so the HUD is always drawn at full resolution), then dynamic resolution's
   eighths of that. 0: the render target's. */
static int view_base_w, view_base_h, view_w, view_h;

void
PS3_Video_SetViewBase(int w, int h)
{
	view_base_w = w;
	view_base_h = h;
}

/* This frame's 3D view size, in the render target's bottom left */
void
PS3_Video_ViewSize(int *w, int *h)
{
	*w = view_w ? view_w : src_w;
	*h = view_h ? view_h : src_h;
}
static u64 dyn_last_start, dyn_total_us;
static int dyn_frames, dyn_late, dyn_hold, dyn_good, dyn_up_wait = 180, dyn_since_up = -1;

void
PS3_Video_SetDynamicRes(int on)
{
	dyn_enabled = on;
	if (!on)
	{
		dyn_k = 8;
	}
}


/* From PS3GL_EndFrame: was the RSX still on the frame before this one? */
void
PS3_Video_DynSample(int gpu_late)
{
	u64 avg;
	int late;

	dyn_frames++;
	dyn_late += gpu_late ? 1 : 0;
	if (dyn_hold > 0)
	{
		dyn_hold--;
	}
	if (dyn_since_up >= 0 && ++dyn_since_up > 600)
	{
		dyn_since_up = -1;
		dyn_up_wait = 180;   /* held up there for 10 s: tries come quickly again */
	}
	if (dyn_frames < 30)
	{
		return;
	}
	avg = dyn_total_us / (u64)dyn_frames;
	late = dyn_late * 100 / dyn_frames;
	dyn_frames = dyn_late = 0;
	dyn_total_us = 0;

	if (!dyn_enabled)
	{
		dyn_k = 8;
		return;
	}
	if (avg > 17600 && late >= 50 && dyn_k > 5 && !dyn_hold)
	{
		if (dyn_since_up >= 0 && dyn_since_up < 120)
		{
			dyn_up_wait = dyn_up_wait * 2 > 1800 ? 1800 : dyn_up_wait * 2;
		}
		dyn_k--;
		dyn_hold = 30;
		dyn_good = 0;
		dyn_since_up = -1;
		PS3_Log("[video] dynamic resolution: 3D view at %d/8 (%.1f ms a frame, the RSX behind %d%%)",
				dyn_k, avg / 1000.0, late);
	}
	else if (avg < 17200 && late < 10 && dyn_k < 8 && !dyn_hold)
	{
		dyn_good += 30;
		if (dyn_good >= dyn_up_wait)
		{
			dyn_k++;
			dyn_hold = 30;
			dyn_good = 0;
			dyn_since_up = 0;
			PS3_Log("[video] dynamic resolution: 3D view at %d/8", dyn_k);
		}
	}
	else
	{
		dyn_good = 0;
	}
}

void
PS3_Video_BindRenderTarget(void)
{
	if (!ready || !gl_mode || !src[cur])
	{
		return;
	}
	PS3_Cmd_Reserve();

	/* the target isn't free until the flip that showed it is done */
	{
		u64 t0 = sysGetSystemTime();

		WaitFlips();
		t0 = sysGetSystemTime() - t0;
		prof_wait_us += t0;
	}

	/* frame to frame time, and this frame's view size (fixed for the frame) */
	{
		u64 now = sysGetSystemTime();
		if (dyn_last_start)
		{
			dyn_total_us += now - dyn_last_start;
		}
		dyn_last_start = now;
		dyn_frame_k = dyn_enabled ? dyn_k : 8;
		{
			int bw = view_base_w > 0 && view_base_w <= src_w ? view_base_w : src_w;
			int bh = view_base_h > 0 && view_base_h <= src_h ? view_base_h : src_h;
			view_w = bw * dyn_frame_k / 8;
			view_h = bh * dyn_frame_k / 8;
		}
	}

	SetMainSurface();
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
	cp_verts = (u8 *)rsxMemalign(128, 3 * FB_COUNT * 4 * PS3GL_VERTEX_SIZE + 128);
	/* cp_depth: only for the pass onto the screen (ColorPass), made when
	   that's first used; the gamma pass on the 3D view doesn't need it */

	if (!cp_mvp || !cp_fp_ucode || !cp_verts)
	{
		PS3_Log("[video] brightness/gamma pass: init failed, staying with the plain blit");
		return cp_state;
	}

	memcpy(cp_fp_ucode, ucode, cp_fp_size);
	rsxAddressToOffset(cp_fp_ucode, &cp_fp_offset);
	rsxAddressToOffset(cp_verts, &cp_verts_offset);

	cp_state = 1;
	PS3_Log("[video] brightness/gamma pass ready");

	return cp_state;
}

/* One textured quad through present_fp (pow(texel, exponent) * brightness):
   'tex' (linear, tex_w x tex_h) drawn into the rectangle of 'dst'. */
typedef struct
{
	u32 dst_off, dst_pitch, z_off, z_pitch;
	int dst_w, dst_h, clear;
	u32 tex_off, tex_pitch;
	int tex_w, tex_h, linear;
	int off_x, off_y, out_w, out_h;
	float brightness, exponent;
	int slot;
	int region;                  /* set: sample only tu0,tv0 .. tu1,tv1 of the texture */
	float tu0, tv0, tu1, tv1;
} pass_t;

static void
Pass(const pass_t *p)
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
	sf.colorOffset[0] = p->dst_off;
	sf.colorPitch[0] = p->dst_pitch;

	for (i = 1; i < 4; i++)
	{
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = p->dst_off;
		sf.colorPitch[i] = 64;
	}

	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = p->z_off;
	sf.depthPitch = p->z_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = (u16)p->dst_w;
	sf.height = (u16)p->dst_h;

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
	vscale[0] = p->dst_w * 0.5f;
	vscale[1] = p->dst_h * -0.5f;
	vscale[2] = 0.5f;
	vscale[3] = 0.0f;
	voffset[0] = p->dst_w * 0.5f;
	voffset[1] = p->dst_h * 0.5f;
	voffset[2] = 0.5f;
	voffset[3] = 0.0f;
	rsxSetViewport(ctx, 0, 0, (u16)p->dst_w, (u16)p->dst_h, 0.0f, 1.0f, vscale, voffset);
	rsxSetViewportClip(ctx, 0, p->dst_w, p->dst_h);
	rsxSetScissor(ctx, 0, 0, (u16)p->dst_w, (u16)p->dst_h);
	if (p->clear)
	{
		rsxSetClearColor(ctx, 0xff000000u);
		rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	}

	/* the picture's rectangle */
	vscale[0] = p->out_w * 0.5f;
	vscale[1] = p->out_h * -0.5f;
	vscale[2] = 0.5f;
	vscale[3] = 0.0f;
	voffset[0] = p->off_x + p->out_w * 0.5f;
	voffset[1] = p->off_y + p->out_h * 0.5f;
	voffset[2] = 0.5f;
	voffset[3] = 0.0f;
	rsxSetViewport(ctx, (u16)p->off_x, (u16)p->off_y, (u16)p->out_w, (u16)p->out_h,
			0.0f, 1.0f, vscale, voffset);
	rsxSetViewportClip(ctx, 0, p->dst_w, p->dst_h);

	/* the image as a linear texture */
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);

	memset(&tex, 0, sizeof(tex));
	tex.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = 0x00AAE4;   /* identity, as in ps3gl */
	tex.width = (u16)p->tex_w;
	tex.height = (u16)p->tex_h;
	tex.depth = 1;
	tex.location = GCM_LOCATION_RSX;
	tex.pitch = p->tex_pitch;
	tex.offset = p->tex_off;

	rsxLoadTexture(ctx, 0, &tex);
	rsxTextureControl(ctx, 0, GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, 0, 0,
			p->linear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
			p->linear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
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
	voff = cp_verts_offset + (u32)p->slot * 4 * PS3GL_VERTEX_SIZE;
	v = (ps3gl_vertex_t *)(cp_verts + (u32)p->slot * 4 * PS3GL_VERTEX_SIZE);

	for (i = 0; i < 4; i++)
	{
		v[i].x = corner[i][0];
		v[i].y = corner[i][1];
		v[i].z = 0.0f;
		v[i].w = 1.0f;
		v[i].u0 = p->region ? p->tu0 + corner[i][2] * (p->tu1 - p->tu0) : corner[i][2];
		v[i].v0 = p->region ? p->tv0 + corner[i][3] * (p->tv1 - p->tv0) : corner[i][3];
		v[i].u1 = p->brightness;
		v[i].v1 = p->exponent;
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

/* The finished picture onto the screen with brightness/gamma (no view pass). */
static void
ColorPass(int off_x, int off_y, int out_w, int out_h)
{
	pass_t p;

	if (!cp_depth)
	{
		cp_depth = (u32 *)rsxMemalign(FB_ALIGN, disp_pitch * disp_h);
		if (!cp_depth)
		{
			return;
		}
		rsxAddressToOffset(cp_depth, &cp_depth_offset);
	}

	memset(&p, 0, sizeof(p));
	p.dst_off = fb_offset[cur];
	p.dst_pitch = disp_pitch;
	p.dst_w = disp_w;
	p.dst_h = disp_h;
	p.z_off = cp_depth_offset;
	p.z_pitch = disp_pitch;
	p.clear = 1;
	p.tex_off = src_offset[cur];
	p.tex_pitch = src_pitch;
	p.tex_w = src_w;
	p.tex_h = src_h;
	p.linear = filter_linear;
	p.off_x = off_x;
	p.off_y = off_y;
	p.out_w = out_w;
	p.out_h = out_h;
	p.brightness = color_brightness;
	p.exponent = color_exponent;
	p.slot = cur;
	Pass(&p);
}

static void SetMainSurface(void);

/*
 * Gamma on the 3D view only: called once the world and the weapon are drawn,
 * before the HUD. The view goes through pow() into a scratch image and is
 * copied back; the HUD and menus are drawn on top unchanged.
 */
void
PS3_Video_GammaView(void)
{
	extern void PS3GL_InvalidateState(void);
	pass_t p;

	if (!ready || !gl_mode || !src[cur] ||
		(color_exponent == 1.0f && color_brightness == 1.0f && view_w >= src_w && view_h >= src_h))
	{
		return;
	}
	if (ColorPassInit() != 1)
	{
		return;
	}
	if (!gm_buf)
	{
		gm_buf = (u32 *)rsxMemalign(FB_ALIGN, src_pitch * src_h);
		if (!gm_buf)
		{
			return;
		}
		rsxAddressToOffset(gm_buf, &gm_offset);
	}

	memset(&p, 0, sizeof(p));
	p.dst_pitch = src_pitch;
	p.dst_w = src_w;
	p.dst_h = src_h;
	p.z_off = depth_offset;   /* not tested or written */
	p.z_pitch = depth_pitch;
	p.tex_pitch = src_pitch;
	p.tex_w = src_w;
	p.tex_h = src_h;
	p.out_w = src_w;
	p.out_h = src_h;

	/* Dynamic resolution: the view is in the render target's bottom left
	   (GL's origin: R_SetupGL's viewport), dyn_frame_k eighths of it each
	   way; it's scaled up over the whole scratch image, filtered. */
	if (view_w < src_w || view_h < src_h)
	{
		int sw = view_w, sh = view_h;

		p.region = 1;
		p.linear = 1;
		p.tu0 = 0.5f / src_w;
		p.tu1 = (sw - 0.5f) / src_w;
		p.tv0 = (src_h - sh + 0.5f) / src_h;
		p.tv1 = (src_h - 0.5f) / src_h;
	}

	/* the view, graded (and scaled up), into the scratch image */
	PS3_Cmd_Reserve();
	p.dst_off = gm_offset;
	p.tex_off = src_offset[cur];
	p.brightness = color_brightness;
	p.exponent = color_exponent;
	p.slot = FB_COUNT + cur * 2;
	Pass(&p);

	/* and back, as it is */
	PS3_Cmd_Reserve();
	p.region = 0;
	p.linear = 0;
	p.dst_off = src_offset[cur];
	p.tex_off = gm_offset;
	p.brightness = 1.0f;
	p.exponent = 1.0f;
	p.slot = FB_COUNT + cur * 2 + 1;
	Pass(&p);

	PS3_Cmd_Reserve();
	SetMainSurface();
	PS3GL_InvalidateState();
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
	t0 = sysGetSystemTime() - t0;
	prof_wait_us += t0;

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

	/* gamma is applied to the 3D view (PS3_Video_GammaView), not the HUD */
	color_pass = 0;

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
	/* Flip and flush. No gcmSetWaitFlip: while the system holds a flip back
	   (an XMB screenshot) the RSX would stall behind it, and a screenshot that
	   waits for the RSX would wait forever (Grid Siege's freeze). The CPU
	   checks the flips instead (WaitFlips) before reusing a buffer. */
	gcmSetFlip(ctx, cur);
	rsxFlushBuffer(ctx);

	flip_queued++;
	cur = (cur + 1) % FB_COUNT;

	if (frames < 3)
	{
		PS3_Log("[video] frame %d presented: %dx%d -> %dx%d at %d,%d", frames,
				src_w, src_h, out_w, out_h, off_x, off_y);
	}

	frames++;
	frames_presented = frames;

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
