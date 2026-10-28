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
 * ps3gl: render state, matrices and the current color. The GCM side of
 * the state (ps3gl_apply_states) and the matrix code are IoQuake3-PS3's
 * ps3gl_states.c / ps3gl_matrices.c.
 *
 * GL's window origin is the bottom left corner, the RSX's the top left:
 * viewport and scissor rectangles are flipped (y = height - y - h), and
 * the viewport transform flips the picture (negative y scale).
 *
 * =======================================================================
 */

#include <math.h>

#include "ps3gl.h"
#include "../ps3_video.h"

int ps3gl_cull_flip(void);

static const float identity[16] = {
	1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 1, 0,
	0, 0, 0, 1
};

/* ================================================================ */
/* GL -> GCM                                                          */
/* ================================================================ */

static uint32_t
CmpFunc(GLenum func)
{
	switch (func)
	{
		case GL_NEVER:    return GCM_NEVER;
		case GL_LESS:     return GCM_LESS;
		case GL_EQUAL:    return GCM_EQUAL;
		case GL_LEQUAL:   return GCM_LEQUAL;
		case GL_GREATER:  return GCM_GREATER;
		case GL_NOTEQUAL: return GCM_NOTEQUAL;
		case GL_GEQUAL:   return GCM_GEQUAL;
		default:          return GCM_ALWAYS;
	}
}

static uint16_t
BlendFactor(GLenum f)
{
	switch (f)
	{
		case GL_ZERO:                return GCM_ZERO;
		case GL_ONE:                 return GCM_ONE;
		case GL_SRC_COLOR:           return GCM_SRC_COLOR;
		case GL_ONE_MINUS_SRC_COLOR: return GCM_ONE_MINUS_SRC_COLOR;
		case GL_SRC_ALPHA:           return GCM_SRC_ALPHA;
		case GL_ONE_MINUS_SRC_ALPHA: return GCM_ONE_MINUS_SRC_ALPHA;
		case GL_DST_ALPHA:           return GCM_DST_ALPHA;
		case GL_ONE_MINUS_DST_ALPHA: return GCM_ONE_MINUS_DST_ALPHA;
		case GL_DST_COLOR:           return GCM_DST_COLOR;
		case GL_ONE_MINUS_DST_COLOR: return GCM_ONE_MINUS_DST_COLOR;
		case GL_SRC_ALPHA_SATURATE:  return GCM_SRC_ALPHA_SATURATE;
		default:                     return GCM_ONE;
	}
}

static uint32_t
StencilOp(GLenum op)
{
	switch (op)
	{
		case GL_ZERO:    return GCM_ZERO;
		case GL_REPLACE: return GCM_REPLACE;
		case GL_INCR:    return GCM_INCR;
		case GL_DECR:    return GCM_DECR;
		case GL_INVERT:  return GCM_INVERT;
		default:         return GCM_KEEP;
	}
}

/* ================================================================ */
/* State                                                              */
/* ================================================================ */

void
ps3gl_states_reset(void)
{
	int i;

	memset(&ps3gl.rs, 0, sizeof(ps3gl.rs));

	ps3gl.rs.blend_src = GL_ONE;
	ps3gl.rs.blend_dst = GL_ZERO;
	ps3gl.rs.alpha_func = GL_ALWAYS;
	ps3gl.rs.depth_mask = 1;
	ps3gl.rs.depth_func = GL_LESS;
	ps3gl.rs.cull_face = GL_BACK;
	ps3gl.rs.front_face = GL_CCW;
	ps3gl.rs.sc_w = ps3gl.rs.vp_w = ps3gl.screen_w;
	ps3gl.rs.sc_h = ps3gl.rs.vp_h = ps3gl.screen_h;
	ps3gl.rs.depth_near = 0.0f;
	ps3gl.rs.depth_far = 1.0f;
	ps3gl.rs.mask_r = ps3gl.rs.mask_g = ps3gl.rs.mask_b = ps3gl.rs.mask_a = 1;
	ps3gl.rs.shade = GL_SMOOTH;
	ps3gl.rs.st_func = GL_ALWAYS;
	ps3gl.rs.st_mask = ps3gl.rs.st_writemask = 0xffffffffu;
	ps3gl.rs.st_fail = ps3gl.rs.st_zfail = ps3gl.rs.st_zpass = GL_KEEP;
	ps3gl.rs.clear_color = 0;
	ps3gl.rs.clear_depth = 1.0f;

	for (i = 0; i < PS3GL_MAX_TMUS; i++)
	{
		ps3gl.tmu[i].tex = 0;
		ps3gl.tmu[i].enabled = 0;
		ps3gl.tmu[i].env = GL_MODULATE;
		ps3gl.tmu[i].rgb_scale = 1;
		ps3gl.tmu[i].hw_tex = -2;
		ps3gl.tmu[i].hw_enabled = -1;
	}

	ps3gl.active_tmu = ps3gl.client_tmu = 0;
	ps3gl.dirty = PS3GL_DIRTY_ALL;
}

