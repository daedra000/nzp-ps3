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
 * ps3gl: client arrays, immediate mode, drawing and the shaders.
 *
 * Every draw copies the vertices it uses into this frame's segment of the
 * vertex ring (36 byte interleaved vertices, IoQuake3-PS3's format) and
 * draws from there: TyrQuake's arrays are in ordinary memory, which the
 * RSX can't read (only the IO mapped window). Indices go inline in the
 * command buffer (rsxDrawInlineIndexArray16), like IoQuake3-PS3.
 *
 * The fragment program follows from the texture units (GL's texture
 * environment, the way TyrQuake uses it):
 *
 *   unit 0   off / GL_REPLACE / GL_MODULATE / GL_COMBINE x2 (models)
 *   unit 1   off / GL_MODULATE / GL_COMBINE x2 x4 (lightmaps) / GL_DECAL
 *            (fullbright mask on models, sky layers)
 *   unit 2   off / GL_DECAL (fullbright mask on the world), sampled with
 *            unit 0's coordinates: TyrQuake gives both the same ones.
 *
 * One generated program per combination (shaders/gen_fp.py).
 *
 * =======================================================================
 */

#include <stdio.h>
#include <string.h>

#include "ps3gl.h"
#include "../ps3_video.h"
#include "ps3gl_shader_data.h"
#include "../ps3_platform.h"

static rsxVertexProgram *vp;
static void *vp_ucode;
static u32 vp_ucode_size;
static rsxProgramConst *vp_mvp;
static rsxProgramConst *vp_fogparams, *vp_fogcolor, *vp_texmat_s, *vp_texmat_t;

static struct {
	rsxFragmentProgram *fp;
	void *ucode;           /* copy in RSX memory */
	u32 size;
	u32 offset;
} fps[PS3GL_FP_COUNT];

/* ================================================================ */
/* Shaders                                                            */
/* ================================================================ */

static void
LoadFP(int slot, const unsigned char *data)
{
	void *ucode;

	fps[slot].fp = (rsxFragmentProgram *)data;
	rsxFragmentProgramGetUCode(fps[slot].fp, &ucode, &fps[slot].size);

	/* the fragment program's code must be in RSX memory */
	fps[slot].ucode = rsxMemalign(64, fps[slot].size);

	if (!fps[slot].ucode)
	{
		PS3_Log("[gl] FATAL: rsxMemalign failed for fragment program %d", slot);
		return;
	}

	memcpy(fps[slot].ucode, ucode, fps[slot].size);
	rsxAddressToOffset(fps[slot].ucode, &fps[slot].offset);
}

void
ps3gl_shaders_init(void)
{
	int i;

	vp = (rsxVertexProgram *)nzp_vp_vpo;
	rsxVertexProgramGetUCode(vp, &vp_ucode, &vp_ucode_size);
	vp_mvp = rsxVertexProgramGetConst(vp, "mvp");
	vp_fogparams = rsxVertexProgramGetConst(vp, "fogparams");
	vp_fogcolor = rsxVertexProgramGetConst(vp, "fogcolor");
	vp_texmat_s = rsxVertexProgramGetConst(vp, "texmat_s");
	vp_texmat_t = rsxVertexProgramGetConst(vp, "texmat_t");
	if (!vp_fogparams || !vp_fogcolor || !vp_texmat_s || !vp_texmat_t)
		PS3_Log("[gl] WARNING: nzp_vp is missing its fog/texture matrix constants");

	for (i = 0; i < PS3GL_FP_COUNT && i < PS3GL_NUM_FP; i++)
	{
		LoadFP(i, ps3gl_fp_data[i]);
	}

	PS3_Log("[gl] shaders: vertex program %u bytes (mvp %s), %d fragment programs",
			(unsigned)vp_ucode_size, vp_mvp ? "ok" : "MISSING", PS3GL_NUM_FP);
}

void
ps3gl_apply_shader(int fp)
{
	gcmContextData *ctx = ps3gl.ctx;

	if (!ps3gl.vp_loaded)
	{
		rsxLoadVertexProgram(ctx, vp, vp_ucode);
		ps3gl.vp_loaded = 1;
		ps3gl.mvp_uploaded = 0;
		ps3gl.fog_uploaded = 0;
		ps3gl.texmat_uploaded = 0;
	}

	if (fp != ps3gl.active_fp && fps[fp].ucode)
	{
		rsxLoadFragmentProgramLocation(ctx, fps[fp].fp, fps[fp].offset, GCM_LOCATION_RSX);
		ps3gl.active_fp = fp;
	}

	if (!ps3gl.mvp_uploaded && vp_mvp)
	{
		rsxSetVertexProgramParameter(ctx, vp, vp_mvp, ps3gl.mvp);
		ps3gl.mvp_uploaded = 1;
	}

	/* GL_FOG (linear): (end, 1 / (end - start), on, 0); factor 1 when off */
	if (!ps3gl.fog_uploaded && vp_fogparams && vp_fogcolor)
	{
		float p[4];
		float range = ps3gl.fog.end - ps3gl.fog.start;

		p[0] = ps3gl.fog.end;
		p[1] = (range > 1e-4f || range < -1e-4f) ? 1.0f / range : 0.0f;
		p[2] = ps3gl.fog.enabled ? 1.0f : 0.0f;
		p[3] = 0.0f;
		rsxSetVertexProgramParameter(ctx, vp, vp_fogparams, p);
		rsxSetVertexProgramParameter(ctx, vp, vp_fogcolor, ps3gl.fog.color);
		ps3gl.fog_uploaded = 1;
	}

	if (!ps3gl.texmat_uploaded && vp_texmat_s && vp_texmat_t)
	{
		rsxSetVertexProgramParameter(ctx, vp, vp_texmat_s, ps3gl.texmat);
		rsxSetVertexProgramParameter(ctx, vp, vp_texmat_t, ps3gl.texmat + 4);
		ps3gl.texmat_uploaded = 1;
	}
}

static int
UnitHasTexture(int i)
{
	const ps3gl_tmu_t *tmu = &ps3gl.tmu[i];

	return tmu->enabled && ps3gl.textures[tmu->tex].data &&
		ps3gl.textures[tmu->tex].num_levels > 0;
}

/* Fragment program for the current texture units; *units = the samplers
   it uses (bit i = unit i). */
static int
ChooseFP(unsigned *units)
{
	int u0 = PS3GL_U0_OFF, u1 = PS3GL_U1_OFF, u2 = PS3GL_U2_OFF;
	unsigned mask = 0;

	if (UnitHasTexture(0))
	{
		const ps3gl_tmu_t *t = &ps3gl.tmu[0];

		mask |= 1;

		if (t->env == GL_REPLACE)
		{
			u0 = PS3GL_U0_REPLACE;
		}
		else if (t->env == GL_COMBINE && t->rgb_scale >= 2)
		{
			u0 = PS3GL_U0_MODULATE2X;
		}
		else
		{
			u0 = PS3GL_U0_MODULATE;
		}
	}

	if (UnitHasTexture(1))
	{
		const ps3gl_tmu_t *t = &ps3gl.tmu[1];

		mask |= 2;

		if (t->env == GL_DECAL)
		{
			u1 = PS3GL_U1_DECAL;
		}
		else if (t->env == GL_COMBINE && t->rgb_scale >= 4)
		{
			u1 = PS3GL_U1_MODULATE4X;
		}
		else if (t->env == GL_COMBINE && t->rgb_scale >= 2)
		{
			u1 = PS3GL_U1_MODULATE2X;
		}
		else
		{
			u1 = PS3GL_U1_MODULATE;
		}
	}

	if (UnitHasTexture(2))
	{
		/* only GL_DECAL (fullbright masks) is done on unit 2 */
		mask |= 4;
		u2 = PS3GL_U2_DECAL;
	}

	*units = mask;

	return PS3GL_FP_INDEX(u0, u1, u2);
}

/* ================================================================ */
/* Client arrays                                                      */
/* ================================================================ */

static ps3gl_array_t *
ArrayFor(GLenum array)
{
	switch (array)
	{
		case GL_VERTEX_ARRAY:        return &ps3gl.va_vertex;
		case GL_COLOR_ARRAY:         return &ps3gl.va_color;
		case GL_TEXTURE_COORD_ARRAY: return &ps3gl.va_texcoord[ps3gl.client_tmu];
		default:                     return NULL;   /* GL_NORMAL_ARRAY... */
	}
}

void APIENTRY
glEnableClientState(GLenum array)
{
	ps3gl_array_t *a = ArrayFor(array);

	if (a)
	{
		a->enabled = 1;
	}
}

void APIENTRY
glDisableClientState(GLenum array)
{
	ps3gl_array_t *a = ArrayFor(array);

	if (a)
	{
		a->enabled = 0;
	}
}

static void
SetArray(ps3gl_array_t *a, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
	a->size = size;
	a->type = type;
	a->stride = stride;
	a->ptr = ptr;
}

void APIENTRY
glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
	SetArray(&ps3gl.va_vertex, size, type, stride, ptr);
}

