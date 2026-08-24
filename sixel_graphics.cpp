// sixel_graphics.cpp — DECSIXEL (Sixel graphics) implementation.
//
// Sixel has no persistent image registry or placement handshake the way
// Kitty graphics does: each DCS-wrapped sequence is a self-contained
// transmit-and-display. So unlike kitty_graphics.cpp, there's no id/query
// bookkeeping — every call to sixel_handle_dcs() decodes straight to a
// texture and appends one placement.
//
// Supported:
//   Raster attributes ("Pan;Pad;Ph;Pv) for sizing
//   Color registers (#Pc;Pu;Px;Py;Pz) — RGB (Pu=2) and HLS (Pu=1)
//   Repeat counts (!Pn)
//   Carriage return ($) and next-line (-)
//   Sixel data bytes (0x3F-0x7E)
//
// Not supported:
//   Background-select mode (Pb parameter) — always transparent background
//   Palette persistence across separate sixel sequences

#include "sixel_graphics.h"
#include "gl_renderer.h"
#include "sdl_renderer.h"
#include "term_pty.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ============================================================================
// STATE
// ============================================================================

struct SixelImage {
    GLuint       tex     = 0;
    SDL_Texture *sdl_tex = nullptr;
    int          pw = 0, ph = 0;
};

struct SixelPlacement {
    SixelImage img;
    int x_cell = 0, y_cell = 0;   // top-left cell at time of placement
    int cols = 0, rows = 0;       // cell footprint
};

struct SixelTermState {
    std::vector<SixelPlacement> placements;
};

static std::unordered_map<Terminal*, SixelTermState> s_terms;

// ============================================================================
// GL SHADER (own program, separate from kitty_graphics.cpp's private one)
// ============================================================================

static const char *SIXEL_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec2 tc;\n"
    "uniform mat4 proj;\n"
    "out vec2 vTC;\n"
    "void main(){ gl_Position = proj * vec4(pos,0,1); vTC = tc; }\n";

static const char *SIXEL_FS =
    "#version 330 core\n"
    "in vec2 vTC;\n"
    "out vec4 frag;\n"
    "uniform sampler2D img;\n"
    "void main(){ frag = texture(img, vTC); }\n";

static GLuint s_prog = 0, s_vao = 0, s_vbo = 0;
static GLint  s_proj_loc = -1, s_tex_loc = -1;

void sixel_init(void) {
    if (g_use_sdl_renderer) return;

    auto compile = [](const char *src, GLenum type) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, NULL);
        glCompileShader(s);
        int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); SDL_Log("[Sixel] shader: %s\n", log); }
        return s;
    };
    GLuint vs = compile(SIXEL_VS, GL_VERTEX_SHADER);
    GLuint fs = compile(SIXEL_FS, GL_FRAGMENT_SHADER);
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glDeleteShader(vs); glDeleteShader(fs);

    s_proj_loc = glGetUniformLocation(s_prog, "proj");
    s_tex_loc  = glGetUniformLocation(s_prog, "img");

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    SDL_Log("[Sixel] GL renderer ready\n");
}

// ============================================================================
// TEXTURE UPLOAD HELPERS
// ============================================================================