static void
EnableDisable(GLenum cap, int on)
{
	switch (cap)
	{
		case GL_BLEND:
			ps3gl.rs.blend = on;
			ps3gl.dirty |= PS3GL_DIRTY_BLEND;
			break;
		case GL_ALPHA_TEST:
			ps3gl.rs.alpha_test = on;
			ps3gl.dirty |= PS3GL_DIRTY_ALPHA;
			break;
		case GL_DEPTH_TEST:
			ps3gl.rs.depth_test = on;
			ps3gl.dirty |= PS3GL_DIRTY_DEPTH;
			break;
		case GL_CULL_FACE:
			ps3gl.rs.cull = on;
			ps3gl.dirty |= PS3GL_DIRTY_CULL;
			break;
		case GL_SCISSOR_TEST:
			ps3gl.rs.scissor = on;
			ps3gl.dirty |= PS3GL_DIRTY_SCISSOR;
			break;
		case GL_STENCIL_TEST:
			ps3gl.rs.stencil = on;
			ps3gl.dirty |= PS3GL_DIRTY_STENCIL;
			break;
		case GL_POLYGON_OFFSET_FILL:
			ps3gl.rs.polyoffset = on;
			ps3gl.dirty |= PS3GL_DIRTY_POLYOFFSET;
			break;
		case GL_TEXTURE_2D:
			ps3gl.tmu[ps3gl.active_tmu].enabled = on;
			break;
		case GL_FOG:
			if (ps3gl.fog.enabled != on)
			{
				ps3gl.fog.enabled = on;
				ps3gl.fog_uploaded = 0;
			}
			break;
		default:
			/* GL_POINT_SMOOTH, GL_MULTISAMPLE...: nothing */
			break;
	}
}

void APIENTRY glEnable(GLenum cap) { EnableDisable(cap, 1); }
void APIENTRY glDisable(GLenum cap) { EnableDisable(cap, 0); }

void APIENTRY
glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	ps3gl.rs.blend_src = sfactor;
	ps3gl.rs.blend_dst = dfactor;
	ps3gl.dirty |= PS3GL_DIRTY_BLEND;
}

void APIENTRY
glAlphaFunc(GLenum func, GLclampf ref)
{
	ps3gl.rs.alpha_func = func;
	ps3gl.rs.alpha_ref = ref;
	ps3gl.dirty |= PS3GL_DIRTY_ALPHA;
}

void APIENTRY
glDepthFunc(GLenum func)
{
	ps3gl.rs.depth_func = func;
	ps3gl.dirty |= PS3GL_DIRTY_DEPTH;
}

void APIENTRY
glDepthMask(GLboolean flag)
{
	ps3gl.rs.depth_mask = flag ? 1 : 0;
	ps3gl.dirty |= PS3GL_DIRTY_DEPTH;
}

void APIENTRY
glDepthRange(GLclampd zNear, GLclampd zFar)
{
	ps3gl.rs.depth_near = (float)zNear;
	ps3gl.rs.depth_far = (float)zFar;
	ps3gl.dirty |= PS3GL_DIRTY_VIEWPORT;
}

void APIENTRY
glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	ps3gl.rs.mask_r = r ? 1 : 0;
	ps3gl.rs.mask_g = g ? 1 : 0;
	ps3gl.rs.mask_b = b ? 1 : 0;
	ps3gl.rs.mask_a = a ? 1 : 0;
	ps3gl.dirty |= PS3GL_DIRTY_COLORMASK;
}

void APIENTRY
glCullFace(GLenum mode)
{
	ps3gl.rs.cull_face = mode;
	ps3gl.dirty |= PS3GL_DIRTY_CULL;
}