void APIENTRY
glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
	SetArray(&ps3gl.va_texcoord[ps3gl.client_tmu], size, type, stride, ptr);
}

void APIENTRY
glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
	SetArray(&ps3gl.va_color, size, type, stride, ptr);
}

/* Normals are only used by the ARB vertex programs, which ps3gl doesn't
   offer (the models are lit on the CPU, with color arrays). */
void APIENTRY
glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
	(void)type;
	(void)stride;
	(void)ptr;
}

static int
TypeSize(GLenum type)
{
	switch (type)
	{
		case GL_BYTE:
		case GL_UNSIGNED_BYTE:  return 1;
		case GL_SHORT:
		case GL_UNSIGNED_SHORT: return 2;
		case GL_DOUBLE:         return 8;
		default:                return 4;
	}
}

static int
Stride(const ps3gl_array_t *a)
{
	return a->stride ? a->stride : a->size * TypeSize(a->type);
}

/* Component c of element i as a float (0 if missing) */
static inline float
Fetch(const ps3gl_array_t *a, const uint8_t *e, int c)
{
	if (c >= a->size)
	{
		return (c == 3) ? 1.0f : 0.0f;
	}

	switch (a->type)
	{
		case GL_FLOAT:  return ((const float *)e)[c];
		case GL_SHORT:  return (float)((const int16_t *)e)[c];
		case GL_INT:    return (float)((const int32_t *)e)[c];
		case GL_DOUBLE: return (float)((const double *)e)[c];
		default:        return 0.0f;
	}
}

