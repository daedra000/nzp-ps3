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
 * <GL/gl.h> for the PS3. There is no OpenGL on the PS3: these functions
 * are implemented by tyrquake-ps3/common/gl/ (ps3gl, from Quake2PS3), a
 * small fixed function OpenGL 1.3 on top of the RSX, and cover what
 * TyrQuake's GL renderer uses. Types and enum values are the standard
 * ones; the extension enums TyrQuake's code names (buffer objects, ARB
 * programs, fog...) are here too so it compiles, even where ps3gl doesn't
 * offer the extension.
 *
 * =======================================================================
 */

#ifndef PS3_GL_H
#define PS3_GL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#ifndef GLAPI
#define GLAPI extern
#endif

/* What is covered, so upstream headers don't redefine it. */
#define GL_VERSION_1_1 1
#define GL_VERSION_1_2 1
#define GL_VERSION_1_3 1
#define GL_ARB_multitexture 1
#define GL_EXT_shared_texture_palette 1
#define GL_EXT_texture_filter_anisotropic 1
#define GL_NV_multisample_filter_hint 1

typedef unsigned int    GLenum;
typedef unsigned char   GLboolean;
typedef unsigned int    GLbitfield;
typedef void            GLvoid;
typedef signed char     GLbyte;
typedef short           GLshort;
typedef int             GLint;
typedef unsigned char   GLubyte;
typedef unsigned short  GLushort;
typedef unsigned int    GLuint;
typedef int             GLsizei;
typedef float           GLfloat;
typedef float           GLclampf;
typedef double          GLdouble;
typedef double          GLclampd;
typedef char            GLchar;
typedef ptrdiff_t       GLintptr;
typedef ptrdiff_t       GLsizeiptr;

#define GL_FALSE                        0
#define GL_TRUE                         1

/* errors */
#define GL_NO_ERROR                     0
#define GL_INVALID_ENUM                 0x0500
#define GL_INVALID_VALUE                0x0501
#define GL_INVALID_OPERATION            0x0502
#define GL_STACK_OVERFLOW               0x0503
#define GL_STACK_UNDERFLOW              0x0504
#define GL_OUT_OF_MEMORY                0x0505

/* primitives */
#define GL_POINTS                       0x0000
#define GL_LINES                        0x0001
#define GL_LINE_LOOP                    0x0002
#define GL_LINE_STRIP                   0x0003
#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_TRIANGLE_FAN                 0x0006
#define GL_QUADS                        0x0007
#define GL_QUAD_STRIP                   0x0008
#define GL_POLYGON                      0x0009

/* glClear */
#define GL_DEPTH_BUFFER_BIT             0x00000100
#define GL_STENCIL_BUFFER_BIT           0x00000400
#define GL_COLOR_BUFFER_BIT             0x00004000

/* comparison */
#define GL_NEVER                        0x0200
#define GL_LESS                         0x0201
#define GL_EQUAL                        0x0202
#define GL_LEQUAL                       0x0203
#define GL_GREATER                      0x0204
#define GL_NOTEQUAL                     0x0205
#define GL_GEQUAL                       0x0206
#define GL_ALWAYS                       0x0207

/* blending */
#define GL_ZERO                         0
#define GL_ONE                          1
#define GL_SRC_COLOR                    0x0300
#define GL_ONE_MINUS_SRC_COLOR          0x0301
#define GL_SRC_ALPHA                    0x0302
#define GL_ONE_MINUS_SRC_ALPHA          0x0303
#define GL_DST_ALPHA                    0x0304
#define GL_ONE_MINUS_DST_ALPHA          0x0305
#define GL_DST_COLOR                    0x0306
#define GL_ONE_MINUS_DST_COLOR          0x0307
#define GL_SRC_ALPHA_SATURATE           0x0308

/* faces */
#define GL_FRONT                        0x0404
#define GL_BACK                         0x0405
#define GL_FRONT_AND_BACK               0x0408
#define GL_CW                           0x0900
#define GL_CCW                          0x0901

/* capabilities */
#define GL_POINT_SMOOTH                 0x0B10
#define GL_LINE_SMOOTH                  0x0B20
#define GL_CULL_FACE                    0x0B44
#define GL_FOG                          0x0B60
#define GL_DEPTH_TEST                   0x0B71
#define GL_STENCIL_TEST                 0x0B90
#define GL_NORMALIZE                    0x0BA1
#define GL_ALPHA_TEST                   0x0BC0
#define GL_DITHER                       0x0BD0
#define GL_BLEND                        0x0BE2
#define GL_SCISSOR_TEST                 0x0C11
#define GL_TEXTURE_2D                   0x0DE1
#define GL_POLYGON_OFFSET_FILL          0x8037
#define GL_MULTISAMPLE                  0x809D
#define GL_CLIP_PLANE0                  0x3000

