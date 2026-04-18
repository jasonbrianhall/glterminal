/*
 * wopr_chess_pieces.h
 *
 * Self-contained OpenGL texture loader and renderer for chess piece sprites.
 * Decodes the embedded 46×46 24-bit BMP data from chess_pieces.h and uploads
 * each piece to a GL_RGBA texture.  Provides a simple draw_chess_piece() call
 * that renders a textured quad at any size using its own mini shader program.
 *
 * Usage:
 *   chess_pieces_gl_init();          // call once after GL context exists
 *   draw_chess_piece(type, color, x, y, size, win_w, win_h);
 *   chess_pieces_gl_shutdown();      // call on teardown
 */

#pragma once

#include <GL/glew.h>
#include "beatchess.h"   // PieceType, ChessColor, WHITE, BLACK, KING…
#include "chess_pieces.h"

// ─── BMP decode ──────────────────────────────────────────────────────────────

// Returns a heap-allocated RGBA buffer (width*height*4 bytes) decoded from a
// 24-bit uncompressed BMP.  Pixels whose green channel is dominant (G > R+30
// and G > B+30 and G > 200) are treated as transparent (alpha = 0).
// Caller must free() the returned pointer.  Returns nullptr on error.
static unsigned char *bmp_to_rgba(const unsigned char *data, unsigned int len,
                                  int *out_w, int *out_h)
{
    if (len < 54 || data[0] != 'B' || data[1] != 'M') return nullptr;

    int data_offset = data[10] | (data[11]<<8) | (data[12]<<16) | (data[13]<<24);
    int width       = data[18] | (data[19]<<8) | (data[20]<<16) | (data[21]<<24);
    int height      = data[22] | (data[23]<<8) | (data[24]<<16) | (data[25]<<24);
    if (height < 0) height = -height;
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) return nullptr;

    int bytes_per_row = ((width * 3 + 3) / 4) * 4;
    const unsigned char *pixels = data + data_offset;

    unsigned char *rgba = (unsigned char *)malloc(width * height * 4);
    if (!rgba) return nullptr;

    for (int row = 0; row < height; row++) {
        // BMPs are stored bottom-up
        const unsigned char *src = pixels + (height - 1 - row) * bytes_per_row;
        unsigned char       *dst = rgba + row * width * 4;
        for (int col = 0; col < width; col++) {
            unsigned char b = src[col*3+0];
            unsigned char g = src[col*3+1];
            unsigned char r = src[col*3+2];
            // Chroma-key: pure/near-pure green background → transparent
            if (g > r + 30 && g > b + 30 && g > 200) {
                dst[col*4+0] = 0;
                dst[col*4+1] = 0;
                dst[col*4+2] = 0;
                dst[col*4+3] = 0;
            } else {
                dst[col*4+0] = r;
                dst[col*4+1] = g;
                dst[col*4+2] = b;
                dst[col*4+3] = 255;
            }
        }
    }
    *out_w = width;
    *out_h = height;
    return rgba;
}

// ─── Shader source ───────────────────────────────────────────────────────────

static const char *PIECE_VERT_SRC = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
uniform vec4 uRect;   // x, y, w, h  in pixels
uniform vec2 uRes;    // window width, height
void main() {
    vec2 px = uRect.xy + aPos * uRect.zw;
    // flip Y: GL bottom-left vs screen top-left
    vec2 ndc = vec2(px.x / uRes.x * 2.0 - 1.0,
                    1.0 - px.y / uRes.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
}
)";

static const char *PIECE_FRAG_SRC = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform float uAlpha;   // overall opacity multiplier (1.0 = fully opaque)
void main() {
    vec4 c = texture(uTex, vUV);
    fragColor = vec4(c.rgb, c.a * uAlpha);
}
)";

// ─── State ────────────────────────────────────────────────────────────────────

struct ChessPiecesGL {
    GLuint prog;
    GLuint vao, vbo;
    GLuint tex[2][7];   // [0=WHITE,1=BLACK][PieceType 1..6]

    GLint  loc_rect;
    GLint  loc_res;
    GLint  loc_alpha;

    bool   ready;
    int    win_w, win_h;  // updated each draw call
};

static ChessPiecesGL g_cpgl = {};

// ─── Init / shutdown ─────────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, 512, nullptr, buf);
        fprintf(stderr, "[chess_pieces] shader error: %s\n", buf);
    }
    return s;
}

static GLuint load_piece_tex(const unsigned char *bmp, unsigned int len) {
    int w, h;
    unsigned char *rgba = bmp_to_rgba(bmp, len, &w, &h);
    if (!rgba) return 0;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(rgba);
    return tex;
}