static inline uint32_t
FetchColor(const ps3gl_array_t *a, const uint8_t *e)
{
	if (a->type == GL_UNSIGNED_BYTE)
	{
		/* bytes R G B A, the vertex format's order */
		if (a->size == 4)
		{
			return ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) |
				((uint32_t)e[2] << 8) | e[3];
		}

		return ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | 0xff;
	}

	if (a->type == GL_FLOAT)
	{
		const float *f = (const float *)e;
		uint32_t c = 0;
		int i;

		for (i = 0; i < 4; i++)
		{
			float v = (i < a->size) ? f[i] : 1.0f;
			uint32_t b = v <= 0.0f ? 0 : v >= 1.0f ? 255 : (uint32_t)(v * 255.0f + 0.5f);

			c |= b << (24 - 8 * i);
		}

		return c;
	}

	return 0xffffffffu;
}

/* Room for 'bytes' in this frame's ring segment: CPU pointer and RSX
   offset. 0 if the segment is full (the draw is dropped). */
static int
RingAlloc(uint32_t bytes, ps3gl_vertex_t **out, uint32_t *offset)
{
	uint32_t seg_base = ps3gl.ring_seg * PS3GL_RING_SEG_SIZE;

	if (ps3gl.ring_head + bytes > PS3GL_RING_SEG_SIZE)
	{
		static int warned;

		if (!warned)
		{
			PS3_Log("[gl] WARNING: vertex ring segment full (%u + %u > %u), draws dropped",
					(unsigned)ps3gl.ring_head, (unsigned)bytes, PS3GL_RING_SEG_SIZE);
			warned = 1;
		}

		ps3gl.st_dropped++;
		return 0;
	}

	*out = (ps3gl_vertex_t *)(ps3gl.ring + seg_base + ps3gl.ring_head);
	*offset = ps3gl.ring_off + seg_base + ps3gl.ring_head;

	/* Every block starts 16 byte aligned: otherwise the attribute offsets
	   drift and the RSX's vertex fetch hangs the console (IoQuake3-PS3's
	   note). */
	ps3gl.ring_head = (ps3gl.ring_head + bytes + 15u) & ~15u;

	return 1;
}

/* Copies vertices first..first+count-1 into the ring and gives the RSX
   offset of the copy. 0 if the ring segment is full. Texture coordinates
   are only read for the units the fragment program samples: an array
   left enabled on an unused unit may point at anything. */