static GLuint upload_texture_rgba(const uint8_t *pixels, int w, int h) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static SDL_Texture *upload_sdl_texture_rgba(const uint8_t *pixels, int w, int h) {
    SDL_Texture *t = SDL_CreateTexture(g_sdl_renderer,
        SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, pixels, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void free_sixel_image(SixelImage &img) {
    if (g_use_sdl_renderer) { if (img.sdl_tex) SDL_DestroyTexture(img.sdl_tex); }
    else                    { if (img.tex)     glDeleteTextures(1, &img.tex); }
    img.tex = 0; img.sdl_tex = nullptr;
}

// ============================================================================
// COLOR
// ============================================================================

struct RGB8 { uint8_t r, g, b; };

// Sixel HLS: hue is 0-360 with a nonstandard zero-point (0 = blue). Close
// approximation via standard HSL math shifted by the sixel convention.
static RGB8 hls_to_rgb(int h, int l, int s) {
    float H = fmodf((float)h + 240.f, 360.f); // shift so sixel's 0=blue lands correctly
    float L = (float)l / 100.f, S = (float)s / 100.f;
    float C = (1.f - fabsf(2.f * L - 1.f)) * S;
    float Hp = H / 60.f;
    float X = C * (1.f - fabsf(fmodf(Hp, 2.f) - 1.f));
    float r1 = 0, g1 = 0, b1 = 0;
    if      (Hp < 1) { r1 = C; g1 = X; b1 = 0; }
    else if (Hp < 2) { r1 = X; g1 = C; b1 = 0; }
    else if (Hp < 3) { r1 = 0; g1 = C; b1 = X; }
    else if (Hp < 4) { r1 = 0; g1 = X; b1 = C; }
    else if (Hp < 5) { r1 = X; g1 = 0; b1 = C; }
    else              { r1 = C; g1 = 0; b1 = X; }
    float m = L - C / 2.f;
    RGB8 out;
    out.r = (uint8_t)std::min(255.f, (r1 + m) * 255.f);
    out.g = (uint8_t)std::min(255.f, (g1 + m) * 255.f);
    out.b = (uint8_t)std::min(255.f, (b1 + m) * 255.f);
    return out;
}

// ============================================================================
// DECODER — run twice: once to measure (canvas==nullptr), once to paint.
// ============================================================================

static int read_int(const char *body, int len, int &i, int def) {
    bool any = false, neg = false;
    int v = 0;
    if (i < len && body[i] == '-') { neg = true; i++; }
    while (i < len && body[i] >= '0' && body[i] <= '9') { v = v * 10 + (body[i] - '0'); i++; any = true; }
    if (!any) return def;
    return neg ? -v : v;
}

static void run_sixel_parser(const char *body, int len,
                              std::vector<uint8_t> *canvas, int canvas_w, int canvas_h,
                              int *out_w, int *out_h) {
    std::unordered_map<int, RGB8> palette;
    int cur_reg = 0;
    int x = 0, y = 0;
    int max_x = 0, max_y = 0;
    int repeat = 1;
    int i = 0;

    while (i < len) {
        char c = body[i];

        if (c == '"') {
            i++;
            read_int(body, len, i, 1);                       // Pan
            if (i < len && body[i] == ';') { i++; read_int(body, len, i, 1); }   // Pad
            if (i < len && body[i] == ';') { i++; int w = read_int(body, len, i, 0); if (w > max_x) max_x = w; }
            if (i < len && body[i] == ';') { i++; int h = read_int(body, len, i, 0); if (h > max_y) max_y = h; }
            continue;
        }
        if (c == '#') {
            i++;
            int reg = read_int(body, len, i, 0);
            if (i < len && body[i] == ';') {
                i++;
                int pu = read_int(body, len, i, 2);
                int p1 = 0, p2 = 0, p3 = 0;
                if (i < len && body[i] == ';') { i++; p1 = read_int(body, len, i, 0); }
                if (i < len && body[i] == ';') { i++; p2 = read_int(body, len, i, 0); }
                if (i < len && body[i] == ';') { i++; p3 = read_int(body, len, i, 0); }
                RGB8 col;
                if (pu == 1) {
                    col = hls_to_rgb(p1, p2, p3);
                } else {
                    col.r = (uint8_t)(std::min(100, p1) * 255 / 100);
                    col.g = (uint8_t)(std::min(100, p2) * 255 / 100);
                    col.b = (uint8_t)(std::min(100, p3) * 255 / 100);
                }
                palette[reg] = col;
            }
            cur_reg = reg;
            continue;
        }
        if (c == '!') {
            i++;
            repeat = read_int(body, len, i, 1);
            if (repeat < 1) repeat = 1;
            continue;
        }
        if (c == '$') { x = 0; i++; continue; }
        if (c == '-') { x = 0; y += 6; i++; continue; }

        if (c >= 0x3F && c <= 0x7E) {
            int bits = c - 0x3F;
            RGB8 col = {255, 255, 255};
            if (canvas) {
                auto it = palette.find(cur_reg);
                if (it != palette.end()) col = it->second;
            }
            for (int rep = 0; rep < repeat; rep++) {
                for (int b = 0; b < 6; b++) {
                    if (bits & (1 << b)) {
                        int px = x, py = y + b;
                        if (px + 1 > max_x) max_x = px + 1;
                        if (py + 1 > max_y) max_y = py + 1;
                        if (canvas && px >= 0 && px < canvas_w && py >= 0 && py < canvas_h) {
                            size_t idx = ((size_t)py * canvas_w + px) * 4;
                            (*canvas)[idx + 0] = col.r;
                            (*canvas)[idx + 1] = col.g;
                            (*canvas)[idx + 2] = col.b;
                            (*canvas)[idx + 3] = 255;
                        }
                    }
                }
                x++;
            }
            repeat = 1;
            i++;
            continue;
        }
        // Stray byte (whitespace some encoders insert between bands) — skip.
        i++;
    }

    if (out_w) *out_w = max_x;
    if (out_h) *out_h = max_y;
}

// ============================================================================
// PUBLIC API
// ============================================================================

void sixel_handle_dcs(Terminal *t, const char *params, int params_len,
                      const char *body, int body_len) {
    (void)params; (void)params_len; // Pb (background-select) not implemented

    SDL_Log("[Sixel] handle_dcs: params_len=%d body_len=%d\n", params_len, body_len);

    if (!body || body_len <= 0) {
        SDL_Log("[Sixel] empty body — nothing to decode\n");
        return;
    }

    int w = 0, h = 0;
    run_sixel_parser(body, body_len, nullptr, 0, 0, &w, &h);
    SDL_Log("[Sixel] measured w=%d h=%d\n", w, h);
    if (w <= 0 || h <= 0) {
        SDL_Log("[Sixel] zero-size image — aborting\n");
        return;
    }
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;

    std::vector<uint8_t> canvas((size_t)w * h * 4, 0); // transparent by default
    run_sixel_parser(body, body_len, &canvas, w, h, nullptr, nullptr);

    SixelImage img;
    img.pw = w; img.ph = h;
    if (g_use_sdl_renderer) img.sdl_tex = upload_sdl_texture_rgba(canvas.data(), w, h);
    else                    img.tex     = upload_texture_rgba(canvas.data(), w, h);

    SDL_Log("[Sixel] uploaded texture: gl_tex=%u sdl_tex=%p\n",
            img.tex, (void*)img.sdl_tex);

    SixelPlacement pl;
    pl.img    = img;
    pl.x_cell = t->cur_col;
    pl.y_cell = t->cur_row;
    pl.cols   = std::max(1, (int)((w + (int)t->cell_w - 1) / (int)t->cell_w));
    pl.rows   = std::max(1, (int)((h + (int)t->cell_h - 1) / (int)t->cell_h));

    int dirty_bottom = std::min(t->rows - 1, pl.y_cell + pl.rows - 1);
    term_dirty_rows(t, pl.y_cell, dirty_bottom);

    s_terms[t].placements.push_back(pl);

    SDL_Log("[Sixel] placed at cell(%d,%d) footprint %dx%d cells, total placements=%zu\n",
            pl.x_cell, pl.y_cell, pl.cols, pl.rows, s_terms[t].placements.size());

    // Cursor moves below the image and back to column 0 — matches common
    // xterm/mlterm behavior for the default (non-DECSDM) cursor mode.
    t->cur_row = std::min(t->rows - 1, pl.y_cell + pl.rows);
    t->cur_col = 0;
}

void sixel_render(Terminal *t, int ox, int oy) {
    auto tit = s_terms.find(t);
    if (tit == s_terms.end() || tit->second.placements.empty()) return;

    static int s_log_count = 0;
    if (s_log_count < 3) {
        SDL_Log("[Sixel] render: %zu placement(s), sdl_renderer=%d\n",
                tit->second.placements.size(), (int)g_use_sdl_renderer);
        s_log_count++;
    }

    float cw = t->cell_w, ch = t->cell_h;

    if (g_use_sdl_renderer) {
        gl_flush_verts();
        for (const SixelPlacement &pl : tit->second.placements) {
            if (!pl.img.sdl_tex) continue;
            float vis_row = (float)(pl.y_cell + t->sb_offset);
            if (vis_row + pl.rows <= 0 || vis_row >= (float)t->rows) continue;
            SDL_FRect dst = {
                ox + pl.x_cell * cw,
                oy + vis_row * ch,
                pl.cols * cw,
                pl.rows * ch
            };
            SDL_RenderCopyF(g_sdl_renderer, pl.img.sdl_tex, nullptr, &dst);
        }
        return;
    }

    if (!s_prog) return; // GL not initialized (shouldn't happen if sixel_init() was called)

    gl_flush_verts();
    glUseProgram(s_prog);
    glUniformMatrix4fv(s_proj_loc, 1, GL_FALSE, G.proj.m);
    glUniform1i(s_tex_loc, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    for (const SixelPlacement &pl : tit->second.placements) {
        if (!pl.img.tex) continue;
        float vis_row = (float)(pl.y_cell + t->sb_offset);
        if (vis_row + pl.rows <= 0 || vis_row >= (float)t->rows) continue;

        float dx = ox + pl.x_cell * cw;
        float dy = oy + vis_row * ch;
        float dw = pl.cols * cw;
        float dh = pl.rows * ch;

        float verts[24] = {
            dx,      dy,      0.f, 0.f,
            dx + dw, dy,      1.f, 0.f,
            dx + dw, dy + dh, 1.f, 1.f,
            dx,      dy,      0.f, 0.f,
            dx + dw, dy + dh, 1.f, 1.f,
            dx,      dy + dh, 0.f, 1.f,
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glBindTexture(GL_TEXTURE_2D, pl.img.tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glUseProgram(0);
}

void sixel_clear(Terminal *t) {
    auto it = s_terms.find(t);
    if (it == s_terms.end()) return;
    for (auto &pl : it->second.placements) free_sixel_image(pl.img);
    it->second.placements.clear();
}

void sixel_scroll(Terminal *t, int lines) {
    auto it = s_terms.find(t);
    if (it == s_terms.end()) return;
    auto &pv = it->second.placements;
    for (auto &pl : pv) pl.y_cell -= lines;
    pv.erase(std::remove_if(pv.begin(), pv.end(), [](SixelPlacement &pl) {
        if (pl.y_cell + pl.rows <= 0) { free_sixel_image(pl.img); return true; }
        return false;
    }), pv.end());
}

void sixel_shutdown(void) {
    for (auto &kv : s_terms)
        for (auto &pl : kv.second.placements) free_sixel_image(pl.img);
    s_terms.clear();

    if (!g_use_sdl_renderer) {
        if (s_vao)  glDeleteVertexArrays(1, &s_vao);
        if (s_vbo)  glDeleteBuffers(1, &s_vbo);
        if (s_prog) glDeleteProgram(s_prog);
        s_vao = s_vbo = s_prog = 0;
    }
}