/* client arrays */
#define GL_VERTEX_ARRAY                 0x8074
#define GL_NORMAL_ARRAY                 0x8075
#define GL_COLOR_ARRAY                  0x8076
#define GL_TEXTURE_COORD_ARRAY          0x8078

/* polygon mode, shading */
#define GL_POINT                        0x1B00
#define GL_LINE                         0x1B01
#define GL_FILL                         0x1B02
#define GL_FLAT                         0x1D00
#define GL_SMOOTH                       0x1D01

/* stencil */
#define GL_KEEP                         0x1E00
#define GL_REPLACE                      0x1E01
#define GL_INCR                         0x1E02
#define GL_DECR                         0x1E03
#define GL_INVERT                       0x150A

/* strings */
#define GL_VENDOR                       0x1F00
#define GL_RENDERER                     0x1F01
#define GL_VERSION                      0x1F02
#define GL_EXTENSIONS                   0x1F03

/* hints */
#define GL_PERSPECTIVE_CORRECTION_HINT  0x0C50
#define GL_DONT_CARE                    0x1100
#define GL_FASTEST                      0x1101
#define GL_NICEST                       0x1102

/* texture environment */
#define GL_TEXTURE_ENV                  0x2300
#define GL_TEXTURE_ENV_MODE             0x2200
#define GL_TEXTURE_ENV_COLOR            0x2201
#define GL_MODULATE                     0x2100
#define GL_DECAL                        0x2101
#define GL_ADD                          0x0104
#define GL_COMBINE                      0x8570
#define GL_COMBINE_RGB                  0x8571
#define GL_COMBINE_ALPHA                0x8572
#define GL_RGB_SCALE                    0x8573
#define GL_PRIMARY_COLOR                0x8577
#define GL_PREVIOUS                     0x8578
#define GL_SOURCE0_RGB                  0x8580
#define GL_SOURCE1_RGB                  0x8581
#define GL_SOURCE2_RGB                  0x8582
#define GL_OPERAND0_RGB                 0x8590
#define GL_OPERAND1_RGB                 0x8591

/* texture parameters */
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_NEAREST                      0x2600
#define GL_LINEAR                       0x2601
#define GL_NEAREST_MIPMAP_NEAREST       0x2700
#define GL_LINEAR_MIPMAP_NEAREST        0x2701
#define GL_NEAREST_MIPMAP_LINEAR        0x2702
#define GL_LINEAR_MIPMAP_LINEAR         0x2703
#define GL_CLAMP                        0x2900
#define GL_REPEAT                       0x2901
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_GENERATE_MIPMAP              0x8191
#define GL_TEXTURE_BASE_LEVEL           0x813C
#define GL_TEXTURE_MAX_LEVEL            0x813D
#define GL_PROXY_TEXTURE_2D             0x8064
#define GL_TEXTURE_MAX_ANISOTROPY_EXT   0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF

/* pixel formats */
#define GL_COLOR_INDEX                  0x1900
#define GL_RED                          0x1903
#define GL_ALPHA                        0x1906
#define GL_RGB                          0x1907
#define GL_RGBA                         0x1908
#define GL_LUMINANCE                    0x1909
#define GL_LUMINANCE_ALPHA              0x190A
#define GL_R3_G3_B2                     0x2A10
#define GL_RGB4                         0x804F
#define GL_RGB5                         0x8050
#define GL_RGB8                         0x8051
#define GL_RGBA2                        0x8055
#define GL_RGBA4                        0x8056
#define GL_RGB5_A1                      0x8057
#define GL_RGBA8                        0x8058
#define GL_SHARED_TEXTURE_PALETTE_EXT   0x81FB

/* data types */
#define GL_BYTE                         0x1400
#define GL_UNSIGNED_BYTE                0x1401
#define GL_SHORT                        0x1402
#define GL_UNSIGNED_SHORT               0x1403
#define GL_INT                          0x1404
#define GL_UNSIGNED_INT                 0x1405
#define GL_FLOAT                        0x1406
#define GL_DOUBLE                       0x140A

/* matrices */
#define GL_MODELVIEW                    0x1700
#define GL_PROJECTION                   0x1701
#define GL_TEXTURE                      0x1702
#define GL_MODELVIEW_MATRIX             0x0BA6
#define GL_PROJECTION_MATRIX            0x0BA7