void APIENTRY
glFrontFace(GLenum mode)
{
	ps3gl.rs.front_face = mode;
	ps3gl.dirty |= PS3GL_DIRTY_CULL;
}

void APIENTRY
glPolygonMode(GLenum face, GLenum mode)
{
	/* only debugging views use GL_LINE */
	(void)face;
	(void)mode;
}

void APIENTRY
glPolygonOffset(GLfloat factor, GLfloat units)
{
	ps3gl.rs.po_factor = factor;
	ps3gl.rs.po_units = units;
	ps3gl.dirty |= PS3GL_DIRTY_POLYOFFSET;
}

void APIENTRY
glShadeModel(GLenum mode)
{
	ps3gl.rs.shade = mode;
	ps3gl.dirty |= PS3GL_DIRTY_SHADE;
}

void APIENTRY
glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	ps3gl.rs.sc_x = x;
	ps3gl.rs.sc_y = y;
	ps3gl.rs.sc_w = width;
	ps3gl.rs.sc_h = height;
	ps3gl.dirty |= PS3GL_DIRTY_SCISSOR;
}

void APIENTRY
glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	ps3gl.rs.vp_x = x;
	ps3gl.rs.vp_y = y;
	ps3gl.rs.vp_w = width;
	ps3gl.rs.vp_h = height;
	ps3gl.dirty |= PS3GL_DIRTY_VIEWPORT;
}

void APIENTRY
glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
	ps3gl.rs.st_func = func;
	ps3gl.rs.st_ref = ref;
	ps3gl.rs.st_mask = mask;
	ps3gl.dirty |= PS3GL_DIRTY_STENCIL;
}

void APIENTRY
glStencilMask(GLuint mask)
{
	ps3gl.rs.st_writemask = mask;
	ps3gl.dirty |= PS3GL_DIRTY_STENCIL;
}

void APIENTRY
glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
	ps3gl.rs.st_fail = fail;
	ps3gl.rs.st_zfail = zfail;
	ps3gl.rs.st_zpass = zpass;
	ps3gl.dirty |= PS3GL_DIRTY_STENCIL;
}

static uint8_t
ToByte(float f)
{
	if (f <= 0.0f)
	{
		return 0;
	}

	if (f >= 1.0f)
	{
		return 255;
	}

	return (uint8_t)(f * 255.0f + 0.5f);
}

void APIENTRY
glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	ps3gl.rs.clear_color = ((uint32_t)ToByte(a) << 24) | ((uint32_t)ToByte(r) << 16) |
		((uint32_t)ToByte(g) << 8) | ToByte(b);
}

void APIENTRY
glClearDepth(GLclampd depth)
{
	ps3gl.rs.clear_depth = (float)depth;
}

void APIENTRY
glClearStencil(GLint s)
{
	ps3gl.rs.clear_stencil = s;
}

void APIENTRY
glClear(GLbitfield mask)
{
	uint32_t gcm_mask = 0;

	if (!ps3gl.ctx)
	{
		return;
	}

	PS3_Cmd_Reserve();
	/* the clear obeys the scissor and the color mask */
	ps3gl_apply_states();

	if (mask & GL_COLOR_BUFFER_BIT)
	{
		rsxSetClearColor(ps3gl.ctx, ps3gl.rs.clear_color);

		if (ps3gl.rs.mask_r) gcm_mask |= GCM_CLEAR_R;
		if (ps3gl.rs.mask_g) gcm_mask |= GCM_CLEAR_G;
		if (ps3gl.rs.mask_b) gcm_mask |= GCM_CLEAR_B;
		if (ps3gl.rs.mask_a) gcm_mask |= GCM_CLEAR_A;
	}

	if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
	{
		/* Z24S8: depth in the top 24 bits */
		float d = ps3gl.rs.clear_depth;
		uint32_t z24;

		if (d < 0.0f) d = 0.0f;
		if (d > 1.0f) d = 1.0f;

		z24 = (uint32_t)(d * 16777215.0f);
		rsxSetClearDepthStencil(ps3gl.ctx, (z24 << 8) | (ps3gl.rs.clear_stencil & 0xff));

		if ((mask & GL_DEPTH_BUFFER_BIT) && ps3gl.rs.depth_mask)
		{
			gcm_mask |= GCM_CLEAR_Z;
		}

		if (mask & GL_STENCIL_BUFFER_BIT)
		{
			gcm_mask |= GCM_CLEAR_S;
		}
	}

	if (gcm_mask)
	{
		/* GL's clear only obeys the scissor, the RSX's also the viewport,
		   and TyrQuake clears right after drawing the water warp into a
		   corner: the whole render target for the clear, then the GL
		   viewport again at the next draw. */
		float scale[4], offset[4];

		scale[0] = ps3gl.screen_w * 0.5f;
		scale[1] = ps3gl.screen_h * -0.5f;
		scale[2] = 0.5f;
		scale[3] = 0.0f;
		offset[0] = ps3gl.screen_w * 0.5f;
		offset[1] = ps3gl.screen_h * 0.5f;
		offset[2] = 0.5f;
		offset[3] = 0.0f;
		rsxSetViewport(ps3gl.ctx, 0, 0, (uint16_t)ps3gl.screen_w, (uint16_t)ps3gl.screen_h,
				0.0f, 1.0f, scale, offset);
		rsxSetViewportClip(ps3gl.ctx, 0, ps3gl.screen_w, ps3gl.screen_h);

		rsxClearSurface(ps3gl.ctx, gcm_mask);

		ps3gl.dirty |= PS3GL_DIRTY_VIEWPORT;
	}
}