static int
CopyVertices(int first, int count, unsigned units, uint32_t *ring_offset)
{
	const ps3gl_array_t *va = &ps3gl.va_vertex;
	const ps3gl_array_t *vc = ps3gl.va_color.enabled ? &ps3gl.va_color : NULL;
	/* unit 2 samples with unit 0's coordinates */
	const ps3gl_array_t *vt0 = ((units & 5) && ps3gl.va_texcoord[0].enabled) ?
		&ps3gl.va_texcoord[0] : NULL;
	const ps3gl_array_t *vt1 = ((units & 2) && ps3gl.va_texcoord[1].enabled) ?
		&ps3gl.va_texcoord[1] : NULL;
	uint32_t bytes = (uint32_t)count * PS3GL_VERTEX_SIZE;
	const uint8_t *pv, *pc = NULL, *pt0 = NULL, *pt1 = NULL;
	int sv, sc = 0, st0 = 0, st1 = 0;
	ps3gl_vertex_t *out;
	uint32_t color = ps3gl.color;
	int fast, i;

	if (!RingAlloc(bytes, &out, ring_offset))
	{
		return 0;
	}

	sv = Stride(va);
	pv = (const uint8_t *)va->ptr + (size_t)first * sv;

	if (vc && vc->ptr)
	{
		sc = Stride(vc);
		pc = (const uint8_t *)vc->ptr + (size_t)first * sc;
	}

	if (vt0 && vt0->ptr)
	{
		st0 = Stride(vt0);
		pt0 = (const uint8_t *)vt0->ptr + (size_t)first * st0;
	}

	if (vt1 && vt1->ptr)
	{
		st1 = Stride(vt1);
		pt1 = (const uint8_t *)vt1->ptr + (size_t)first * st1;
	}

	/* the usual case: float positions, float texcoords, byte colors */
	fast = (va->type == GL_FLOAT) &&
		(!pt0 || (vt0->type == GL_FLOAT && vt0->size >= 2)) &&
		(!pt1 || (vt1->type == GL_FLOAT && vt1->size >= 2)) &&
		(!pc || (vc->type == GL_UNSIGNED_BYTE && vc->size == 4));

	for (i = 0; i < count; i++, out++)
	{
		if (fast)
		{
			const float *p = (const float *)pv;

			out->x = p[0];
			out->y = p[1];
			out->z = (va->size > 2) ? p[2] : 0.0f;
			out->w = 1.0f;

			if (pt0)
			{
				out->u0 = ((const float *)pt0)[0];
				out->v0 = ((const float *)pt0)[1];
				pt0 += st0;
			}
			else
			{
				out->u0 = out->v0 = 0.0f;
			}

			if (pt1)
			{
				out->u1 = ((const float *)pt1)[0];
				out->v1 = ((const float *)pt1)[1];
				pt1 += st1;
			}
			else
			{
				out->u1 = out->v1 = 0.0f;
			}

			if (pc)
			{
				out->color = ((uint32_t)pc[0] << 24) | ((uint32_t)pc[1] << 16) |
					((uint32_t)pc[2] << 8) | pc[3];
				pc += sc;
			}
			else
			{
				out->color = color;
			}
		}
		else
		{
			out->x = Fetch(va, pv, 0);
			out->y = Fetch(va, pv, 1);
			out->z = Fetch(va, pv, 2);
			out->w = 1.0f;

			if (pt0)
			{
				out->u0 = Fetch(vt0, pt0, 0);
				out->v0 = Fetch(vt0, pt0, 1);
				pt0 += st0;
			}
			else
			{
				out->u0 = out->v0 = 0.0f;
			}

			if (pt1)
			{
				out->u1 = Fetch(vt1, pt1, 0);
				out->v1 = Fetch(vt1, pt1, 1);
				pt1 += st1;
			}
			else
			{
				out->u1 = out->v1 = 0.0f;
			}

			if (pc)
			{
				out->color = FetchColor(vc, pc);
				pc += sc;
			}
			else
			{
				out->color = color;
			}
		}

		pv += sv;
	}

	return 1;
}

static void
BindRing(uint32_t off)
{
	gcmContextData *ctx = ps3gl.ctx;

	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, off + PS3GL_VATTR_POS_OFF,
			PS3GL_VERTEX_SIZE, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, off + PS3GL_VATTR_TC0_OFF,
			PS3GL_VERTEX_SIZE, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, off + PS3GL_VATTR_TC1_OFF,
			PS3GL_VERTEX_SIZE, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, off + PS3GL_VATTR_COLOR_OFF,
			PS3GL_VERTEX_SIZE, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
}

static int
Prim(GLenum mode, uint32_t *prim)
{
	switch (mode)
	{
		case GL_POINTS:         *prim = GCM_TYPE_POINTS; return 1;
		case GL_LINES:          *prim = GCM_TYPE_LINES; return 1;
		case GL_LINE_LOOP:      *prim = GCM_TYPE_LINE_LOOP; return 1;
		case GL_LINE_STRIP:     *prim = GCM_TYPE_LINE_STRIP; return 1;
		case GL_TRIANGLES:      *prim = GCM_TYPE_TRIANGLES; return 1;
		case GL_TRIANGLE_STRIP: *prim = GCM_TYPE_TRIANGLE_STRIP; return 1;
		case GL_TRIANGLE_FAN:   *prim = GCM_TYPE_TRIANGLE_FAN; return 1;
		case GL_QUADS:          *prim = GCM_TYPE_QUADS; return 1;
		case GL_QUAD_STRIP:     *prim = GCM_TYPE_QUAD_STRIP; return 1;
		case GL_POLYGON:        *prim = GCM_TYPE_POLYGON; return 1;
		default:                return 0;
	}
}

