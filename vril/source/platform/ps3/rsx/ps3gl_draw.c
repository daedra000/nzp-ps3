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
 * command buffer, in pieces that fit the room made for them.
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
#include <stdlib.h>
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

/* Indexed draw, the indices inline in the command buffer (as TyrQuakeCell
   and IoQuake3-PS3) but in pieces that always fit the room made for them
   first: a list of a few thousand indices is longer than the room kept free
   (PS3_Cmd_Reserve), and librsx wrapping the command buffer in the middle of
   it corrupts it (the RSX hung reading indices as commands at the stairs
   light). Triangle lists split on whole triangles; other primitives are
   drawn whole (they are short). */
#define IDX_CHUNK 3000   /* indices: 1500 words */

static void
DrawIndexedInline(uint32_t prim, const uint16_t *idx, int count)
{
	while (count > 0)
	{
		int n = count;

		if (n > IDX_CHUNK && prim == GCM_TYPE_TRIANGLES)
		{
			n = IDX_CHUNK;   /* a multiple of 3 */
		}

		PS3_Cmd_ReserveWords((uint32_t)n / 2 + 64);
		rsxDrawInlineIndexArray16(ps3gl.ctx, prim, 0, (u32)n, idx);
		idx += n;
		count -= n;
	}
}

/* Copies vertices first..first+count-1 into the ring and gives the RSX
   offset of the copy. 0 if the ring segment is full. Texture coordinates
   are only read for the units the fragment program samples: an array
   left enabled on an unused unit may point at anything. */
static int
CopyVertexList(int first, int count, unsigned units, uint32_t *ring_offset, const int *list);

static int
CopyVertices(int first, int count, unsigned units, uint32_t *ring_offset)
{
	return CopyVertexList(first, count, units, ring_offset, NULL);
}

/* As CopyVertices, but with 'list' the vertices list[0..count-1] (first is 0):
   only the vertices an indexed draw uses. */
static int
CopyVertexList(int first, int count, unsigned units, uint32_t *ring_offset, const int *list)
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

	const uint8_t *bv = pv, *bc = pc, *bt0 = pt0, *bt1 = pt1;

	/* the usual case: float positions, float texcoords, byte colors */
	fast = (va->type == GL_FLOAT) &&
		(!pt0 || (vt0->type == GL_FLOAT && vt0->size >= 2)) &&
		(!pt1 || (vt1->type == GL_FLOAT && vt1->size >= 2)) &&
		(!pc || (vc->type == GL_UNSIGNED_BYTE && vc->size == 4));

	for (i = 0; i < count; i++, out++)
	{
		if (list)
		{
			size_t v = (size_t)list[i];
			pv = bv + v * sv;
			if (pc) pc = bc + v * sc;
			if (pt0) pt0 = bt0 + v * st0;
			if (pt1) pt1 = bt1 + v * st1;
		}

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

	if (count <= 0 || first < 0 || !Prim(mode, &prim) ||
		!PrepareDraw(&units))
	{
		return;
	}

	if (!CopyVertices(first, count, units, &off))
	{
		return;
	}

	BindRing(off);
	rsxDrawVertexArray(ps3gl.ctx, prim, 0, (u32)count);

	ps3gl.st_draws++;
	ps3gl.st_verts += (uint32_t)count;
}

/* ================================================================ */
/* The world's vertices in RSX memory                                 */
/* ================================================================ */

/*
 * As Halo CE PS3's vertex cache: the world's vertices never change during a
 * map, but each frame copied every visible one into the ring again, and a
 * detailed custom map (Croft Manor's fireplace room: 30 000-40 000 vertices
 * in view) missed 60 fps on that alone. The renderer hands each page of the
 * world's vertices over once, at the map's load (ps3glWorldPage); they're
 * converted into RSX memory twice (the texture pass's coordinates, and the
 * lightmap pass's) and the draws only send their indices.
 *
 * Only while the current colour is white (what the world draws with; the
 * vertices carry it): any other colour, and the draw goes through the ring
 * as before. A page without room in RSX memory goes through the ring too.
 */
typedef struct
{
	uint8_t *mem[2];   /* 0: texture coordinates, 1: lightmap coordinates */
	uint32_t off[2];
	int count;
} world_page_t;

#define WORLD_MAX_BYTES (48u * 1024 * 1024)

static world_page_t *world_pages;
static int world_npages;
static uint32_t world_bytes;
static int world_cur = -1, world_cur_lm;