/* GL rectangle (origin bottom left) -> RSX rectangle (top left), clipped
   to the render target. Returns 0 if nothing is left. */
static int
FlipClip(int x, int y, int w, int h, int *ox, int *oy, int *ow, int *oh)
{
	int sw = ps3gl.screen_w, sh = ps3gl.screen_h;

	y = sh - y - h;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > sw) w = sw - x;
	if (y + h > sh) h = sh - y;

	*ox = x;
	*oy = y;
	*ow = w;
	*oh = h;

	return (w > 0 && h > 0);
}

void
ps3gl_apply_states(void)
{
	gcmContextData *ctx = ps3gl.ctx;
	uint32_t d = ps3gl.dirty;

	if (!d || !ctx)
	{
		return;
	}

	if (d & PS3GL_DIRTY_BLEND)
	{
		rsxSetBlendEnable(ctx, ps3gl.rs.blend ? GCM_TRUE : GCM_FALSE);

		if (ps3gl.rs.blend)
		{
			uint16_t s = BlendFactor(ps3gl.rs.blend_src);
			uint16_t t = BlendFactor(ps3gl.rs.blend_dst);

			rsxSetBlendFunc(ctx, s, t, s, t);
			rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
		}
	}

	if (d & PS3GL_DIRTY_ALPHA)
	{
		rsxSetAlphaTestEnable(ctx, ps3gl.rs.alpha_test ? GCM_TRUE : GCM_FALSE);

		if (ps3gl.rs.alpha_test)
		{
			rsxSetAlphaFunc(ctx, CmpFunc(ps3gl.rs.alpha_func), ToByte(ps3gl.rs.alpha_ref));
		}
	}

	if (d & PS3GL_DIRTY_DEPTH)
	{
		rsxSetDepthTestEnable(ctx, ps3gl.rs.depth_test ? GCM_TRUE : GCM_FALSE);
		rsxSetDepthFunc(ctx, CmpFunc(ps3gl.rs.depth_func));
		rsxSetDepthWriteEnable(ctx, ps3gl.rs.depth_mask ? GCM_TRUE : GCM_FALSE);
	}

	if (d & PS3GL_DIRTY_CULL)
	{
		rsxSetCullFaceEnable(ctx, ps3gl.rs.cull ? GCM_TRUE : GCM_FALSE);

		if (ps3gl.rs.cull)
		{
			uint32_t face;
			int ccw = (ps3gl.rs.front_face != GL_CW);

			switch (ps3gl.rs.cull_face)
			{
				case GL_FRONT:          face = GCM_CULL_FRONT; break;
				case GL_FRONT_AND_BACK: face = GCM_CULL_ALL; break;
				default:                face = GCM_CULL_BACK; break;
			}

			/* gl_ps3_cullflip: in case the RSX sees the winding the other
			   way round (y flip). IoQuake3-PS3 does it like GL, and so did
			   Quake2PS3 on hardware (the cvar stayed 0). */
			if (ps3gl_cull_flip())
			{
				ccw = !ccw;
			}

			rsxSetCullFace(ctx, face);
			rsxSetFrontFace(ctx, ccw ? GCM_FRONTFACE_CCW : GCM_FRONTFACE_CW);
		}
	}

	if (d & PS3GL_DIRTY_SCISSOR)
	{
		int x, y, w, h;

		if (ps3gl.rs.scissor)
		{
			if (!FlipClip(ps3gl.rs.sc_x, ps3gl.rs.sc_y, ps3gl.rs.sc_w, ps3gl.rs.sc_h,
						&x, &y, &w, &h))
			{
				/* empty: a 1x1 corner is the closest the RSX can do */
				x = y = 0;
				w = h = 1;
			}
		}
		else
		{
			x = y = 0;
			w = ps3gl.screen_w;
			h = ps3gl.screen_h;
		}

		rsxSetScissor(ctx, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
	}

	if (d & PS3GL_DIRTY_VIEWPORT)
	{
		/* The transform uses the whole viewport (even the part outside the
		   render target); only the rectangle given to the RSX is clipped. */
		int vx = ps3gl.rs.vp_x, vw = ps3gl.rs.vp_w, vh = ps3gl.rs.vp_h;
		int vy = ps3gl.screen_h - ps3gl.rs.vp_y - vh;
		float zn = ps3gl.rs.depth_near, zf = ps3gl.rs.depth_far;
		float scale[4], offset[4];
		int cx, cy, cw, ch;

		scale[0] = vw * 0.5f;
		scale[1] = vh * -0.5f;
		scale[2] = (zf - zn) * 0.5f;
		scale[3] = 0.0f;
		offset[0] = vx + vw * 0.5f;
		offset[1] = vy + vh * 0.5f;
		offset[2] = (zf + zn) * 0.5f;
		offset[3] = 0.0f;

		if (!FlipClip(ps3gl.rs.vp_x, ps3gl.rs.vp_y, vw, vh, &cx, &cy, &cw, &ch))
		{
			cx = cy = 0;
			cw = ch = 1;
		}

		rsxSetViewport(ctx, (uint16_t)cx, (uint16_t)cy, (uint16_t)cw, (uint16_t)ch,
				zn, zf, scale, offset);
		rsxSetViewportClip(ctx, 0, ps3gl.screen_w, ps3gl.screen_h);
	}

	if (d & PS3GL_DIRTY_COLORMASK)
	{
		uint32_t mask = 0;

		if (ps3gl.rs.mask_b) mask |= GCM_COLOR_MASK_B;
		if (ps3gl.rs.mask_g) mask |= GCM_COLOR_MASK_G;
		if (ps3gl.rs.mask_r) mask |= GCM_COLOR_MASK_R;
		if (ps3gl.rs.mask_a) mask |= GCM_COLOR_MASK_A;

		rsxSetColorMask(ctx, mask);
	}

	if (d & PS3GL_DIRTY_POLYOFFSET)
	{
		rsxSetPolygonOffsetFillEnable(ctx, ps3gl.rs.polyoffset ? GCM_TRUE : GCM_FALSE);

		if (ps3gl.rs.polyoffset)
		{
			rsxSetPolygonOffset(ctx, ps3gl.rs.po_factor, ps3gl.rs.po_units);
		}
	}

	if (d & PS3GL_DIRTY_SHADE)
	{
		rsxSetShadeModel(ctx, ps3gl.rs.shade == GL_FLAT ?
				GCM_SHADE_MODEL_FLAT : GCM_SHADE_MODEL_SMOOTH);
	}

	if (d & PS3GL_DIRTY_STENCIL)
	{
		rsxSetStencilTestEnable(ctx, ps3gl.rs.stencil ? GCM_TRUE : GCM_FALSE);

		if (ps3gl.rs.stencil)
		{
			rsxSetStencilFunc(ctx, CmpFunc(ps3gl.rs.st_func), ps3gl.rs.st_ref,
					ps3gl.rs.st_mask);
			rsxSetStencilOp(ctx, StencilOp(ps3gl.rs.st_fail),
					StencilOp(ps3gl.rs.st_zfail), StencilOp(ps3gl.rs.st_zpass));
			rsxSetStencilMask(ctx, ps3gl.rs.st_writemask);
		}
	}

	ps3gl.dirty = 0;
}

/* ================================================================ */
/* Current color                                                      */
/* ================================================================ */

static void
SetColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	ps3gl.color = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

void APIENTRY glColor3f(GLfloat r, GLfloat g, GLfloat b)
{
	SetColor(ToByte(r), ToByte(g), ToByte(b), 255);
}

void APIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	SetColor(ToByte(r), ToByte(g), ToByte(b), ToByte(a));
}