/* Bring-up trace: what each draw of the last frames sent, so a frame the RSX
   never finishes (a GPU hang) can be listed from WaitForFrame. */
#define TRACE_FRAMES 4
#define TRACE_DRAWS  192

typedef struct {
	uint16_t mode, fp;
	uint32_t count;
	uint8_t  units;
	uint8_t  blend, alpha, depth;
	struct { int16_t name; uint16_t w, h; uint8_t swz, levels; } tex[PS3GL_MAX_TMUS];
	uint32_t area;   /* screen pixels its bounding box covers (capture frames only) */
} trace_draw_t;

static struct {
	uint32_t frame;
	int n;
	trace_draw_t d[TRACE_DRAWS];
} trace[TRACE_FRAMES];

static int last_fp;

/* A GPU-heavy frame (the RSX made the CPU wait for a free framebuffer) is
   captured: every draw's screen area from its vertices, the biggest listed.
   Reading the ring back from RSX memory is slow: only that one frame, and
   at most 48 vertices of each draw. */
static uint32_t capture_frame;
static int captures;

void
ps3gl_request_capture(void)
{
	if (captures < 4 && !capture_frame)
	{
		capture_frame = ps3gl.frame + 1;
	}
}

static uint32_t
DrawArea(const ps3gl_vertex_t *v, int n)
{
	const float *m = ps3gl.mvp;
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, sw, sh;
	int i, step = n > 48 ? n / 48 : 1;

	for (i = 0; i < n; i += step)
	{
		float x = v[i].x, y = v[i].y, z = v[i].z;
		float cw = m[12] * x + m[13] * y + m[14] * z + m[15];
		float cx, cy;

		if (cw <= 0.001f)
		{
			return (uint32_t)(ps3gl.screen_w * ps3gl.screen_h);   /* through the camera: call it all */
		}
		cx = (m[0] * x + m[1] * y + m[2] * z + m[3]) / cw;
		cy = (m[4] * x + m[5] * y + m[6] * z + m[7]) / cw;
		if (cx < x0) x0 = cx;
		if (cx > x1) x1 = cx;
		if (cy < y0) y0 = cy;
		if (cy > y1) y1 = cy;
	}
	if (x0 < -1.0f) x0 = -1.0f;
	if (y0 < -1.0f) y0 = -1.0f;
	if (x1 > 1.0f) x1 = 1.0f;
	if (y1 > 1.0f) y1 = 1.0f;
	if (x1 <= x0 || y1 <= y0)
	{
		return 0;
	}
	sw = (x1 - x0) * 0.5f * ps3gl.screen_w;
	sh = (y1 - y0) * 0.5f * ps3gl.screen_h;
	return (uint32_t)(sw * sh);
}

static void TraceDraw(GLenum mode, int count, unsigned units);

/* GPU time per draw: in a captured frame the RSX writes its clock into
   report TS_BASE+i before draw i (and TS_BASE+n after the last one); the
   reports are read once that frame is done. */
#define TS_BASE 256
#define TS_MAX  (TRACE_DRAWS < 400 ? TRACE_DRAWS : 400)
static uint32_t ts_pending_frame;
static int ts_count;

static void
TraceDrawAt(GLenum mode, int count, unsigned units, uint32_t ring_off, int nverts)
{
	TraceDraw(mode, count, units);
	if (capture_frame == ps3gl.frame)
	{
		int slot = ps3gl.frame % TRACE_FRAMES;
		int idx = trace[slot].n - 1;
		if (idx < TRACE_DRAWS)
		{
			const ps3gl_vertex_t *v = (const ps3gl_vertex_t *)(ps3gl.ring + (ring_off - ps3gl.ring_off));
			trace[slot].d[idx].area = DrawArea(v, nverts);
		}
		if (idx < TS_MAX)
			rsxSetTimeStamp(ps3gl.ctx, TS_BASE + idx);
	}
}

/* Called at each frame start: the captured frame's GPU times, once the RSX
   has finished it. */
