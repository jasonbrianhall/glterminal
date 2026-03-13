#pragma once
#include <GL/glew.h>
#include <GL/gl.h>

// ============================================================================
// VERTEX / GL STATE
// ============================================================================

typedef struct { float x, y, r, g, b, a; } Vertex;

typedef struct {
    float m[16];
} Mat4;

// VS and FS shader source strings are defined in gl_renderer.cpp

struct GLState {
    GLuint prog, vao, vbo;
    GLint  proj_loc;
    Mat4   proj;
    float  cr, cg, cb, ca;
};

extern GLState G;

Mat4  mat4_ortho(float l, float r, float b, float t, float n, float f);
void  gl_init_renderer(int w, int h);
void  draw_verts(Vertex *v, int n, GLenum mode);
void  draw_rect(float x, float y, float w, float h, float r, float g, float b, float a);