/* pixel store */
#define GL_UNPACK_ROW_LENGTH            0x0CF2
#define GL_UNPACK_ALIGNMENT             0x0CF5
#define GL_PACK_ALIGNMENT               0x0D05

/* queries */
#define GL_MAX_TEXTURE_SIZE             0x0D33
#define GL_DEPTH_BITS                   0x0D56
#define GL_STENCIL_BITS                 0x0D57
#define GL_DEPTH_WRITEMASK              0x0B72
#define GL_COLOR_WRITEMASK              0x0C23

/* multitexture */
#define GL_TEXTURE0                     0x84C0
#define GL_TEXTURE1                     0x84C1
#define GL_TEXTURE0_ARB                 0x84C0
#define GL_TEXTURE1_ARB                 0x84C1
#define GL_ACTIVE_TEXTURE               0x84E0
#define GL_MAX_TEXTURE_UNITS            0x84E2

/* point parameters (not supported: ref_gl1 falls back to triangles) */
#define GL_POINT_SIZE_MIN               0x8126
#define GL_POINT_SIZE_MAX               0x8127
#define GL_POINT_DISTANCE_ATTENUATION   0x8129

#define GL_MULTISAMPLE_FILTER_HINT_NV   0x8534

#define GL_TEXTURE2                     0x84C2
#define GL_TEXTURE2_ARB                 0x84C2
#define GL_MAX_TEXTURE_UNITS_ARB        0x84E2
#define GL_MAX_ELEMENTS_VERTICES        0x80E8
#define GL_MAX_ELEMENTS_INDICES         0x80E9
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS   0x86A3
#define GL_READ_BUFFER                  0x0C02
#define GL_DRAW_BUFFER                  0x0C01

/* fog */
#define GL_EXP                          0x0800
#define GL_EXP2                         0x0801
#define GL_FOG_DENSITY                  0x0B62
#define GL_FOG_START                    0x0B63
#define GL_FOG_END                      0x0B64
#define GL_FOG_MODE                     0x0B65
#define GL_FOG_COLOR                    0x0B66

/* buffer objects and ARB programs: not offered, the names only */
#define GL_ARRAY_BUFFER                 0x8892
#define GL_ELEMENT_ARRAY_BUFFER         0x8893
#define GL_STREAM_DRAW                  0x88E0
#define GL_STATIC_DRAW                  0x88E4
#define GL_DYNAMIC_DRAW                 0x88E8
#define GL_READ_ONLY                    0x88B8
#define GL_WRITE_ONLY                   0x88B9
#define GL_VERTEX_PROGRAM_ARB           0x8620
#define GL_FRAGMENT_PROGRAM_ARB         0x8804
#define GL_PROGRAM_ERROR_POSITION_ARB   0x864B
#define GL_PROGRAM_ERROR_STRING_ARB     0x8874
#define GL_PROGRAM_FORMAT_ASCII_ARB     0x8875

/* ---------------------------------------------------------------- */

GLAPI void APIENTRY glEnable(GLenum cap);
GLAPI void APIENTRY glDisable(GLenum cap);
GLAPI void APIENTRY glEnableClientState(GLenum array);
GLAPI void APIENTRY glDisableClientState(GLenum array);
GLAPI void APIENTRY glBlendFunc(GLenum sfactor, GLenum dfactor);
GLAPI void APIENTRY glAlphaFunc(GLenum func, GLclampf ref);
GLAPI void APIENTRY glDepthFunc(GLenum func);
GLAPI void APIENTRY glDepthMask(GLboolean flag);
GLAPI void APIENTRY glDepthRange(GLclampd zNear, GLclampd zFar);
GLAPI void APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
GLAPI void APIENTRY glCullFace(GLenum mode);
GLAPI void APIENTRY glFrontFace(GLenum mode);
GLAPI void APIENTRY glPolygonMode(GLenum face, GLenum mode);
GLAPI void APIENTRY glPolygonOffset(GLfloat factor, GLfloat units);
GLAPI void APIENTRY glShadeModel(GLenum mode);
GLAPI void APIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
GLAPI void APIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
GLAPI void APIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask);
GLAPI void APIENTRY glStencilMask(GLuint mask);
GLAPI void APIENTRY glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
GLAPI void APIENTRY glHint(GLenum target, GLenum mode);
GLAPI void APIENTRY glPointSize(GLfloat size);
GLAPI void APIENTRY glLineWidth(GLfloat width);