void APIENTRY glColor4fv(const GLfloat *v)
{
	SetColor(ToByte(v[0]), ToByte(v[1]), ToByte(v[2]), ToByte(v[3]));
}

void APIENTRY glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
	SetColor(r, g, b, a);
}

void APIENTRY glColor4ubv(const GLubyte *v)
{
	SetColor(v[0], v[1], v[2], v[3]);
}

void APIENTRY glColor3fv(const GLfloat *v)
{
	SetColor(ToByte(v[0]), ToByte(v[1]), ToByte(v[2]), 255);
}

void APIENTRY glColor3ub(GLubyte r, GLubyte g, GLubyte b)
{
	SetColor(r, g, b, 255);
}

void APIENTRY glColor3ubv(const GLubyte *v)
{
	SetColor(v[0], v[1], v[2], 255);
}

/* ================================================================ */
/* Matrices (column major, like GL)                                   */
/* ================================================================ */

static ps3gl_matstack_t *
Stack(void)
{
	switch (ps3gl.matrix_mode)
	{
		case GL_PROJECTION: return &ps3gl.proj;
		case GL_TEXTURE:    return &ps3gl.tex;   /* unit 0's, applied in nzp_vp */
		default:            return &ps3gl.mv;
	}
}

static float *
Top(void)
{
	ps3gl_matstack_t *s = Stack();

	return s->stack[s->depth];
}