void
ps3gl_capture_report(void)
{
	int slot, i, n, top[12], ntop = 0, j;
	uint64_t t[TS_MAX + 1], total;

	if (!ts_pending_frame || (int)(ps3gl_completed_frame() - ts_pending_frame) < 0)
	{
		return;
	}
	slot = ts_pending_frame % TRACE_FRAMES;
	n = ts_count;
	if (trace[slot].frame != ts_pending_frame || n <= 0)
	{
		ts_pending_frame = 0;   /* its draw list is gone: too late */
		return;
	}
	ts_pending_frame = 0;
	for (i = 0; i <= n; i++)
		t[i] = gcmGetTimeStamp(TS_BASE + i);
	total = t[n] - t[0];
	for (i = 0; i < n; i++)
	{
		uint64_t d = t[i + 1] - t[i];
		for (j = ntop; j > 0 && (t[top[j - 1] + 1] - t[top[j - 1]]) < d; j--)
			if (j < 12) top[j] = top[j - 1];
		if (j < 12) { top[j] = i; if (ntop < 12) ntop++; }
	}
	PS3_Log("[gpu] RSX time for those %d draws: %.2f ms; slowest:", n, total / 1e6);
	for (i = 0; i < ntop; i++)
	{
		const trace_draw_t *d = &trace[slot].d[top[i]];
		PS3_Log("[gpu]  #%d %.2f ms, mode 0x%x count %u fp %u blend %d alpha %d depth %d t0 %d %ux%u lv %d",
				top[i], (t[top[i] + 1] - t[top[i]]) / 1e6, d->mode, (unsigned)d->count, d->fp,
				d->blend, d->alpha, d->depth, d->tex[0].name, d->tex[0].w, d->tex[0].h, d->tex[0].levels);
	}
}

/* At the end of a captured frame: total coverage and the biggest draws. */
void
ps3gl_capture_end(void)
{
	int slot, i, j, n, top[12], ntop = 0;
	double total = 0, screen;

	if (capture_frame != ps3gl.frame)
	{
		return;
	}
	capture_frame = 0;
	captures++;
	slot = ps3gl.frame % TRACE_FRAMES;
	n = trace[slot].n < TRACE_DRAWS ? trace[slot].n : TRACE_DRAWS;
	/* the end mark for the last draw's GPU time; read when the frame is done */
	ts_count = n < TS_MAX ? n : TS_MAX;
	PS3_Cmd_Reserve();   /* librsx must never wrap the buffer itself (ps3_video.c) */
	rsxSetTimeStamp(ps3gl.ctx, TS_BASE + ts_count);
	ts_pending_frame = ps3gl.frame;
	screen = (double)ps3gl.screen_w * ps3gl.screen_h;
	for (i = 0; i < n; i++)
	{
		total += trace[slot].d[i].area;
		/* insertion into the top 12 by area */
		for (j = ntop; j > 0 && trace[slot].d[top[j - 1]].area < trace[slot].d[i].area; j--)
			if (j < 12) top[j] = top[j - 1];
		if (j < 12) { top[j] = i; if (ntop < 12) ntop++; }
	}
	PS3_Log("[gpu] heavy frame %u: %d draws (%d traced), pixels drawn %.1f screens; biggest:",
			(unsigned)ps3gl.frame, trace[slot].n, n, total / screen);
	for (i = 0; i < ntop; i++)
	{
		const trace_draw_t *d = &trace[slot].d[top[i]];
		PS3_Log("[gpu]  #%d %.2f screens, mode 0x%x count %u fp %u blend %d alpha %d depth %d t0 %d %ux%u lv %d",
				top[i], d->area / screen, d->mode, (unsigned)d->count, d->fp, d->blend, d->alpha, d->depth,
				d->tex[0].name, d->tex[0].w, d->tex[0].h, d->tex[0].levels);
	}
}

static void
TraceDraw(GLenum mode, int count, unsigned units)
{
	int slot = ps3gl.frame % TRACE_FRAMES, i;
	trace_draw_t *d;

	if (trace[slot].frame != ps3gl.frame)
	{
		trace[slot].frame = ps3gl.frame;
		trace[slot].n = 0;
	}
	if (trace[slot].n >= TRACE_DRAWS)
	{
		trace[slot].n++;
		return;
	}
	d = &trace[slot].d[trace[slot].n++];
	d->mode = (uint16_t)mode;
	d->fp = (uint16_t)last_fp;
	d->count = (uint32_t)count;
	d->area = 0;
	d->units = (uint8_t)units;
	d->blend = (uint8_t)ps3gl.rs.blend;
	d->alpha = (uint8_t)ps3gl.rs.alpha_test;
	d->depth = (uint8_t)ps3gl.rs.depth_test;
	for (i = 0; i < PS3GL_MAX_TMUS; i++)
	{
		const ps3gl_texture_t *t = &ps3gl.textures[ps3gl.tmu[i].tex];
		d->tex[i].name = (int16_t)((units & (1u << i)) ? ps3gl.tmu[i].tex : -1);
		d->tex[i].w = t->width;
		d->tex[i].h = t->height;
		d->tex[i].swz = t->swizzled;
		d->tex[i].levels = t->num_levels;
	}
}