inline void chess_pieces_gl_init() {
    if (g_cpgl.ready) return;

    // Build shader
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   PIECE_VERT_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, PIECE_FRAG_SRC);
    g_cpgl.prog = glCreateProgram();
    glAttachShader(g_cpgl.prog, vs);
    glAttachShader(g_cpgl.prog, fs);
    glLinkProgram(g_cpgl.prog);
    glDeleteShader(vs); glDeleteShader(fs);

    g_cpgl.loc_rect  = glGetUniformLocation(g_cpgl.prog, "uRect");
    g_cpgl.loc_res   = glGetUniformLocation(g_cpgl.prog, "uRes");
    g_cpgl.loc_alpha = glGetUniformLocation(g_cpgl.prog, "uAlpha");

    // Unit quad: position (0..1, 0..1) and UV
    float verts[] = {
        // x    y    u    v
        0.f, 0.f,  0.f, 0.f,
        1.f, 0.f,  1.f, 0.f,
        1.f, 1.f,  1.f, 1.f,
        0.f, 0.f,  0.f, 0.f,
        1.f, 1.f,  1.f, 1.f,
        0.f, 1.f,  0.f, 1.f,
    };
    glGenVertexArrays(1, &g_cpgl.vao);
    glGenBuffers(1, &g_cpgl.vbo);
    glBindVertexArray(g_cpgl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_cpgl.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glBindVertexArray(0);

    // Upload all 12 piece textures
    // Index: [0=WHITE][KING..PAWN], [1=BLACK][KING..PAWN]
    g_cpgl.tex[0][KING]   = load_piece_tex(white_king_bmp,   white_king_bmp_len);
    g_cpgl.tex[0][QUEEN]  = load_piece_tex(white_queen_bmp,  white_queen_bmp_len);
    g_cpgl.tex[0][ROOK]   = load_piece_tex(white_rook_bmp,   white_rook_bmp_len);
    g_cpgl.tex[0][BISHOP] = load_piece_tex(white_bishop_bmp, white_bishop_bmp_len);
    g_cpgl.tex[0][KNIGHT] = load_piece_tex(white_knight_bmp, white_knight_bmp_len);
    g_cpgl.tex[0][PAWN]   = load_piece_tex(white_pawn_bmp,   white_pawn_bmp_len);

    g_cpgl.tex[1][KING]   = load_piece_tex(black_king_bmp,   black_king_bmp_len);
    g_cpgl.tex[1][QUEEN]  = load_piece_tex(black_queen_bmp,  black_queen_bmp_len);
    g_cpgl.tex[1][ROOK]   = load_piece_tex(black_rook_bmp,   black_rook_bmp_len);
    g_cpgl.tex[1][BISHOP] = load_piece_tex(black_bishop_bmp, black_bishop_bmp_len);
    g_cpgl.tex[1][KNIGHT] = load_piece_tex(black_knight_bmp, black_knight_bmp_len);
    g_cpgl.tex[1][PAWN]   = load_piece_tex(black_pawn_bmp,   black_pawn_bmp_len);

    g_cpgl.ready = true;
}

inline void chess_pieces_gl_shutdown() {
    if (!g_cpgl.ready) return;
    for (int c = 0; c < 2; c++)
        for (int t = 1; t <= 6; t++)
            if (g_cpgl.tex[c][t]) glDeleteTextures(1, &g_cpgl.tex[c][t]);
    glDeleteBuffers(1, &g_cpgl.vbo);
    glDeleteVertexArrays(1, &g_cpgl.vao);
    glDeleteProgram(g_cpgl.prog);
    g_cpgl.ready = false;
}

// ─── Draw ─────────────────────────────────────────────────────────────────────

// Draw a piece sprite.
//   px, py  — top-left pixel coordinate (screen space, Y=0 at top)
//   size    — square size in pixels
//   win_w, win_h — current window dimensions (for NDC conversion)
//   alpha   — 0..1 overall opacity
inline void draw_chess_piece(PieceType type, ChessColor color,
                              float px, float py, float size,
                              int win_w, int win_h, float alpha = 1.0f)
{
    if (!g_cpgl.ready || type == EMPTY || color == NONE) return;
    int ci = (color == WHITE) ? 0 : 1;
    GLuint tex = g_cpgl.tex[ci][type];
    if (!tex) return;

    // Flush any pending colored-quad geometry from gl_draw_rect before switching programs
    // (The caller must call gl_flush_verts() before this if needed — we don't own that state.)

    GLint prev_prog; glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    GLint prev_vao;  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);

    glUseProgram(g_cpgl.prog);
    glUniform4f(g_cpgl.loc_rect, px, py, size, size);
    glUniform2f(g_cpgl.loc_res,  (float)win_w, (float)win_h);
    glUniform1f(g_cpgl.loc_alpha, alpha);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(g_cpgl.vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(prev_prog);
    glBindVertexArray(prev_vao);
}