/* out = a * b */
static void
MatMul(float *out, const float *a, const float *b)
{
	float t[16];
	int c, r;

	for (c = 0; c < 4; c++)
	{
		for (r = 0; r < 4; r++)
		{
			t[c * 4 + r] =
				a[0 * 4 + r] * b[c * 4 + 0] +
				a[1 * 4 + r] * b[c * 4 + 1] +
				a[2 * 4 + r] * b[c * 4 + 2] +
				a[3 * 4 + r] * b[c * 4 + 3];
		}
	}

	memcpy(out, t, sizeof(t));
}

static void
MultTop(const float *m)
{
	float *top = Top();

	MatMul(top, top, m);
	ps3gl.mvp_dirty = 1;
}

void
ps3gl_matrices_reset(void)
{
	memcpy(ps3gl.mv.stack[0], identity, sizeof(identity));
	memcpy(ps3gl.proj.stack[0], identity, sizeof(identity));
	memcpy(ps3gl.tex.stack[0], identity, sizeof(identity));
	ps3gl.mv.depth = ps3gl.proj.depth = ps3gl.tex.depth = 0;
	ps3gl.matrix_mode = GL_MODELVIEW;
	ps3gl.mvp_dirty = 1;
	memset(ps3gl.texmat, 0, sizeof(ps3gl.texmat));
	ps3gl.texmat[0] = ps3gl.texmat[5] = 1.0f;
	ps3gl.texmat_uploaded = 0;
}

void
ps3gl_apply_matrices(void)
{
	float m[16];
	float tm[8];
	const float *t;
	int r, c;

	if (!ps3gl.mvp_dirty)
	{
		return;
	}

	/* texture matrix rows for (s, t, 0, 1); only re-sent when they change */
	t = ps3gl.tex.stack[ps3gl.tex.depth];
	tm[0] = t[0]; tm[1] = t[4]; tm[2] = t[8];  tm[3] = t[12];
	tm[4] = t[1]; tm[5] = t[5]; tm[6] = t[9];  tm[7] = t[13];
	if (memcmp(tm, ps3gl.texmat, sizeof(tm)) != 0)
	{
		memcpy(ps3gl.texmat, tm, sizeof(tm));
		ps3gl.texmat_uploaded = 0;
	}

	MatMul(m, ps3gl.proj.stack[ps3gl.proj.depth], ps3gl.mv.stack[ps3gl.mv.depth]);

	/* the vertex program wants rows */
	for (r = 0; r < 4; r++)
	{
		for (c = 0; c < 4; c++)
		{
			ps3gl.mvp[r * 4 + c] = m[c * 4 + r];
		}
	}

	ps3gl.mvp_dirty = 0;
	ps3gl.mvp_uploaded = 0;
}

