// gl_renderer.h — auto-selects backend based on SDL_RENDERER define.
//
// When building with -DSDL_RENDERER the entire OpenGL path is replaced by
// sdl_renderer.h which exposes the same API surface.  All other source files
// (#include "gl_renderer.h") require no changes.
//
// Original OpenGL-specific declarations follow below for the normal build.

#ifdef SDL_RENDERER
#  include "sdl_renderer.h"
#else

#pragma once
#include <GL/glew.h>
#include <GL/gl.h>
#include <stdint.h>

// ============================================================================
// VERTEX / GL STATE
// ============================================================================

typedef struct { float x, y, r, g, b, a; } Vertex;

typedef struct {
    float x, y;
    float u, v;
    float tint_r, tint_g, tint_b, tint_a;
    float color_glyph;  // 0.0 = grayscale, 1.0 = RGBA emoji
} GlyphVertex;

typedef struct { float m[16]; } Mat4;

struct GLState {
    GLuint prog, vao, vbo;
    GLint  proj_loc;
    Mat4   proj;
    float  cr, cg, cb, ca;
};

extern GLState G;

#define MAX_GLYPH_VERTS 65536

struct GlyphGLState {
    GLuint prog = 0;
    GLuint vao  = 0;
    GLuint vbo  = 0;
    GLint  proj_loc     = -1;
    GLint  atlas_loc    = -1;
};
extern GlyphGLState GG;

// ============================================================================
// RENDER MODES  (bitmask — multiple modes can be active simultaneously)
// ============================================================================

#define RENDER_MODE_NORMAL    0
#define RENDER_MODE_CRT       1
#define RENDER_MODE_LCD       2
#define RENDER_MODE_VHS       3
#define RENDER_MODE_FOCUS     4
#define RENDER_MODE_C64       5
#define RENDER_MODE_COMPOSITE 6
#define RENDER_MODE_BLOOM     7
#define RENDER_MODE_GHOSTING  8
#define RENDER_MODE_WIREFRAME 9
#define RENDER_MODE_COUNT     10

#define RENDER_BIT_CRT        (1u<<RENDER_MODE_CRT)
#define RENDER_BIT_LCD        (1u<<RENDER_MODE_LCD)
#define RENDER_BIT_VHS        (1u<<RENDER_MODE_VHS)
#define RENDER_BIT_FOCUS      (1u<<RENDER_MODE_FOCUS)
#define RENDER_BIT_C64        (1u<<RENDER_MODE_C64)
#define RENDER_BIT_COMPOSITE  (1u<<RENDER_MODE_COMPOSITE)
#define RENDER_BIT_BLOOM      (1u<<RENDER_MODE_BLOOM)
#define RENDER_BIT_GHOSTING   (1u<<RENDER_MODE_GHOSTING)
#define RENDER_BIT_WIREFRAME  (1u<<RENDER_MODE_WIREFRAME)

extern uint32_t g_render_mode;

// ============================================================================
// API
// ============================================================================

Mat4  mat4_ortho(float l, float r, float b, float t, float n, float f);
void  gl_init_renderer(int w, int h);
void  gl_resize_fbo(int w, int h);

void  gl_begin_frame(void);
void  gl_end_frame(float time, int win_w, int win_h);

void  draw_verts(Vertex *v, int n, GLenum mode);
void  draw_rect(float x, float y, float w, float h, float r, float g, float b, float a);
void  draw_glyph_verts(GlyphVertex *v, int n);

void  gl_flush_verts(void);
void  gl_flush_glyphs(void);

void  gl_begin_term_frame(int win_w, int win_h, float bg_r, float bg_g, float bg_b);
void  gl_clear_term_frame(int win_w, int win_h, float bg_r, float bg_g, float bg_b);
void  gl_end_term_frame(void);

void  gl_update_ghost(int win_w, int win_h);

extern bool g_wireframe_cells;

#endif // !SDL_RENDERER

// ============================================================================
// BASIC GRAPHICS FBO
// Persistent drawing surface — survives terminal redraws.
// Call gl_basic_begin() before BASIC draw calls, gl_basic_end() after.
// gl_basic_clear() wipes it (call on CLS).
// ============================================================================
void gl_basic_begin(int win_w, int win_h);
void gl_basic_end(void);
void gl_basic_clear(int win_w, int win_h);
