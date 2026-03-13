#include "gl_renderer.h"
#include "gl_terminal.h"  // MAX_VERTS
#include <SDL2/SDL.h>
#include <stddef.h>       // offsetof

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec4 col;\n"
    "uniform mat4 proj;\n"
    "out vec4 vCol;\n"
    "void main(){gl_Position=proj*vec4(pos,0,1);vCol=col;}\n";

static const char *FS =
    "#version 330 core\n"
    "in vec4 vCol;\n"
    "out vec4 frag;\n"
    "void main(){frag=vCol;}\n";

GLState G = {};

Mat4 mat4_ortho(float l, float r, float b, float t, float n, float f) {
    Mat4 m = {};
    m.m[0]  =  2.f/(r-l);
    m.m[5]  =  2.f/(t-b);
    m.m[10] = -2.f/(f-n);
    m.m[12] = -(r+l)/(r-l);
    m.m[13] = -(t+b)/(t-b);
    m.m[14] = -(f+n)/(f-n);
    m.m[15] =  1.f;
    return m;
}

static GLuint compile_shader(const char *src, GLenum type) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
        //SDL_Log("[GL] shader error: %s\n", log);
    }
    return s;
}

void gl_init_renderer(int w, int h) {
    glewExperimental = GL_TRUE;
    glewInit();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLuint vs = compile_shader(VS, GL_VERTEX_SHADER);
    GLuint fs = compile_shader(FS, GL_FRAGMENT_SHADER);
    G.prog = glCreateProgram();
    glAttachShader(G.prog, vs); glAttachShader(G.prog, fs);
    glLinkProgram(G.prog);
    glDeleteShader(vs); glDeleteShader(fs);

    glGenVertexArrays(1, &G.vao);
    glGenBuffers(1, &G.vbo);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, MAX_VERTS * sizeof(Vertex), NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,r));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glUseProgram(G.prog);
    G.proj_loc = glGetUniformLocation(G.prog, "proj");
    G.proj = mat4_ortho(0, (float)w, (float)h, 0, -1, 1);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glUseProgram(0);

    G.cr = G.cg = G.cb = G.ca = 1.f;
}

void draw_verts(Vertex *v, int n, GLenum mode) {
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * sizeof(Vertex), v);
    glBindVertexArray(G.vao);
    glDrawArrays(mode, 0, n);
}

void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    Vertex v[6] = {
        {x,   y,   r,g,b,a}, {x+w, y,   r,g,b,a}, {x+w, y+h, r,g,b,a},
        {x,   y,   r,g,b,a}, {x+w, y+h, r,g,b,a}, {x,   y+h, r,g,b,a},
    };
    draw_verts(v, 6, GL_TRIANGLES);
}