void
ps3gl_trace_dump(uint32_t frame)
{
	static int dumped;
	int slot = frame % TRACE_FRAMES, i, n;

	if (dumped++ >= 2 || trace[slot].frame != frame)
	{
		return;
	}
	n = trace[slot].n;
	PS3_Log("[gl] frame %u: %d draws (the RSX never finished it)", (unsigned)frame, n);
	for (i = 0; i < n && i < TRACE_DRAWS; i++)
	{
		const trace_draw_t *d = &trace[slot].d[i];
		PS3_Log("[gl]  #%d mode 0x%x count %u fp %u units %u blend %d alpha %d depth %d"
				" t0 %d %ux%u swz %d lv %d t1 %d %ux%u",
				i, d->mode, (unsigned)d->count, d->fp, d->units, d->blend, d->alpha, d->depth,
				d->tex[0].name, d->tex[0].w, d->tex[0].h, d->tex[0].swz, d->tex[0].levels,
				d->tex[1].name, d->tex[1].w, d->tex[1].h);
	}
}

/* State, matrices, textures and shader to the RSX. Gives the samplers in
   use. */
static void
PrepareState(unsigned *units_out)
{
	gcmContextData *ctx = ps3gl.ctx;
	unsigned units;
	int fp;

	PS3_Cmd_Reserve();

	/* A copy into a texture (glCopyTexSubImage2D) must be finished, and
	   out of the texture cache, before anything samples it. */
	if (ps3gl.copy_pending)
	{
		rsxSetWaitForIdle(ctx);
		rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
		ps3gl.copy_pending = 0;
	}

	fp = ChooseFP(&units);
	last_fp = fp;

	ps3gl_apply_states();
	ps3gl_apply_matrices();
	ps3gl_apply_textures(units);
	ps3gl_apply_shader(fp);

	ps3gl.drew = 1;
	*units_out = units;
}

static int
PrepareDraw(unsigned *units_out)
{
	if (!ps3gl.ctx || !ps3gl.ring || !ps3gl.va_vertex.enabled || !ps3gl.va_vertex.ptr)
	{
		return 0;
	}

	PrepareState(units_out);

	return 1;
}

void APIENTRY
glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	uint32_t prim, off;
	unsigned units;

	if (count <= 0 || first < 0 || !Prim(mode, &prim) || !PrepareDraw(&units))
	{
		return;
	}

	if (!CopyVertices(first, count, units, &off))
	{
		return;
	}

	BindRing(off);
	TraceDrawAt(mode, count, units, off, count);
	rsxDrawVertexArray(ps3gl.ctx, prim, 0, (u32)count);

	ps3gl.st_draws++;
	ps3gl.st_verts += (uint32_t)count;
}

void APIENTRY
glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	static uint16_t idx16[65536];
	uint32_t prim, off;
	unsigned units;
	int i, lo = 0x7fffffff, hi = -1;

	if (count <= 0 || !indices || !Prim(mode, &prim) || !PrepareDraw(&units))
	{
		return;
	}

	if (count > 65536)
	{
		static int warned;

		if (!warned)
		{
			PS3_Log("[gl] WARNING: glDrawElements with %d indices, cut to 65536", count);
			warned = 1;
		}

		count = 65536;
	}

	/* the range of vertices used, and the indices as 16 bits */
	for (i = 0; i < count; i++)
	{
		int v;

		switch (type)
		{
			case GL_UNSIGNED_BYTE: v = ((const uint8_t *)indices)[i]; break;
			case GL_UNSIGNED_INT:  v = (int)((const uint32_t *)indices)[i]; break;
			default:               v = ((const uint16_t *)indices)[i]; break;
		}

		if (v < lo) lo = v;
		if (v > hi) hi = v;

		idx16[i] = (uint16_t)v;
	}

	if (hi - lo >= 65536)
	{
		return;
	}

	if (!CopyVertices(lo, hi - lo + 1, units, &off))
	{
		return;
	}

	if (lo)
	{
		for (i = 0; i < count; i++)
		{
			idx16[i] = (uint16_t)(idx16[i] - lo);
		}
	}

	BindRing(off);
	TraceDrawAt(mode, count, units, off, hi - lo + 1);
	rsxDrawInlineIndexArray16(ps3gl.ctx, prim, 0, (u32)count, idx16);

	ps3gl.st_draws++;
	ps3gl.st_verts += (uint32_t)(hi - lo + 1);
}

/* GL 1.2: the range is a hint, glDrawElements finds it anyway. */
void APIENTRY
glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
		const GLvoid *indices)
{
	(void)start;
	(void)end;
	glDrawElements(mode, count, type, indices);
}

/* ================================================================ */
/* Immediate mode                                                     */
/* ================================================================ */

/*
 * glBegin .. glEnd collect the vertices here, glEnd draws them. The
 * current texture coordinate goes to both units (GL keeps one per unit,
 * TyrQuake only sets unit 0's in immediate mode). Quads and polygons are
 * drawn as triangles / fans: Quake 2's renderer proved those on hardware.
 */