void
ps3glWorldPagesFree(void)
{
	int i, j;

	world_cur = -1;
	if (!world_pages)
	{
		return;
	}
	PS3_Video_Finish();   /* the RSX may still read them */
	for (i = 0; i < world_npages; i++)
		for (j = 0; j < 2; j++)
			if (world_pages[i].mem[j])
				rsxFree(world_pages[i].mem[j]);
	free(world_pages);
	world_pages = NULL;
	world_npages = 0;
	world_bytes = 0;
}

/* 'v': count vertices of 'stride' bytes, the coordinates at those offsets */
void
ps3glWorldPage(int page, const void *v, int stride, int uv_off, int lm_off, int xyz_off, int count)
{
	uint32_t bytes = (uint32_t)count * PS3GL_VERTEX_SIZE;
	int j, i;

	if (page < 0 || count <= 0 || count > 65536)
	{
		return;
	}
	if (page >= world_npages)
	{
		world_page_t *n = (world_page_t *)realloc(world_pages, (size_t)(page + 1) * sizeof(*n));
		if (!n)
		{
			return;
		}
		memset(n + world_npages, 0, (size_t)(page + 1 - world_npages) * sizeof(*n));
		world_pages = n;
		world_npages = page + 1;
	}
	world_pages[page].count = count;
	for (j = 0; j < 2; j++)
	{
		ps3gl_vertex_t *out;

		if (world_bytes + bytes > WORLD_MAX_BYTES ||
			!(world_pages[page].mem[j] = (uint8_t *)rsxMemalign(128, bytes)))
		{
			static int warned;
			if (!warned)
			{
				PS3_Log("[gl] world in RSX memory: no room for page %d (%u KB used), drawn through the ring",
						page, (unsigned)(world_bytes >> 10));
				warned = 1;
			}
			world_pages[page].mem[j] = NULL;
			continue;
		}
		world_bytes += bytes;
		rsxAddressToOffset(world_pages[page].mem[j], &world_pages[page].off[j]);
		out = (ps3gl_vertex_t *)world_pages[page].mem[j];
		for (i = 0; i < count; i++, out++)
		{
			const uint8_t *src = (const uint8_t *)v + (size_t)i * stride;
			const float *xyz = (const float *)(src + xyz_off);
			const float *uv = (const float *)(src + (j ? lm_off : uv_off));
			const float *lm = (const float *)(src + lm_off);

			out->x = xyz[0];
			out->y = xyz[1];
			out->z = xyz[2];
			out->w = 1.0f;
			out->u0 = uv[0];
			out->v0 = uv[1];
			out->u1 = lm[0];
			out->v1 = lm[1];
			out->color = 0xffffffffu;
		}
	}
	__asm__ volatile("sync" ::: "memory");
	/* the RSX's vertex cache may hold what this memory had before */
	PS3_Cmd_Reserve();
	rsxInvalidateVertexCache(ps3gl.ctx);
	if (page == 0)
		PS3_Log("[gl] world in RSX memory: the vertices are converted at the map's load");
}

/* The next draws are of this page's vertices (-1: not the world) */
void
ps3glUseWorldPage(int page, int lightmap)
{
	world_cur = page;
	world_cur_lm = lightmap ? 1 : 0;
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

	/* the world: straight from RSX memory, only the indices sent */
	if (world_cur >= 0 && world_cur < world_npages && ps3gl.color == 0xffffffffu &&
		world_pages[world_cur].mem[world_cur_lm] && hi < world_pages[world_cur].count)
	{
		BindRing(world_pages[world_cur].off[world_cur_lm]);
		DrawIndexedInline(prim, idx16, count);
		ps3gl.st_draws++;
		return;
	}

	/* The world draws each texture's surfaces from one big vertex array:
	   the range lo..hi spans thousands of vertices the draw doesn't use.
	   Copy only the ones it does (the world's CPU time was mostly this). */
	if (hi - lo + 1 > count && hi < 65536)
	{
		static uint32_t stamp[65536], gen;
		static uint16_t remap[65536];
		static int list[65536];
		int n = 0;

		if (++gen == 0)
		{
			memset(stamp, 0, sizeof(stamp));
			gen = 1;
		}

		for (i = 0; i < count; i++)
		{
			int v = idx16[i];

			if (stamp[v] != gen)
			{
				stamp[v] = gen;
				remap[v] = (uint16_t)n;
				list[n++] = v;
			}

			idx16[i] = remap[v];
		}

		if (!CopyVertexList(0, n, units, &off, list))
		{
			return;
		}

		lo = 0;
		hi = n - 1;
	}
	else
	{
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
	}

	BindRing(off);
	DrawIndexedInline(prim, idx16, count);

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

	if (nidx)
	{
		DrawIndexedInline(prim, im_idx, nidx);
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