GLAPI void APIENTRY glClear(GLbitfield mask);
GLAPI void APIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
GLAPI void APIENTRY glClearDepth(GLclampd depth);
GLAPI void APIENTRY glClearStencil(GLint s);
GLAPI void APIENTRY glDrawBuffer(GLenum mode);
GLAPI void APIENTRY glFinish(void);
GLAPI void APIENTRY glFlush(void);
GLAPI void APIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
		GLenum format, GLenum type, GLvoid *pixels);

GLAPI GLenum APIENTRY glGetError(void);
GLAPI const GLubyte * APIENTRY glGetString(GLenum name);
GLAPI void APIENTRY glGetFloatv(GLenum pname, GLfloat *params);
GLAPI void APIENTRY glGetIntegerv(GLenum pname, GLint *params);
GLAPI void APIENTRY glGetBooleanv(GLenum pname, GLboolean *params);

GLAPI void APIENTRY glMatrixMode(GLenum mode);
GLAPI void APIENTRY glLoadIdentity(void);
GLAPI void APIENTRY glLoadMatrixf(const GLfloat *m);
GLAPI void APIENTRY glMultMatrixf(const GLfloat *m);
GLAPI void APIENTRY glPushMatrix(void);
GLAPI void APIENTRY glPopMatrix(void);
GLAPI void APIENTRY glOrtho(GLdouble left, GLdouble right, GLdouble bottom,
		GLdouble top, GLdouble zNear, GLdouble zFar);
GLAPI void APIENTRY glFrustum(GLdouble left, GLdouble right, GLdouble bottom,
		GLdouble top, GLdouble zNear, GLdouble zFar);
GLAPI void APIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);

GLAPI void APIENTRY glColor3f(GLfloat r, GLfloat g, GLfloat b);
GLAPI void APIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
GLAPI void APIENTRY glColor4fv(const GLfloat *v);
GLAPI void APIENTRY glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
GLAPI void APIENTRY glColor4ubv(const GLubyte *v);
GLAPI void APIENTRY glColor3fv(const GLfloat *v);
GLAPI void APIENTRY glColor3ub(GLubyte r, GLubyte g, GLubyte b);
GLAPI void APIENTRY glColor3ubv(const GLubyte *v);

/* immediate mode */
GLAPI void APIENTRY glBegin(GLenum mode);
GLAPI void APIENTRY glEnd(void);
GLAPI void APIENTRY glVertex2f(GLfloat x, GLfloat y);
GLAPI void APIENTRY glVertex2i(GLint x, GLint y);
GLAPI void APIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glVertex3fv(const GLfloat *v);
GLAPI void APIENTRY glTexCoord2f(GLfloat s, GLfloat t);
GLAPI void APIENTRY glTexCoord2fv(const GLfloat *v);

GLAPI void APIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
GLAPI void APIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
GLAPI void APIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
GLAPI void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count);
GLAPI void APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
GLAPI void APIENTRY glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
		GLenum type, const GLvoid *indices);
GLAPI void APIENTRY glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr);

GLAPI void APIENTRY glGenTextures(GLsizei n, GLuint *textures);
GLAPI void APIENTRY glDeleteTextures(GLsizei n, const GLuint *textures);
GLAPI void APIENTRY glBindTexture(GLenum target, GLuint texture);
GLAPI void APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalFormat,
		GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
		const GLvoid *pixels);
GLAPI void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset,
		GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
		const GLvoid *pixels);
GLAPI void APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param);
GLAPI void APIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat param);
GLAPI void APIENTRY glTexEnvi(GLenum target, GLenum pname, GLint param);
GLAPI void APIENTRY glTexEnvf(GLenum target, GLenum pname, GLfloat param);
GLAPI void APIENTRY glPixelStorei(GLenum pname, GLint param);
GLAPI void APIENTRY glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params);
GLAPI void APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset,
		GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
GLAPI void APIENTRY glReadBuffer(GLenum mode);

GLAPI void APIENTRY glFogf(GLenum pname, GLfloat param);
GLAPI void APIENTRY glFogi(GLenum pname, GLint param);
GLAPI void APIENTRY glFogfv(GLenum pname, const GLfloat *params);

/* GL 1.3 / ARB_multitexture (TyrQuake gets them through GL_GetProcAddress) */
GLAPI void APIENTRY glActiveTexture(GLenum texture);
GLAPI void APIENTRY glClientActiveTexture(GLenum texture);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GL_H */
