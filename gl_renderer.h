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
#define RENDER_MODE_COUNT     6

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

void  draw_verts(Vertex *v, int n, GLenum mode);
void  draw_rect(float x, float y, float w, float h, float r, float g, float b, float a);

// Set the focused row (0..1 UV) for FOCUS mode — call before gl_end_frame