#define IM_MAX 4096

static ps3gl_vertex_t im_buf[IM_MAX];
static int im_count;
static uint16_t im_idx[IM_MAX / 4 * 6];

void APIENTRY
glBegin(GLenum mode)
{
	ps3gl.in_begin = 1;
	ps3gl.im_mode = mode;
	im_count = 0;
}

/* Draws the collected vertices (count of them) */
static void
ImFlush(int count)
{
	ps3gl_vertex_t *out;
	uint32_t prim, off;
	unsigned units;
	int nidx = 0, i;

	if (count <= 0 || !ps3gl.ctx || !ps3gl.ring)
	{
		return;
	}

	switch (ps3gl.im_mode)
	{
		case GL_QUADS:
			count -= count % 4;

			for (i = 0; i + 3 < count; i += 4)
			{
				im_idx[nidx++] = (uint16_t)i;
				im_idx[nidx++] = (uint16_t)(i + 1);
				im_idx[nidx++] = (uint16_t)(i + 2);
				im_idx[nidx++] = (uint16_t)i;
				im_idx[nidx++] = (uint16_t)(i + 2);
				im_idx[nidx++] = (uint16_t)(i + 3);
			}

			prim = GCM_TYPE_TRIANGLES;
			break;
		case GL_POLYGON:
		case GL_TRIANGLE_FAN:
			prim = GCM_TYPE_TRIANGLE_FAN;
			break;
		case GL_QUAD_STRIP:
		case GL_TRIANGLE_STRIP:
			prim = GCM_TYPE_TRIANGLE_STRIP;
			break;
		case GL_TRIANGLES:
			count -= count % 3;
			prim = GCM_TYPE_TRIANGLES;
			break;
		default:
			if (!Prim(ps3gl.im_mode, &prim))
			{
				return;
			}
			break;
	}

	if (count <= 0)
	{
		return;
	}

	PrepareState(&units);

	if (!RingAlloc((uint32_t)count * PS3GL_VERTEX_SIZE, &out, &off))
	{
		return;
	}

	memcpy(out, im_buf, (size_t)count * PS3GL_VERTEX_SIZE);

	BindRing(off);
	TraceDrawAt(ps3gl.im_mode, count, units, off, count);

	if (nidx)
	{
		rsxDrawInlineIndexArray16(ps3gl.ctx, prim, 0, (u32)nidx, im_idx);
	}
	else
	{
		rsxDrawVertexArray(ps3gl.ctx, prim, 0, (u32)count);
	}

	ps3gl.st_draws++;
	ps3gl.st_verts += (uint32_t)count;
}

static void
ImVertex(float x, float y, float z)
{
	ps3gl_vertex_t *v;

	if (!ps3gl.in_begin)
	{
		return;
	}

	if (im_count >= IM_MAX)
	{
		/* Lists of separate primitives can be drawn in pieces; strips,
		   fans and polygons that long are cut. */
		if (ps3gl.im_mode == GL_TRIANGLES || ps3gl.im_mode == GL_QUADS ||
			ps3gl.im_mode == GL_LINES || ps3gl.im_mode == GL_POINTS)
		{
			ImFlush(im_count);
			im_count = 0;
		}
		else
		{
			static int warned;

			if (!warned)
			{
				PS3_Log("[gl] WARNING: glBegin/glEnd with more than %d vertices, cut", IM_MAX);
				warned = 1;
			}

			return;
		}
	}

	v = &im_buf[im_count++];
	v->x = x;
	v->y = y;
	v->z = z;
	v->w = 1.0f;
	v->u0 = v->u1 = ps3gl.im_tc[0];
	v->v0 = v->v1 = ps3gl.im_tc[1];
	v->color = ps3gl.color;
}

void APIENTRY
glEnd(void)
{
	if (!ps3gl.in_begin)
	{
		return;
	}

	ImFlush(im_count);
	im_count = 0;
	ps3gl.in_begin = 0;
}

void APIENTRY glVertex2f(GLfloat x, GLfloat y) { ImVertex(x, y, 0.0f); }
void APIENTRY glVertex2i(GLint x, GLint y) { ImVertex((float)x, (float)y, 0.0f); }
void APIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z) { ImVertex(x, y, z); }
void APIENTRY glVertex3fv(const GLfloat *v) { ImVertex(v[0], v[1], v[2]); }

void APIENTRY
glTexCoord2f(GLfloat s, GLfloat t)
{
	ps3gl.im_tc[0] = s;
	ps3gl.im_tc[1] = t;
}

void APIENTRY
glTexCoord2fv(const GLfloat *v)
{
	ps3gl.im_tc[0] = v[0];
	ps3gl.im_tc[1] = v[1];
}
