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
 * ps3gl: the OpenGL 1.3 subset TyrQuake's GL renderer uses, on top of the
 * RSX. Quake2PS3's ps3gl (made for Yamagi's ref_gl1), with what GLQuake
 * needs on top:
 *
 *  - Immediate mode (glBegin/glEnd), for the 2D drawing, sprites and the
 *    screen blends. Quads and polygons become triangles.
 *  - Three texture units and GL's texture environments as TyrQuake
 *    combines them (REPLACE, MODULATE, COMBINE x2/x4, DECAL): one
 *    generated fragment program per combination (shaders/gen_fp.py).
 *  - glCopyTexSubImage2D, for the water warp textures (rendered into the
 *    corner of the frame, then copied and swizzled by the RSX).
 *  - Texture names are handed out and taken back (TyrQuake frees and
 *    reallocates them on every map).
 *
 * Quake2PS3's ps3gl is itself based on ps3gl from IoQuake3-PS3 (GPLv2,
 * code/gl/), which runs Quake 3 on real hardware: same state tracking,
 * matrix stacks, vertex format, shaders and GCM calls. Changed for Quake 2
 * and for what Doom64-PS3 taught us on hardware:
 *
 *  - Real client arrays: glDrawArrays (all primitive types) and
 *    glDrawElements with any start index. ref_gl1 draws everything this
 *    way (no glBegin/glEnd).
 *  - Power of two textures are SWIZZLED, with their mip levels packed one
 *    after the other: linear textures only work with CLAMP, and Quake 2
 *    repeats its wall textures. Non power of two ones (the cinematics)
 *    stay linear and clamped.
 *  - Texture names index the table directly, no linear search per bind
 *    (glGenTextures hands them out here).
 *  - A frame fence (backend label): a texture the RSX may still be reading
 *    is never overwritten by glTexImage2D (new storage is allocated, the
 *    old one freed later), and freed memory waits for the RSX too.
 *  - GL_UNPACK_ROW_LENGTH and GL_RGB_SCALE (lightmap updates, overbright).
 *
 * The vertex ring has one segment per frame in flight; everything is
 * copied there (the game's arrays live in ordinary memory).
 *
 * =======================================================================
 */

#ifndef PS3GL_H
#define PS3GL_H

#include <stdint.h>
#include <string.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "GL/gl.h"

#define PS3GL_MAX_TMUS          3
#define PS3GL_MAX_TEXTURES      4096    /* texture names 1..4095 */
#define PS3GL_MATRIX_DEPTH      32

/* One ring segment per frame that can be in flight (triple buffering). */
#define PS3GL_RING_SEGMENTS     3
#define PS3GL_RING_SEG_SIZE     (6 * 1024 * 1024)

/* GCM labels 0-63 are the system's. IoQuake3-PS3 uses 64-66. */
#define PS3GL_LABEL_FRAME       68

/* Frames a texture memory block waits after its last use before being
   freed or reused, on top of the fence (belt and braces). */
#define PS3GL_FREE_DELAY        2

/* Dirty flags for the render state */
#define PS3GL_DIRTY_BLEND       0x0001u
#define PS3GL_DIRTY_ALPHA       0x0002u
#define PS3GL_DIRTY_DEPTH       0x0004u
#define PS3GL_DIRTY_STENCIL     0x0008u
#define PS3GL_DIRTY_VIEWPORT    0x0010u
#define PS3GL_DIRTY_CULL        0x0020u
#define PS3GL_DIRTY_SCISSOR     0x0040u
#define PS3GL_DIRTY_COLORMASK   0x0080u
#define PS3GL_DIRTY_POLYOFFSET  0x0100u
#define PS3GL_DIRTY_SHADE       0x0200u
#define PS3GL_DIRTY_ALL         0xFFFFu

/* Fragment programs (shaders/gen_fp.py, ps3gl_shader_data.h): one per
   combination of what the texture units do, index u0 + 4 * (u1 + 5 * u2). */
enum { PS3GL_U0_OFF, PS3GL_U0_REPLACE, PS3GL_U0_MODULATE, PS3GL_U0_MODULATE2X };
enum { PS3GL_U1_OFF, PS3GL_U1_MODULATE, PS3GL_U1_MODULATE2X, PS3GL_U1_MODULATE4X,
	PS3GL_U1_DECAL };
enum { PS3GL_U2_OFF, PS3GL_U2_DECAL };
#define PS3GL_FP_INDEX(u0, u1, u2) ((u0) + 4 * ((u1) + 5 * (u2)))
#define PS3GL_FP_COUNT 40

/* 36 byte interleaved vertex, as in IoQuake3-PS3 */
#pragma pack(push, 1)
typedef struct {
	float x, y, z, w;
	float u0, v0;
	float u1, v1;
	uint32_t color;         /* bytes R G B A */
} ps3gl_vertex_t;
#pragma pack(pop)

#define PS3GL_VERTEX_SIZE       36
#define PS3GL_VATTR_POS_OFF     0
#define PS3GL_VATTR_TC0_OFF     16
#define PS3GL_VATTR_TC1_OFF     24
#define PS3GL_VATTR_COLOR_OFF   32

typedef struct {
	uint8_t    *data;           /* RSX memory, NULL = no storage */
	uint32_t    offset;
	uint32_t    size;
	uint32_t    pitch;          /* linear only */
	uint16_t    width, height;  /* level 0 */
	uint8_t     swizzled;
	uint8_t     max_levels;     /* levels the storage has room for */
	uint8_t     num_levels;     /* levels uploaded (contiguous from 0) */
	uint8_t     dirty;          /* descriptor or sampler state changed */
	uint8_t     allocated;      /* name handed out by glGenTextures */
	GLenum      min_filter, mag_filter, wrap_s, wrap_t;
	uint32_t    last_frame;     /* last frame that drew with it */
	gcmTexture  gt;
} ps3gl_texture_t;

typedef struct {
	int         tex;            /* bound texture name */
	int         enabled;        /* GL_TEXTURE_2D */
	GLenum      env;            /* GL_TEXTURE_ENV_MODE */
	int         rgb_scale;      /* GL_RGB_SCALE (GL_COMBINE) */
	int         hw_tex;         /* what the sampler has: name, -1 none */
	int         hw_enabled;
} ps3gl_tmu_t;

typedef struct {
	float       stack[PS3GL_MATRIX_DEPTH][16];
	int         depth;
} ps3gl_matstack_t;

typedef struct {
	int         enabled;
	GLint       size;
	GLenum      type;
	GLsizei     stride;
	const void *ptr;
} ps3gl_array_t;

typedef struct {
	gcmContextData *ctx;
	int         screen_w, screen_h;     /* the render target */

	uint32_t    frame;                  /* frame being built, from 1 */
	volatile uint32_t *frame_label;     /* last frame the RSX finished */

	uint32_t    dirty;
	struct {
		int      blend;
		GLenum   blend_src, blend_dst;
		int      alpha_test;
		GLenum   alpha_func;
		float    alpha_ref;
		int      depth_test, depth_mask;
		GLenum   depth_func;
		int      cull;
		GLenum   cull_face, front_face;
		int      scissor;
		int      sc_x, sc_y, sc_w, sc_h;    /* GL convention */
		int      vp_x, vp_y, vp_w, vp_h;    /* GL convention */
		float    depth_near, depth_far;
		int      mask_r, mask_g, mask_b, mask_a;
		int      polyoffset;
		float    po_factor, po_units;
		GLenum   shade;
		int      stencil;
		GLenum   st_func;
		GLint    st_ref;
		GLuint   st_mask, st_writemask;
		GLenum   st_fail, st_zfail, st_zpass;
		uint32_t clear_color;               /* ARGB */
		float    clear_depth;
		int      clear_stencil;
	} rs;

	/* texture units */
	int         active_tmu, client_tmu;
	ps3gl_tmu_t tmu[PS3GL_MAX_TMUS];
	int         unpack_row_length;
	int         tex_written;            /* CPU wrote texture memory */

	/* matrices (the texture matrix is kept but not used) */
	GLenum      matrix_mode;
	ps3gl_matstack_t mv, proj, tex;
	int         mvp_dirty;
	float       mvp[16];                /* row major, for the VP */
	int         mvp_uploaded;
	/* NZ:P additions, evaluated in nzp_vp (rsx/shaders/nzp_vp.vasm):
	   the texture matrix applied to unit 0's coordinates (two rows that
	   are dotted with (s, t, 0, 1)), and GL_FOG in GL_LINEAR mode, its
	   factor from the clip-space w (the eye depth under glFrustum). */
	float       texmat[8];
	int         texmat_uploaded;
	struct {
		int     enabled;
		float   start, end;
		float   color[4];
	} fog;
	int         fog_uploaded;

	/* client arrays */
	ps3gl_array_t va_vertex, va_color, va_texcoord[PS3GL_MAX_TMUS];
	uint32_t    color;                  /* current color, bytes R G B A */

	/* immediate mode: glBegin .. glEnd */
	int         in_begin;
	GLenum      im_mode;
	float       im_tc[2];               /* current texture coordinate */

	/* drawn since the RSX was last made to wait (glCopyTexSubImage2D) */
	int         drew;
	/* a copy into a texture is queued: wait for it before the next draw */
	int         copy_pending;

	/* vertex ring */
	uint8_t    *ring;
	uint32_t    ring_off;
	uint32_t    ring_seg, ring_head;
	uint32_t    ring_peak;

	/* shaders */
	int         active_fp;
	int         vp_loaded;

	ps3gl_texture_t *textures;          /* [PS3GL_MAX_TEXTURES] */

	/* statistics, reset by PS3GL_LogStats */
	uint32_t    st_draws, st_verts, st_upload, st_dropped, st_copies;
	uint32_t    st_tex_bytes;
} ps3gl_state_t;

extern ps3gl_state_t ps3gl;

/* ---- used by the platform (vid_ps3_gl.c, ps3_video.c) ---- */

/* Once the render target exists (PS3_Video_SetRenderTarget). Sets the
   size again on a mode change. 0 ok, -1 error. */
int  PS3GL_Init(int width, int height);
int  PS3GL_Ready(void);

/* Frame bracket. Begin binds the render target (waiting for a free one)
   and resets the per frame state; End closes the frame (fence). The
   caller then presents with PS3_Video_Present(). */
void PS3GL_BeginFrame(void);
void PS3GL_EndFrame(void);

/* One log line with the averages since the last call. */
void PS3GL_LogStats(int frames);   /* 0: only resets the counters */

/* Culling direction check (cvar gl_ps3_cullflip). */
void PS3GL_SetCullFlip(int flip);

/* For GL_GetProcAddress: the entry points TyrQuake looks up by name. */
void *PS3GL_GetProcAddress(const char *name);

/* ---- internal ---- */

uint32_t ps3gl_completed_frame(void);
int      ps3gl_cull_flip(void);
void     ps3gl_defer_free(void *data, uint32_t last_frame);
void     ps3gl_process_frees(void);

void     ps3gl_states_reset(void);
void     ps3gl_apply_states(void);
void     ps3gl_matrices_reset(void);
void     ps3gl_apply_matrices(void);
void     ps3gl_textures_init(void);
void     ps3gl_apply_textures(unsigned units);   /* bit i: sampler i used */
void     ps3gl_shaders_init(void);
void     ps3gl_apply_shader(int fp);

#endif /* PS3GL_H */