void APIENTRY
glMatrixMode(GLenum mode)
{
	ps3gl.matrix_mode = mode;
}

void APIENTRY
glLoadIdentity(void)
{
	memcpy(Top(), identity, sizeof(identity));
	ps3gl.mvp_dirty = 1;
}

void APIENTRY
glLoadMatrixf(const GLfloat *m)
{
	memcpy(Top(), m, 16 * sizeof(float));
	ps3gl.mvp_dirty = 1;
}

void APIENTRY
glMultMatrixf(const GLfloat *m)
{
	MultTop(m);
}

void APIENTRY
glPushMatrix(void)
{
	ps3gl_matstack_t *s = Stack();

	if (s->depth < PS3GL_MATRIX_DEPTH - 1)
	{
		memcpy(s->stack[s->depth + 1], s->stack[s->depth], 16 * sizeof(float));
		s->depth++;
	}
}

void APIENTRY
glPopMatrix(void)
{
	ps3gl_matstack_t *s = Stack();

	if (s->depth > 0)
	{
		s->depth--;
		ps3gl.mvp_dirty = 1;
	}
}

void APIENTRY
glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	float m[16];

	memset(m, 0, sizeof(m));
	m[0] = (float)(2.0 / (r - l));
	m[5] = (float)(2.0 / (t - b));
	m[10] = (float)(-2.0 / (f - n));
	m[12] = (float)(-(r + l) / (r - l));
	m[13] = (float)(-(t + b) / (t - b));
	m[14] = (float)(-(f + n) / (f - n));
	m[15] = 1.0f;

	MultTop(m);
}

void APIENTRY
glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	float m[16];

	memset(m, 0, sizeof(m));
	m[0] = (float)(2.0 * n / (r - l));
	m[5] = (float)(2.0 * n / (t - b));
	m[8] = (float)((r + l) / (r - l));
	m[9] = (float)((t + b) / (t - b));
	m[10] = (float)(-(f + n) / (f - n));
	m[11] = -1.0f;
	m[14] = (float)(-2.0 * f * n / (f - n));

	MultTop(m);
}

void APIENTRY
glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
	float m[16];

	memcpy(m, identity, sizeof(m));
	m[12] = x;
	m[13] = y;
	m[14] = z;

	MultTop(m);
}

void APIENTRY
glScalef(GLfloat x, GLfloat y, GLfloat z)
{
	float m[16];

	memcpy(m, identity, sizeof(m));
	m[0] = x;
	m[5] = y;
	m[10] = z;

	MultTop(m);
}

void APIENTRY
glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
	float rad = angle * (float)(M_PI / 180.0);
	float c = cosf(rad), s = sinf(rad), ic;
	float len = sqrtf(x * x + y * y + z * z);
	float m[16];

	if (len < 1e-6f)
	{
		return;
	}

	x /= len;
	y /= len;
	z /= len;
	ic = 1.0f - c;

	m[0] = x * x * ic + c;     m[4] = x * y * ic - z * s; m[8] = x * z * ic + y * s;  m[12] = 0;
	m[1] = y * x * ic + z * s; m[5] = y * y * ic + c;     m[9] = y * z * ic - x * s;  m[13] = 0;
	m[2] = z * x * ic - y * s; m[6] = z * y * ic + x * s; m[10] = z * z * ic + c;     m[14] = 0;
	m[3] = 0;                  m[7] = 0;                  m[11] = 0;                  m[15] = 1;

	MultTop(m);
}

void APIENTRY
glGetFloatv(GLenum pname, GLfloat *params)
{
	if (!params)
	{
		return;
	}

	switch (pname)
	{
		case GL_MODELVIEW_MATRIX:
			memcpy(params, ps3gl.mv.stack[ps3gl.mv.depth], 16 * sizeof(float));
			break;
		case GL_PROJECTION_MATRIX:
			memcpy(params, ps3gl.proj.stack[ps3gl.proj.depth], 16 * sizeof(float));
			break;
		case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT:
			params[0] = 1.0f;
			break;
		default:
			params[0] = 0.0f;
			break;
	}
}
