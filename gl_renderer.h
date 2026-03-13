#pragma once
#include <GL/glew.h>
#include <GL/gl.h>

// ============================================================================
// VERTEX / GL STATE
// ============================================================================

typedef struct { float x, y, r, g, b, a; } Vertex;

typedef struct { float m[16]; } Mat4;

struct GLState {
    GLuint prog, vao, vbo;
    GLint  proj_loc;
    Mat4   proj;
    float  cr, cg, cb, ca;
};

extern GLState G;

// ============================================================================
// RENDER MODES
// ============================================================================

#define RENDER_MODE_NORMAL    0
#define RENDER_MODE_CRT       1
#define RENDER_MODE_LCD       2
#define RENDER_MODE_VHS       3
#define RENDER_MODE_FOCUS     4
#define RENDER_MODE_C64       5
#define RENDER_MODE_COMPOSITE 6
#define RENDER_MODE_COUNT     7

extern int g_render_mode;

// ============================================================================
// API
// ============================================================================

Mat4  mat4_ortho(float l, float r, float b, float t, float n, float f);
void  gl_init_renderer(int w, int h);
void  gl_resize_fbo(int w, int h);

// Call before rendering terminal content — binds the offscreen FBO
void  gl_begin_frame(void);
// Call after rendering terminal content — applies post-process and blits to screen
void  gl_end_frame(float time, int win_w, int win_h);

// Append vertices to the frame accumulator (does NOT issue a draw call).
// All geometry must be GL_TRIANGLES — mixing modes is not supported.
void  draw_verts(Vertex *v, int n, GLenum mode);
void  draw_rect(float x, float y, float w, float h, float r, float g, float b, float a);

// Issue one glDrawArrays for everything accumulated since the last flush.
// Called automatically by gl_end_frame(); call manually only if you need an
// explicit mid-frame ordering boundary (e.g. before the post-process blit).
void  gl_flush_verts(void);

// Two-FBO split: terminal content is cached in a separate FBO so fight mode
// and animated render modes don't need to re-walk every cell each frame.
//
// Usage:
//   if (term_dirty) {
//       gl_begin_term_frame(w, h, bg_r, bg_g, bg_b);
//       term_render(...);
//       gl_end_term_frame();
//   }
//   gl_begin_frame();       // binds composite FBO, blits term cache in
//   fight_render(...);
//   gl_end_frame(...);      // post-process + blit to screen
void  gl_begin_term_frame(int win_w, int win_h, float bg_r, float bg_g, float bg_b);
void  gl_end_term_frame(void);

// Set the focused row (0..1 UV) for FOCUS mode — call before gl_end_frame
