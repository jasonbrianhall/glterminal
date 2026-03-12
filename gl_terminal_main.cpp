// gl_terminal_main.cpp
// Standalone OpenGL VT100 terminal
// Build: see Makefile
//
// Reuses your FreeType-to-triangles approach from cometbuster_render_gl.cpp
// Uses SDL2 for window + input (no GTK dependency)

#include <GL/glew.h>
#include <GL/gl.h>
#include <SDL2/SDL.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <vector>
#include <unordered_map>
#include <string>

// Pull in the embedded font
#include "DejaVuMonoBold.h"
#include "NotoEmoji.h"
#include "DejaVuMono.h"
#include "DejaVuMonoOblique.h"
#include "DejaVuMonoBoldOblique.h"
#include "gl_terminal.h"

// Additional attribute bits (extending gl_terminal.h's ATTR_* defines)
#define ATTR_STRIKE    (1<<5)
#define ATTR_OVERLINE  (1<<6)
#define ATTR_DIM       (1<<7)

static int   g_theme_idx   = 0;
static float g_opacity     = 1.0f;   // 0.0 = fully transparent, 1.0 = opaque
static bool  g_blink_text_on = true; // text blink visibility state
static SDL_Window *g_sdl_window = nullptr;  // set in main(), used by OSC title handler

// Active resolved palette (copied from theme on apply, so 256-colour cube still works)
static float g_palette16[16][3];

static void apply_theme(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) return;
    g_theme_idx = idx;
    const Theme &th = THEMES[idx];
    const float (*pal)[3] = th.palette ? th.palette : PAL_DEFAULT;
    // Special overrides for themes without a custom palette
    if (!th.palette) {
        if (strcmp(th.name,"Matrix")==0) {
            memcpy(g_palette16, PAL_DEFAULT, sizeof(g_palette16));
            // Make fg green
            g_palette16[2][0]=0.f; g_palette16[2][1]=1.f; g_palette16[2][2]=0.f;
            g_palette16[7][0]=0.f; g_palette16[7][1]=0.9f; g_palette16[7][2]=0.f;
        } else if (strcmp(th.name,"Ocean")==0) {
            memcpy(g_palette16, PAL_DEFAULT, sizeof(g_palette16));
            g_palette16[4][0]=0.4f; g_palette16[4][1]=0.7f; g_palette16[4][2]=1.0f;
            g_palette16[6][0]=0.4f; g_palette16[6][1]=0.9f; g_palette16[6][2]=1.0f;
            g_palette16[7][0]=0.85f;g_palette16[7][1]=0.92f;g_palette16[7][2]=1.0f;
        } else {
            memcpy(g_palette16, PAL_DEFAULT, sizeof(g_palette16));
        }
    } else {
        memcpy(g_palette16, pal, sizeof(g_palette16));
    }
}

static Mat4 mat4_ortho(float l, float r, float b, float t, float n, float f) {
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
        SDL_Log("[GL] shader error: %s\n", log);
    }
    return s;
}

static void gl_init_renderer(int w, int h) {
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

static void draw_verts(Vertex *v, int n, GLenum mode) {
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * sizeof(Vertex), v);
    glBindVertexArray(G.vao);
    glDrawArrays(mode, 0, n);
}

// Draw a filled rectangle using two triangles
static void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    Vertex v[6] = {
        {x,   y,   r,g,b,a}, {x+w, y,   r,g,b,a}, {x+w, y+h, r,g,b,a},
        {x,   y,   r,g,b,a}, {x+w, y+h, r,g,b,a}, {x,   y+h, r,g,b,a},
    };
    draw_verts(v, 6, GL_TRIANGLES);
}

// ============================================================================
// FREETYPE  (same approach as your code — rasterise bitmap to pixel quads)
// ============================================================================

static FT_Library s_ft_lib  = NULL;
// Four faces for the monospace font family:
// s_ft_face        = Bold        (Monospace.h — original embedded font)
// s_ft_face_reg    = Regular     (DejaVuMono.h)
// s_ft_face_obl    = Oblique     (DejaVuMonoOblique.h)
// s_ft_face_bobl   = BoldOblique (DejaVuMonoBoldOblique.h)
static FT_Face    s_ft_face      = NULL;  // Bold
static FT_Face    s_ft_face_reg  = NULL;  // Regular
static FT_Face    s_ft_face_obl  = NULL;  // Oblique
static FT_Face    s_ft_face_bobl = NULL;  // BoldOblique
static unsigned char *s_font_buf      = NULL;
static unsigned char *s_font_buf_reg  = NULL;
static unsigned char *s_font_buf_obl  = NULL;
static unsigned char *s_font_buf_bobl = NULL;

// Emoji fallback face — loaded from embedded NotoEmoji
static FT_Face    s_emoji_face = NULL;
static unsigned char *s_emoji_buf = NULL;

// Select the right face for given attrs
static FT_Face face_for_attrs(uint8_t attrs) {
    bool bold = (attrs & ATTR_BOLD)   != 0;
    bool ital = (attrs & ATTR_ITALIC) != 0;
    if (bold && ital) return s_ft_face_bobl ? s_ft_face_bobl : s_ft_face;
    if (bold)         return s_ft_face;       // Bold is the original Monospace.h
    if (ital)         return s_ft_face_obl  ? s_ft_face_obl  : s_ft_face_reg;
    return s_ft_face_reg ? s_ft_face_reg : s_ft_face;
}

static void ft_init(void) {
    FT_Init_FreeType(&s_ft_lib);

    size_t decoded_size = 0;
    int r = base64_decode(MONOSPACE_FONT_B64, MONOSPACE_FONT_B64_SIZE, &s_font_buf, &decoded_size);
    if (r != 0 || !s_font_buf) {
        SDL_Log("[Font] base64 decode failed\n");
        return;
    }

    FT_New_Memory_Face(s_ft_lib, s_font_buf, (FT_Long)decoded_size, 0, &s_ft_face);
    SDL_Log("[Font] loaded: %s %s\n", s_ft_face->family_name, s_ft_face->style_name);

    // Helper lambda to load an embedded variant
    auto load_variant = [&](const char *b64, size_t b64_size,
                            unsigned char **buf, FT_Face *face) {
        size_t sz = 0;
        if (base64_decode(b64, b64_size, buf, &sz) == 0 && *buf) {
            if (FT_New_Memory_Face(s_ft_lib, *buf, (FT_Long)sz, 0, face) == 0)
                SDL_Log("[Font] loaded: %s %s\n", (*face)->family_name, (*face)->style_name);
            else { free(*buf); *buf = nullptr; }
        }
    };
    load_variant(DEJAVU_REGULAR_FONT_B64,     DEJAVU_REGULAR_FONT_B64_SIZE,     &s_font_buf_reg,  &s_ft_face_reg);
    load_variant(DEJAVU_OBLIQUE_FONT_B64,     DEJAVU_OBLIQUE_FONT_B64_SIZE,     &s_font_buf_obl,  &s_ft_face_obl);
    load_variant(DEJAVU_BOLDOBLIQUE_FONT_B64, DEJAVU_BOLDOBLIQUE_FONT_B64_SIZE, &s_font_buf_bobl, &s_ft_face_bobl);

    // Load embedded NotoEmoji
    size_t emoji_decoded_size = 0;
    int er = base64_decode(NOTOEMOJI_FONT_B64, NOTOEMOJI_FONT_B64_SIZE, &s_emoji_buf, &emoji_decoded_size);
    if (er == 0 && s_emoji_buf) {
        if (FT_New_Memory_Face(s_ft_lib, s_emoji_buf, (FT_Long)emoji_decoded_size, 0, &s_emoji_face) == 0)
            SDL_Log("[Font] emoji: %s %s\n", s_emoji_face->family_name, s_emoji_face->style_name);
        else {
            SDL_Log("[Font] emoji face load failed\n");
            free(s_emoji_buf); s_emoji_buf = nullptr;
        }
    } else {
        SDL_Log("[Font] emoji base64 decode failed\n");
    }
}

// Decode a single UTF-8 sequence, return codepoint, advance *p
static uint32_t next_codepoint(const unsigned char **p) {
    uint32_t cp;
    unsigned char c = **p;
    if (c < 0x80)       { cp = c;                      *p += 1; }
    else if (c < 0xE0)  { cp = (c&0x1F)<<6  | ((*p)[1]&0x3F);             *p += 2; }
    else if (c < 0xF0)  { cp = (c&0x0F)<<12 | ((*p)[1]&0x3F)<<6 | ((*p)[2]&0x3F); *p += 3; }
    else                { cp = (c&0x07)<<18 | ((*p)[1]&0x3F)<<12| ((*p)[2]&0x3F)<<6|((*p)[3]&0x3F); *p += 4; }
    return cp;
}

// Returns true if the codepoint is in a known emoji range
static bool is_emoji_codepoint(uint32_t cp) {
    return (cp >= 0x2600  && cp <= 0x27BF)   // Misc symbols, dingbats
        || (cp >= 0x1F300 && cp <= 0x1FAFF)  // Main emoji block
        || (cp >= 0x1F000 && cp <= 0x1F02F)  // Mahjong/domino tiles
        || (cp >= 0xFE00  && cp <= 0xFE0F);  // Variation selectors
}

// Encode a codepoint as UTF-8 into buf (must be >= 5 bytes). Returns byte count.
static int cp_to_utf8(uint32_t cp, char *buf) {
    if (cp < 0x80)      { buf[0]=(char)cp; return 1; }
    if (cp < 0x800)     { buf[0]=(char)(0xC0|(cp>>6)); buf[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000)   { buf[0]=(char)(0xE0|(cp>>12)); buf[1]=(char)(0x80|((cp>>6)&0x3F)); buf[2]=(char)(0x80|(cp&0x3F)); return 3; }
    buf[0]=(char)(0xF0|(cp>>18)); buf[1]=(char)(0x80|((cp>>12)&0x3F)); buf[2]=(char)(0x80|((cp>>6)&0x3F)); buf[3]=(char)(0x80|(cp&0x3F)); return 4;
}

// Draw a single glyph from `face`. Handles both BGRA (color emoji) and grayscale bitmaps.
// Returns advance width, or 0 if glyph not found in this face.
static float draw_glyph(FT_Face face, uint32_t cp, float cx, float baseline_y,
                         int font_px, int emoji_px, float r, float g, float b, float a,
                         std::vector<Vertex> &verts) {
    float glyph_scale = 1.0f;
    if (face->num_fixed_sizes > 0) {
        int best = 0;
        int best_diff = abs(face->available_sizes[0].height - emoji_px);
        for (int si = 1; si < face->num_fixed_sizes; si++) {
            int diff = abs(face->available_sizes[si].height - emoji_px);
            if (diff < best_diff) { best_diff = diff; best = si; }
        }
        FT_Select_Size(face, best);
        glyph_scale = (float)emoji_px / (float)face->available_sizes[best].height;
    } else {
        // Use emoji_px for the emoji face so outline emoji fill the cell
        int req_px = (face == s_emoji_face) ? emoji_px : font_px;
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)req_px);
    }
    // Always request FT_LOAD_COLOR for the emoji face — NotoEmoji-Regular uses
    // COLRv0 outlines which render as BGRA (not grayscale) and require this flag.
    int load_flags = FT_LOAD_RENDER;
    if (face->num_fixed_sizes > 0 || face == s_emoji_face) load_flags |= FT_LOAD_COLOR;

    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (!gi) {
        if (cp > 0x7F) SDL_Log("[Glyph] cp=U+%04X not in face '%s'\n", cp, face->family_name);
        return 0.f;
    }
    int load_err = FT_Load_Glyph(face, gi, load_flags);
    if (load_err) {
        SDL_Log("[Glyph] cp=U+%04X FT_Load_Glyph failed err=%d flags=0x%x\n", cp, load_err, load_flags);
        return 0.f;
    }

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bm = &slot->bitmap;


    // COLRv1 / SVG glyphs load successfully but produce an empty bitmap —
    // treat as not-found so the caller can try another face.
    if (bm->width == 0 || bm->rows == 0) return 0.f;

    float render_scale = glyph_scale;

    float drawn_w = bm->width * render_scale;
    float drawn_h = bm->rows  * render_scale;
    float gx, gy;
    if (face->num_fixed_sizes > 0) {
        // Fixed-size (bitmap) emoji: center scaled bitmap in cell
        gx = cx + (font_px - drawn_w) * 0.5f;
        gy = baseline_y - drawn_h;
    } else {
        gx = cx + slot->bitmap_left;
        gy = baseline_y - slot->bitmap_top;
    }

    if (bm->pixel_mode == FT_PIXEL_MODE_BGRA) {
        // Color emoji (NotoColorEmoji etc) — 4 bytes per pixel: B, G, R, A
        for (int row = 0; row < (int)bm->rows; row++) {
            for (int col = 0; col < (int)bm->width; col++) {
                const unsigned char *px = bm->buffer + row * bm->pitch + col * 4;
                float pb = px[0]/255.f, pg = px[1]/255.f, pr = px[2]/255.f, pa = px[3]/255.f * a;
                if (pa < 0.01f) continue;
                float ex = gx + col * render_scale;
                float ey = gy + row * render_scale;
                float ps = render_scale < 1.f ? 1.f : render_scale;
                verts.push_back({ex,      ey,      pr,pg,pb,pa});
                verts.push_back({ex+ps,   ey,      pr,pg,pb,pa});
                verts.push_back({ex+ps,   ey+ps,   pr,pg,pb,pa});
                verts.push_back({ex,      ey,      pr,pg,pb,pa});
                verts.push_back({ex+ps,   ey+ps,   pr,pg,pb,pa});
                verts.push_back({ex,      ey+ps,   pr,pg,pb,pa});
            }
        }
    } else {
        for (int row = 0; row < (int)bm->rows; row++) {
            for (int col = 0; col < (int)bm->width; col++) {
                unsigned char pv = bm->buffer[row * bm->pitch + col];
                if (pv < 1) continue;
                // Use pv as alpha — but boost faint antialiased pixels so they
                // are actually visible. Gamma-expand: fa = (pv/255)^0.5
                float fa = a * sqrtf(pv / 255.f);
                float ex = gx + col * render_scale;
                float ey = gy + row * render_scale;
                float ps = render_scale < 1.f ? 1.f : render_scale;
                verts.push_back({ex,     ey,     r,g,b,fa});
                verts.push_back({ex+ps,  ey,     r,g,b,fa});
                verts.push_back({ex+ps,  ey+ps,  r,g,b,fa});
                verts.push_back({ex,     ey,     r,g,b,fa});
                verts.push_back({ex+ps,  ey+ps,  r,g,b,fa});
                verts.push_back({ex,     ey+ps,  r,g,b,fa});
            }
        }
    }
    return (float)(slot->advance.x >> 6) * render_scale;
}

// Draw a string at pixel position using FreeType bitmap → triangles (your technique)
static float draw_text(const char *text, float x, float y, int font_px, int emoji_px,
                       float r, float g, float b, float a, uint8_t attrs = 0) {
    if (!s_ft_face || !text || !*text) return x;

    static std::vector<Vertex> verts;
    verts.clear();

    FT_Face base_face = face_for_attrs(attrs);

    float cx = x;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_codepoint(&p);
        if (!cp) continue;
        if (cp == 0xFE0F) continue;

        FT_Face face = base_face;
        if (s_emoji_face && is_emoji_codepoint(cp)) {
            if (FT_Get_Char_Index(s_emoji_face, cp)) face = s_emoji_face;
        }

        // For grayscale emoji (outline faces, num_fixed_sizes==0), force white so
        // they're visible on dark backgrounds. BGRA color emoji ignore r,g,b anyway.
        bool is_grayscale_emoji = (face == s_emoji_face && face->num_fixed_sizes == 0);
        float er = is_grayscale_emoji ? 1.f : r;
        float eg = is_grayscale_emoji ? 1.f : g;
        float eb = is_grayscale_emoji ? 1.f : b;

        float adv = draw_glyph(face, cp, cx, y, font_px, emoji_px, er, eg, eb, a, verts);
        if (adv == 0.f) {
            // Try emoji face as fallback
            if (face != s_emoji_face && s_emoji_face)
                adv = draw_glyph(s_emoji_face, cp, cx, y, font_px, emoji_px, 1.f, 1.f, 1.f, a, verts);
        }
        if (adv == 0.f) {
            // Last resort: space advance so chars don't stack
            FT_Set_Pixel_Sizes(base_face, 0, (FT_UInt)font_px);
            FT_UInt gi = FT_Get_Char_Index(base_face, ' ');
            if (!FT_Load_Glyph(base_face, gi, FT_LOAD_DEFAULT))
                adv = (float)(base_face->glyph->advance.x >> 6);
        }
        cx += adv;
    }

    if (!verts.empty())
        draw_verts(verts.data(), (int)verts.size(), GL_TRIANGLES);

    return cx;
}

// Measure text width without drawing
[[maybe_unused]] static float measure_text(const char *text, int font_px) {
    if (!s_ft_face || !text) return 0;
    FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)font_px);
    float w = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_codepoint(&p);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, cp);
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT))
            w += (float)(s_ft_face->glyph->advance.x >> 6);
    }
    return w;
}

// Color: palette index (0-255) or 24-bit RGB packed as 0x01RRGGBB
// High byte 0x00 = palette index in low byte
// High byte 0x01 = 24-bit RGB in low 3 bytes
typedef uint32_t TermColorVal;
#define TCOLOR_PALETTE(idx)    ((TermColorVal)(idx))
#define TCOLOR_RGB(r,g,b)      ((TermColorVal)(0x01000000u | ((r)<<16) | ((g)<<8) | (b)))
#define TCOLOR_IS_RGB(c)       (((c) & 0xFF000000u) == 0x01000000u)
#define TCOLOR_R(c)            (((c)>>16)&0xFF)
#define TCOLOR_G(c)            (((c)>>8)&0xFF)
#define TCOLOR_B(c)            ((c)&0xFF)
#define TCOLOR_IDX(c)          ((c)&0xFF)

struct TermColor { float r,g,b; };

// Resolve a TermColorVal to float RGB
static TermColor tcolor_resolve(TermColorVal c) {
    if (TCOLOR_IS_RGB(c))
        return { TCOLOR_R(c)/255.f, TCOLOR_G(c)/255.f, TCOLOR_B(c)/255.f };
    // 256-color palette
    int idx = (int)TCOLOR_IDX(c);
    // System 16: resolved from g_palette16 (set by apply_theme)
    if (idx < 16) return { g_palette16[idx][0], g_palette16[idx][1], g_palette16[idx][2] };
    // 216-color cube (indices 16-231)
    if (idx < 232) {
        int i = idx - 16;
        int b = i % 6, g = (i/6) % 6, r = i/36;
        auto cv = [](int v) { return v ? (55 + v*40)/255.f : 0.f; };
        return { cv(r), cv(g), cv(b) };
    }
    // Grayscale ramp (indices 232-255)
    float v = (8 + (idx-232)*10) / 255.f;
    return { v, v, v };
}

struct Cell {
    uint32_t    cp;
    TermColorVal fg, bg;
    uint8_t     attrs, _pad[3];
};

typedef enum { PS_NORMAL, PS_ESC, PS_CSI, PS_OSC, PS_CHARSET } ParseState;

struct Terminal {
    Cell         *cells;
    int           cols, rows;
    int           cur_row, cur_col;
    TermColorVal  cur_fg, cur_bg;
    uint8_t       cur_attrs;
    ParseState    state;
    char          csi[256];
    int           csi_len;
    char          osc[512];
    int           osc_len;
    int           pty_fd;
    pid_t         child;
    float         cell_w, cell_h;
    double      blink;
    double      cursor_blink;          // independent cursor blink accumulator
    bool        cursor_on;             // cursor blink visibility
    bool        cursor_blink_enabled;  // false = always on
    int         cursor_shape;          // 0=block, 1=underline bar, 2=beam
    bool        autowrap;              // DECSET ?7 — default on
    bool        mouse_report;          // DECSET ?1000
    bool        bracketed_paste;       // DECSET ?2004
    bool        app_cursor_keys;       // DECSET ?1 — sends \eOA instead of \e[A
    bool        mouse_sgr;             // DECSET ?1006 — SGR mouse encoding
    // ESC 7 / ESC 8 saved cursor
    int         saved7_row, saved7_col;
    TermColorVal saved7_fg, saved7_bg;
    uint8_t     saved7_attrs;
    // Scroll region (DECSTBM) — rows are 0-based inclusive
    int         scroll_top, scroll_bot;
    // Alternate screen buffer (for ?1049h/l)
    Cell        *alt_cells;       // saved normal screen, nullptr if not in alt
    int          saved_cur_row, saved_cur_col;
    TermColorVal saved_cur_fg, saved_cur_bg;
    uint8_t      saved_cur_attrs;
    bool         in_alt_screen;
    // Scrollback buffer — ring buffer of rows
    Cell        *sb_buf;        // flat: [sb_cap * cols]
    int          sb_cap;        // max rows in scrollback (e.g. 5000)
    int          sb_head;       // index of oldest row (ring head)
    int          sb_count;      // number of rows stored (0..sb_cap)
    int          sb_offset;     // viewport offset: 0=live, N=N rows back
    // Selection
    int         sel_start_row, sel_start_col;
    int         sel_end_row,   sel_end_col;
    bool        sel_active;
    bool        sel_exists;
};

// Accessor macro - replaces CELL(t,r,c)
#define CELL(t,r,c) ((t)->cells[(r)*(t)->cols+(c)])

// Forward declaration for term_paste
static void term_write(Terminal *t, const char *s, int n);

// ============================================================================
// SELECTION HELPERS
// ============================================================================

// Convert pixel coords to cell row/col, clamped to grid
static void pixel_to_cell(Terminal *t, int px, int py, int ox, int oy,
                           int *row, int *col) {
    *col = (int)((px - ox) / t->cell_w);
    *row = (int)((py - oy) / t->cell_h);
    if (*col < 0) *col = 0;
    if (*row < 0) *row = 0;
    if (*col >= t->cols) *col = t->cols - 1;
    if (*row >= t->rows) *row = t->rows - 1;
}

// Is cell (r,c) inside the current selection? (order-independent)
static bool cell_in_sel(Terminal *t, int r, int c) {
    if (!t->sel_exists && !t->sel_active) return false;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    // Normalise so r0,c0 <= r1,c1
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }
    if (r < r0 || r > r1) return false;
    if (r == r0 && c < c0) return false;
    if (r == r1 && c > c1) return false;
    return true;
}

// Copy selection to clipboard
static void term_copy_selection(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;

    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }

    // Worst case: every cell is one UTF-8 char + newline per row
    int bufsize = (r1 - r0 + 1) * (t->cols + 1) + 1;
    char *buf = (char*)malloc(bufsize);
    int pos = 0;

    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;

        // Find last non-space on this line segment for trimming
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = CELL(t,r,c).cp;
            if (cp && cp != ' ') last_nonspace = c;
        }

        for (int c = cs; c <= last_nonspace; c++) {
            uint32_t cp = CELL(t,r,c).cp;
            if (!cp) cp = ' ';
            // Encode as UTF-8
            if (cp < 0x80) {
                buf[pos++] = (char)cp;
            } else if (cp < 0x800) {
                buf[pos++] = (char)(0xC0 | (cp>>6));
                buf[pos++] = (char)(0x80 | (cp&0x3F));
            } else if (cp < 0x10000) {
                buf[pos++] = (char)(0xE0 | (cp>>12));
                buf[pos++] = (char)(0x80 | ((cp>>6)&0x3F));
                buf[pos++] = (char)(0x80 | (cp&0x3F));
            } else {
                buf[pos++] = (char)(0xF0 | (cp>>18));
                buf[pos++] = (char)(0x80 | ((cp>>12)&0x3F));
                buf[pos++] = (char)(0x80 | ((cp>>6)&0x3F));
                buf[pos++] = (char)(0x80 | (cp&0x3F));
            }
        }
        if (r < r1) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    SDL_SetClipboardText(buf);
    free(buf);
    SDL_Log("[Term] copied %d chars to clipboard\n", pos);
}

// Paste clipboard contents to PTY
static void term_paste(Terminal *t) {
    if (!SDL_HasClipboardText()) return;
    char *text = SDL_GetClipboardText();
    if (text && text[0]) {
        if (t->bracketed_paste) term_write(t, "\x1b[200~", 6);
        term_write(t, text, (int)strlen(text));
        if (t->bracketed_paste) term_write(t, "\x1b[201~", 6);
    }
    SDL_free(text);
}

// Helper: append a hex colour like "#rrggbb" to a std::string
static void append_hex_color(std::string &s, float r, float g, float b) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x",
             (int)(r*255+.5f), (int)(g*255+.5f), (int)(b*255+.5f));
    s += buf;
}

// Helper: HTML-escape a single codepoint into s
static void append_html_char(std::string &s, uint32_t cp) {
    if      (cp == '<')  s += "&lt;";
    else if (cp == '>')  s += "&gt;";
    else if (cp == '&')  s += "&amp;";
    else if (cp == '"')  s += "&quot;";
    else if (cp < 0x80)  s += (char)cp;
    else if (cp < 0x800) {
        s += (char)(0xC0|(cp>>6));
        s += (char)(0x80|(cp&0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0|(cp>>12));
        s += (char)(0x80|((cp>>6)&0x3F));
        s += (char)(0x80|(cp&0x3F));
    } else {
        s += (char)(0xF0|(cp>>18));
        s += (char)(0x80|((cp>>12)&0x3F));
        s += (char)(0x80|((cp>>6)&0x3F));
        s += (char)(0x80|(cp&0x3F));
    }
}

// Copy selection to clipboard as an HTML <span> fragment preserving colours and bold.
static void term_copy_selection_html(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;

    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }

    // Resolve theme background for the outer div
    const Theme &th = THEMES[g_theme_idx];
    char bg_hex[8];
    snprintf(bg_hex, sizeof(bg_hex), "#%02x%02x%02x",
             (int)(th.bg_r*255+.5f), (int)(th.bg_g*255+.5f), (int)(th.bg_b*255+.5f));

    std::string html;
    html.reserve(4096);
    html += "<div style=\"background:";
    html += bg_hex;
    html += ";font-family:monospace;white-space:pre;padding:4px;display:inline-block\">";

    // Track the current span state so we only open/close when attributes change
    TermColorVal last_fg = ~(TermColorVal)0, last_bg = ~(TermColorVal)0;
    uint8_t last_attrs = 0xFF;
    bool span_open = false;

    auto close_span = [&]() {
        if (span_open) { html += "</span>"; span_open = false; }
    };

    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;

        // Trim trailing spaces on each line
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = CELL(t,r,c).cp;
            if (cp && cp != ' ') last_nonspace = c;
        }

        for (int c = cs; c <= last_nonspace; c++) {
            Cell &cell = CELL(t,r,c);
            uint32_t cp = cell.cp ? cell.cp : ' ';

            TermColorVal fg = cell.fg, bg = cell.bg;
            uint8_t attrs = cell.attrs;
            if (attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }

            // Open a new span if any attribute changed
            if (fg != last_fg || bg != last_bg || attrs != last_attrs) {
                close_span();

                TermColor fc = tcolor_resolve(fg);
                TermColor bc = tcolor_resolve(bg);

                html += "<span style=\"color:";
                append_hex_color(html, fc.r, fc.g, fc.b);
                html += ";background:";
                append_hex_color(html, bc.r, bc.g, bc.b);
                if (attrs & ATTR_BOLD)      html += ";font-weight:bold";
                if (attrs & ATTR_ITALIC)    html += ";font-style:italic";
                if (attrs & ATTR_UNDERLINE)  html += ";text-decoration:underline";
                html += "\">";
                span_open = true;

                last_fg = fg; last_bg = bg; last_attrs = attrs;
            }

            append_html_char(html, cp);
        }
        close_span();
        last_fg = ~(TermColorVal)0; // force new span on next row
        if (r < r1) html += '\n';
    }
    html += "</div>";

    SDL_SetClipboardText(html.c_str());
    SDL_Log("[Term] copied %zu bytes of HTML to clipboard\n", html.size());
}

// Copy selection as plain text with ANSI escape codes preserving colours and bold.
static void term_copy_selection_ansi(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;

    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }

    std::string out;
    out.reserve(4096);

    TermColorVal last_fg = ~(TermColorVal)0, last_bg = ~(TermColorVal)0;
    uint8_t last_attrs = 0xFF;

    auto emit_sgr = [&](TermColorVal fg, TermColorVal bg, uint8_t attrs) {
        out += "\x1b[0"; // always reset first
        if (attrs & ATTR_BOLD)      out += ";1";
        if (attrs & ATTR_UNDERLINE) out += ";4";
        if (attrs & ATTR_BLINK)     out += ";5";

        // Foreground
        if (TCOLOR_IS_RGB(fg)) {
            char buf[32];
            snprintf(buf, sizeof(buf), ";38;2;%d;%d;%d",
                     (int)TCOLOR_R(fg), (int)TCOLOR_G(fg), (int)TCOLOR_B(fg));
            out += buf;
        } else {
            int idx = (int)TCOLOR_IDX(fg);
            if (idx < 8)        { char b[8]; snprintf(b,sizeof(b),";3%d",idx);     out+=b; }
            else if (idx < 16)  { char b[8]; snprintf(b,sizeof(b),";9%d",idx-8);   out+=b; }
            else                { char b[16]; snprintf(b,sizeof(b),";38;5;%d",idx); out+=b; }
        }

        // Background
        if (TCOLOR_IS_RGB(bg)) {
            char buf[32];
            snprintf(buf, sizeof(buf), ";48;2;%d;%d;%d",
                     (int)TCOLOR_R(bg), (int)TCOLOR_G(bg), (int)TCOLOR_B(bg));
            out += buf;
        } else {
            int idx = (int)TCOLOR_IDX(bg);
            if (idx < 8)        { char b[8]; snprintf(b,sizeof(b),";4%d",idx);      out+=b; }
            else if (idx < 16)  { char b[8]; snprintf(b,sizeof(b),";10%d",idx-8);   out+=b; }
            else                { char b[16]; snprintf(b,sizeof(b),";48;5;%d",idx);  out+=b; }
        }

        out += 'm';
        last_fg = fg; last_bg = bg; last_attrs = attrs;
    };

    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;

        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = CELL(t,r,c).cp;
            if (cp && cp != ' ') last_nonspace = c;
        }

        for (int c = cs; c <= last_nonspace; c++) {
            Cell &cell = CELL(t,r,c);
            uint32_t cp = cell.cp ? cell.cp : ' ';

            TermColorVal fg = cell.fg, bg = cell.bg;
            uint8_t attrs = cell.attrs;
            if (attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }

            if (fg != last_fg || bg != last_bg || attrs != last_attrs)
                emit_sgr(fg, bg, attrs);

            // UTF-8 encode
            if (cp < 0x80)      out += (char)cp;
            else if (cp < 0x800){ out+=(char)(0xC0|(cp>>6)); out+=(char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000){ out+=(char)(0xE0|(cp>>12)); out+=(char)(0x80|((cp>>6)&0x3F)); out+=(char)(0x80|(cp&0x3F)); }
            else { out+=(char)(0xF0|(cp>>18)); out+=(char)(0x80|((cp>>12)&0x3F)); out+=(char)(0x80|((cp>>6)&0x3F)); out+=(char)(0x80|(cp&0x3F)); }
        }

        // Reset at end of each line then newline
        out += "\x1b[0m";
        last_fg = ~(TermColorVal)0;
        if (r < r1) out += '\n';
    }

    SDL_SetClipboardText(out.c_str());
    SDL_Log("[Term] copied %zu bytes of ANSI to clipboard\n", out.size());
}

// Push one row into the scrollback ring buffer
static void sb_push(Terminal *t, int row) {
    SDL_Log("[Scroll] sb_push called: sb_buf=%p sb_cap=%d scroll_top=%d scroll_bot=%d rows=%d\n",
        (void*)t->sb_buf, t->sb_cap, t->scroll_top, t->scroll_bot, t->rows);
    if (!t->sb_buf || t->sb_cap == 0) return;
    // Only push when scroll region is full screen (not inside apps like vim)
    if (t->scroll_top != 0 || t->scroll_bot != t->rows - 1) return;
    int slot = (t->sb_head + t->sb_count) % t->sb_cap;
    memcpy(t->sb_buf + slot * t->cols, &CELL(t, row, 0), sizeof(Cell) * t->cols);
    if (t->sb_count < t->sb_cap) {
        t->sb_count++;
    } else {
        // Buffer full — advance head (oldest row evicted)
        t->sb_head = (t->sb_head + 1) % t->sb_cap;
    }
    if (t->sb_count <= 5 || t->sb_count % 50 == 0)
        SDL_Log("[Scroll] sb_push: sb_count now %d\n", t->sb_count);
}

// Get a scrollback row by logical index (0 = oldest, sb_count-1 = newest)
static Cell* sb_row(Terminal *t, int idx) {
    int slot = (t->sb_head + idx) % t->sb_cap;
    return t->sb_buf + slot * t->cols;
}

static void scroll_up(Terminal *t) {
    int top = t->scroll_top;
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    // Save the row being scrolled off into scrollback
    SDL_Log("[Scroll] scroll_up: top=%d bot=%d rows=%d\n", top, bot, t->rows);
    if (top == 0) sb_push(t, top);
    if (bot > top)
        memmove(&CELL(t,top,0), &CELL(t,top+1,0), sizeof(Cell)*t->cols*(bot-top));
    for (int c = 0; c < t->cols; c++)
        CELL(t,bot,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
}

static void scroll_down(Terminal *t) {
    int top = t->scroll_top;
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    if (bot > top)
        memmove(&CELL(t,top+1,0), &CELL(t,top,0), sizeof(Cell)*t->cols*(bot-top));
    for (int c = 0; c < t->cols; c++)
        CELL(t,top,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
}

static void newline(Terminal *t) {
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    if (t->cur_row < bot) { t->cur_row++; return; }
    if (t->cur_row == bot) { scroll_up(t); return; }
    if (t->cur_row < t->rows - 1) t->cur_row++;
}

static void sgr(Terminal *t, const char *p) {
    char buf[128]; strncpy(buf, p, 127); buf[127]='\0';
    int params[32]; int pc = 0;
    char *tok = strtok(buf, ";");
    while (tok && pc < 32) { params[pc++] = atoi(tok); tok = strtok(NULL, ";"); }
    if (!pc) { params[0]=0; pc=1; }

    for (int i = 0; i < pc; i++) {
        int v = params[i];
        if      (v == 0)  { t->cur_fg=TCOLOR_PALETTE(7); t->cur_bg=TCOLOR_PALETTE(0); t->cur_attrs=0; }
        else if (v == 1)  t->cur_attrs |= ATTR_BOLD;
        else if (v == 2)  t->cur_attrs |= ATTR_DIM;
        else if (v == 3)  t->cur_attrs |= ATTR_ITALIC;
        else if (v == 4)  t->cur_attrs |= ATTR_UNDERLINE;
        else if (v == 5)  t->cur_attrs |= ATTR_BLINK;
        else if (v == 7)  t->cur_attrs |= ATTR_REVERSE;
        else if (v == 9)  t->cur_attrs |= ATTR_STRIKE;
        else if (v == 22) t->cur_attrs &= ~(ATTR_BOLD | ATTR_DIM);
        else if (v == 23) t->cur_attrs &= ~ATTR_ITALIC;
        else if (v == 24) t->cur_attrs &= ~ATTR_UNDERLINE;
        else if (v == 25) t->cur_attrs &= ~ATTR_BLINK;
        else if (v == 27) t->cur_attrs &= ~ATTR_REVERSE;
        else if (v == 29) t->cur_attrs &= ~ATTR_STRIKE;
        else if (v == 53) t->cur_attrs |= ATTR_OVERLINE;
        else if (v == 55) t->cur_attrs &= ~ATTR_OVERLINE;
        // Foreground
        else if (v>=30 && v<=37)   t->cur_fg = TCOLOR_PALETTE(v-30);
        else if (v == 38) {
            if (i+1 < pc && params[i+1] == 5 && i+2 < pc) {
                // 256-color: 38;5;n
                t->cur_fg = TCOLOR_PALETTE(params[i+2] & 0xFF);
                i += 2;
            } else if (i+1 < pc && params[i+1] == 2 && i+4 < pc) {
                // 24-bit RGB: 38;2;r;g;b
                t->cur_fg = TCOLOR_RGB(params[i+2], params[i+3], params[i+4]);
                i += 4;
            }
        }
        else if (v == 39)            t->cur_fg = TCOLOR_PALETTE(7);
        // Background
        else if (v>=40 && v<=47)   t->cur_bg = TCOLOR_PALETTE(v-40);
        else if (v == 48) {
            if (i+1 < pc && params[i+1] == 5 && i+2 < pc) {
                // 256-color: 48;5;n
                t->cur_bg = TCOLOR_PALETTE(params[i+2] & 0xFF);
                i += 2;
            } else if (i+1 < pc && params[i+1] == 2 && i+4 < pc) {
                // 24-bit RGB: 48;2;r;g;b
                t->cur_bg = TCOLOR_RGB(params[i+2], params[i+3], params[i+4]);
                i += 4;
            }
        }
        else if (v == 49)            t->cur_bg = TCOLOR_PALETTE(0);
        // Bright fg/bg
        else if (v>=90 && v<=97)   t->cur_fg = TCOLOR_PALETTE(v-90+8);
        else if (v>=100 && v<=107) t->cur_bg = TCOLOR_PALETTE(v-100+8);
    }
}

static void dispatch_csi(Terminal *t) {
    if (!t->csi_len) return;
    char final = t->csi[t->csi_len-1];
    t->csi[t->csi_len-1] = '\0';
    const char *p = t->csi;
    switch (final) {
    case 'm': sgr(t, p); break;
    case 'H': case 'f': {
        int row=1,col=1; sscanf(p,"%d;%d",&row,&col);
        t->cur_row = SDL_clamp(row-1, 0, t->rows-1);
        t->cur_col = SDL_clamp(col-1, 0, t->cols-1);
        break;
    }
    case 'A': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_max(0,t->cur_row-n); break; }
    case 'B': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_min(t->rows-1,t->cur_row+n); break; }
    case 'C': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_min(t->cols-1,t->cur_col+n); break; }
    case 'D': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_max(0,t->cur_col-n); break; }
    case 'G': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_clamp(n-1,0,t->cols-1); break; }
    case 'J': {
        int n=atoi(p);
        if (n==2||n==3) {
            for(int r=0;r<t->rows;r++) for(int c=0;c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
            t->cur_row=t->cur_col=0;
        } else if(n==1) {
            for(int r=0;r<t->cur_row;r++) for(int c=0;c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        } else {
            for(int r=t->cur_row;r<t->rows;r++)
                for(int c=(r==t->cur_row?t->cur_col:0);c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        }
        break;
    }
    case 'K': {
        int n=atoi(p);
        int s=(n==1)?0:t->cur_col, e=(n==0)?t->cols:t->cur_col+1;
        for(int c=s;c<e&&c<t->cols;c++) CELL(t,t->cur_row,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        break;
    }
    case 'P': { // DCH delete chars
        int n=atoi(p); if(n<1)n=1;
        int r=t->cur_row, c=t->cur_col;
        memmove(&CELL(t,r,c), &CELL(t,r,c+n), sizeof(Cell)*(t->cols-c-n));
        for(int i=t->cols-n;i<t->cols;i++) CELL(t,r,i)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        break;
    }
    case 'h': case 'l': {
        bool set = (final == 'h');
        // Private modes start with '?'
        if (p[0] == '?') {
            int mode = atoi(p + 1);
            if (mode == 1049) {
                if (set && !t->in_alt_screen) {
                    // Enter alt screen: save current cells + cursor
                    int sz = t->rows * t->cols;
                    t->alt_cells = (Cell*)malloc(sizeof(Cell) * sz);
                    memcpy(t->alt_cells, t->cells, sizeof(Cell) * sz);
                    t->saved_cur_row   = t->cur_row;
                    t->saved_cur_col   = t->cur_col;
                    t->saved_cur_fg    = t->cur_fg;
                    t->saved_cur_bg    = t->cur_bg;
                    t->saved_cur_attrs = t->cur_attrs;
                    // Clear screen for alt buffer
                    for (int i = 0; i < sz; i++)
                        t->cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
                    t->cur_row = t->cur_col = 0;
                    t->scroll_top = 0; t->scroll_bot = t->rows - 1;
                    t->in_alt_screen = true;
                } else if (!set && t->in_alt_screen) {
                    // Exit alt screen: restore cells + cursor
                    int sz = t->rows * t->cols;
                    if (t->alt_cells) {
                        memcpy(t->cells, t->alt_cells, sizeof(Cell) * sz);
                        free(t->alt_cells);
                        t->alt_cells = nullptr;
                    }
                    t->cur_row   = t->saved_cur_row;
                    t->cur_col   = t->saved_cur_col;
                    t->cur_fg    = t->saved_cur_fg;
                    t->cur_bg    = t->saved_cur_bg;
                    t->cur_attrs = t->saved_cur_attrs;
                    t->scroll_top = 0; t->scroll_bot = t->rows - 1;
                    t->in_alt_screen = false;
                }
            } else if (mode == 25) {
                // DECTCEM — cursor visible/hidden
                if (!set) { t->cursor_on = false; t->cursor_blink_enabled = false; }
                else       { t->cursor_on = true;  t->cursor_blink_enabled = true;  }
            } else if (mode == 1) {
                // DECCKM — application cursor keys
                t->app_cursor_keys = set;
            } else if (mode == 12) {
                // Cursor blink on/off
                t->cursor_blink_enabled = set;
                if (!set) t->cursor_on = true;
            } else if (mode == 7) {
                // DECAWM — auto-wrap
                t->autowrap = set;
            } else if (mode == 1000 || mode == 1002 || mode == 1003) {
                // Mouse reporting (button/drag/any events)
                t->mouse_report = set;
                if (!set) t->mouse_sgr = false;
            } else if (mode == 1006) {
                // SGR mouse encoding — coordinates as decimal, handles >col 95
                t->mouse_sgr = set;
            } else if (mode == 2004) {
                // Bracketed paste
                t->bracketed_paste = set;
            }
        }
        break;
    }
    // DECSCUSR — set cursor style: CSI Ps SP q
    // Ps: 0,1=blinking block  2=steady block  3=blinking underline
    //     4=steady underline   5=blinking beam  6=steady beam
    case 'q': {
        int plen = (int)strlen(p);
        if (plen > 0 && p[plen-1] == ' ') {
            int ps = atoi(p);
            t->cursor_blink_enabled = (ps == 0 || ps == 1 || ps == 3 || ps == 5);
            if (ps == 0 || ps == 1 || ps == 2) t->cursor_shape = 0;       // block
            else if (ps == 3 || ps == 4)        t->cursor_shape = 1;       // underline
            else if (ps == 5 || ps == 6)        t->cursor_shape = 2;       // beam
            if (!t->cursor_blink_enabled) t->cursor_on = true;
        }
        break;
    }
    // Soft reset: CSI ! p
    case 'p': {
        if (p[0] == '!') {
            // DECSTR — soft reset
            t->cur_attrs        = 0;
            t->cur_fg           = TCOLOR_PALETTE(7);
            t->cur_bg           = TCOLOR_PALETTE(0);
            t->scroll_top       = 0;
            t->scroll_bot       = t->rows - 1;
            t->autowrap         = true;
            t->cursor_blink_enabled = true;
            t->cursor_on        = true;
            t->mouse_report     = false;
            t->mouse_sgr        = false;
            t->app_cursor_keys  = false;
            t->bracketed_paste  = false;
        }
        break;
    }
    // VPA — vertical position absolute (row, 1-based)
    case 'd': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_clamp(n-1,0,t->rows-1); break; }
    // HPA — horizontal position absolute (col, 1-based) — same as G
    case '`': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_clamp(n-1,0,t->cols-1); break; }
    // DECSTBM — set scroll region
    case 'r': {
        int top=1, bot=t->rows; sscanf(p,"%d;%d",&top,&bot);
        t->scroll_top = SDL_clamp(top-1, 0, t->rows-1);
        t->scroll_bot = SDL_clamp(bot-1, 0, t->rows-1);
        if (t->scroll_top >= t->scroll_bot) { t->scroll_top=0; t->scroll_bot=t->rows-1; }
        t->cur_row = t->scroll_top; t->cur_col = 0; // cursor goes home
        break;
    }
    // IL — insert lines
    case 'L': {
        int n=atoi(p); if(n<1)n=1;
        int bot=SDL_min(t->scroll_bot,t->rows-1);
        for(int i=0;i<n;i++) {
            if(bot>t->cur_row)
                memmove(&CELL(t,t->cur_row+1,0),&CELL(t,t->cur_row,0),sizeof(Cell)*t->cols*(bot-t->cur_row));
            for(int c=0;c<t->cols;c++) CELL(t,t->cur_row,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        }
        break;
    }
    // DL — delete lines
    case 'M': {
        int n=atoi(p); if(n<1)n=1;
        int bot=SDL_min(t->scroll_bot,t->rows-1);
        for(int i=0;i<n;i++) {
            if(bot>t->cur_row)
                memmove(&CELL(t,t->cur_row,0),&CELL(t,t->cur_row+1,0),sizeof(Cell)*t->cols*(bot-t->cur_row));
            for(int c=0;c<t->cols;c++) CELL(t,bot,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        }
        break;
    }
    // SU — scroll up N lines
    case 'S': { int n=atoi(p); if(n<1)n=1; for(int i=0;i<n;i++) scroll_up(t); break; }
    // SD — scroll down N lines
    case 'T': { int n=atoi(p); if(n<1)n=1; for(int i=0;i<n;i++) scroll_down(t); break; }
    // ECH — erase N chars at cursor
    case 'X': { int n=atoi(p); if(n<1)n=1;
        for(int c=t->cur_col;c<t->cur_col+n&&c<t->cols;c++) CELL(t,t->cur_row,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        break; }
    // ICH — insert N blank chars
    case '@': { int n=atoi(p); if(n<1)n=1;
        for(int c=t->cols-1;c>=t->cur_col+n;c--) CELL(t,t->cur_row,c)=CELL(t,t->cur_row,c-n);
        for(int c=t->cur_col;c<t->cur_col+n&&c<t->cols;c++) CELL(t,t->cur_row,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        break; }
    // DA — Primary Device Attributes
    case 'c': {
        if (p[0] == '\0' || p[0] == '0')
            term_write(t, "\x1b[?62;22c", 9);  // VT220, ANSI color
        else if (p[0] == '>')
            term_write(t, "\x1b[>0;10;1c", 10); // Secondary DA: VT100, version 10
        break;
    }
    // DSR — Device Status Report / CPR
    case 'n': {
        int n = atoi(p);
        if (n == 5) {
            term_write(t, "\x1b[0n", 4); // status: OK
        } else if (n == 6) {
            // Cursor Position Report: row and col are 1-based
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
                               t->cur_row + 1, t->cur_col + 1);
            term_write(t, buf, len);
        }
        break;
    }
    // ANSI save cursor (CSI s)
    case 's': {
        t->saved7_row   = t->cur_row;
        t->saved7_col   = t->cur_col;
        t->saved7_fg    = t->cur_fg;
        t->saved7_bg    = t->cur_bg;
        t->saved7_attrs = t->cur_attrs;
        break;
    }
    // ANSI restore cursor (CSI u)
    case 'u': {
        t->cur_row   = t->saved7_row;
        t->cur_col   = t->saved7_col;
        t->cur_fg    = t->saved7_fg;
        t->cur_bg    = t->saved7_bg;
        t->cur_attrs = t->saved7_attrs;
        break;
    }
    default: break;
    }
}

static void term_feed(Terminal *t, const char *buf, int len) {
    // UTF-8 decode state (carried across calls so multi-byte sequences
    // split across read() boundaries are handled correctly)
    static uint32_t utf8_cp    = 0;  // codepoint accumulator
    static int      utf8_left  = 0;  // continuation bytes still expected

    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];

        // ── UTF-8 assembly ──────────────────────────────────────────────────
        uint32_t ready_cp = 0;  // set to non-zero when a full codepoint is decoded
        if (t->state == PS_NORMAL && ch >= 0x80) {
            if (ch >= 0xF0 && ch <= 0xF7) {
                utf8_cp = ch & 0x07; utf8_left = 3;
            } else if (ch >= 0xE0 && ch <= 0xEF) {
                utf8_cp = ch & 0x0F; utf8_left = 2;
            } else if (ch >= 0xC2 && ch <= 0xDF) {
                utf8_cp = ch & 0x1F; utf8_left = 1;
            } else if (ch >= 0x80 && ch <= 0xBF && utf8_left > 0) {
                utf8_cp = (utf8_cp << 6) | (ch & 0x3F);
                utf8_left--;
                if (utf8_left == 0) ready_cp = utf8_cp;
            } else {
                // Invalid byte — reset
                utf8_cp = 0; utf8_left = 0;
            }
            if (!ready_cp) continue; // wait for more bytes
            // Full codepoint decoded — place it directly and move on
            if (t->cur_col >= t->cols) {
                if (t->autowrap) { t->cur_col=0; newline(t); }
                else t->cur_col = t->cols - 1;
            }
            CELL(t,t->cur_row,t->cur_col++) = {ready_cp, t->cur_fg, t->cur_bg, t->cur_attrs, {0,0,0}};
            continue;
        } else if (t->state == PS_NORMAL) {
            utf8_left = 0;
        }

        switch (t->state) {
        case PS_NORMAL:
            if      (ch == 0x1b) { t->state = PS_ESC; }
            else if (ch == '\r') { t->cur_col = 0; }
            else if (ch == '\n') { newline(t); }
            else if (ch == '\b') { if(t->cur_col>0) t->cur_col--; }
            else if (ch == '\t') { t->cur_col=(t->cur_col+8)&~7; if(t->cur_col>=t->cols)t->cur_col=t->cols-1; }
            else if (ch == 0x07) { /* BEL */ }
            else if (ch == 0x0e || ch == 0x0f) { /* charset shifts — ignore */ }
            else {
                uint32_t cp = (uint32_t)ch;
                if (cp >= 0x20) {
                    if (t->cur_col >= t->cols) {
                        if (t->autowrap) { t->cur_col=0; newline(t); }
                        else t->cur_col = t->cols - 1;
                    }
                    CELL(t,t->cur_row,t->cur_col++) = {cp, t->cur_fg, t->cur_bg, t->cur_attrs, {0,0,0}};
                }
            }
            break;
        case PS_ESC:
            if (ch == '[') {
                t->state=PS_CSI; t->csi_len=0; memset(t->csi,0,sizeof(t->csi));
            } else if (ch == ']') {
                t->state=PS_OSC;
            } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+') {
                // charset designator ESC ( X — skip the next byte (charset code)
                t->state = PS_CHARSET;
            } else {
                if (ch == '=' || ch == '>') {
                    // Application/normal keypad mode — ignore
                } else if (ch == '7') {
                    // DECSC — save cursor
                    t->saved7_row   = t->cur_row;
                    t->saved7_col   = t->cur_col;
                    t->saved7_fg    = t->cur_fg;
                    t->saved7_bg    = t->cur_bg;
                    t->saved7_attrs = t->cur_attrs;
                } else if (ch == '8') {
                    // DECRC — restore cursor
                    t->cur_row   = t->saved7_row;
                    t->cur_col   = t->saved7_col;
                    t->cur_fg    = t->saved7_fg;
                    t->cur_bg    = t->saved7_bg;
                    t->cur_attrs = t->saved7_attrs;
                } else if (ch == 'c') {
                    for(int r=0;r<t->rows;r++) for(int c=0;c<t->cols;c++) CELL(t,r,c)={' ',TCOLOR_PALETTE(7),TCOLOR_PALETTE(0),0,{0,0,0}};
                    t->cur_row=t->cur_col=0; t->cur_fg=7; t->cur_bg=0; t->cur_attrs=0;
                } else if (ch == 'M') {
                    // Reverse index — scroll down if at top of scroll region
                    if (t->cur_row > t->scroll_top) { t->cur_row--; }
                    else { scroll_down(t); }
                }
                t->state = PS_NORMAL;
            }
            break;
        case PS_CSI:
            if (ch >= 0x40 && ch <= 0x7e) {
                if (t->csi_len < (int)sizeof(t->csi)-1) t->csi[t->csi_len++] = (char)ch;
                t->csi[t->csi_len] = '\0';
                dispatch_csi(t);
                t->state = PS_NORMAL; t->csi_len = 0;
            } else if (ch >= 0x20 && ch < 0x40) {
                if (t->csi_len < (int)sizeof(t->csi)-1) t->csi[t->csi_len++] = (char)ch;
            } else {
                t->state = PS_NORMAL; t->csi_len = 0;
            }
            break;
        case PS_CHARSET:
            // Eat the charset code byte and return to normal
            t->state = PS_NORMAL;
            break;
        case PS_OSC:
            // Accumulate OSC payload until BEL or ST (ESC \)
            if (ch == 0x07 || ch == 0x1b) {
                t->osc[t->osc_len] = '\0';
                // Parse: "Ps;Pt"  — Ps 0,1,2 = set icon+title / icon / title
                const char *semi = strchr(t->osc, ';');
                if (semi) {
                    int ps = atoi(t->osc);
                    SDL_Log("[OSC] ps=%d title='%s'\n", ps, semi + 1);
                    if (ps == 0 || ps == 2) {
                        if (g_sdl_window)
                            SDL_SetWindowTitle(g_sdl_window, semi + 1);
                    }
                }
                t->osc_len = 0;
                t->state = (ch == 0x1b) ? PS_ESC : PS_NORMAL;
            } else {
                if (t->osc_len < (int)sizeof(t->osc) - 1)
                    t->osc[t->osc_len++] = (char)ch;
            }
            break;
        }
    }
}

// ============================================================================
// PTY
// ============================================================================

static bool term_spawn(Terminal *t, const char *cmd) {
    struct winsize ws = {
        .ws_row=(unsigned short)t->rows, .ws_col=(unsigned short)t->cols,
        .ws_xpixel=(unsigned short)(t->cols*(int)t->cell_w),
        .ws_ypixel=(unsigned short)(t->rows*(int)t->cell_h)
    };
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return false; }
    if (pid == 0) {
        char cols_str[16], rows_str[16];
        snprintf(cols_str, sizeof(cols_str), "%d", t->cols);
        snprintf(rows_str, sizeof(rows_str), "%d", t->rows);
        setenv("TERM","xterm-256color",1);
        setenv("COLUMNS", cols_str, 1);
        setenv("LINES",   rows_str, 1);
        const char *argv[] = {cmd, NULL};
        execvp(argv[0], (char*const*)argv);
        _exit(1);
    }
    t->pty_fd = master;
    t->child  = pid;
    int fl = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);
    SDL_Log("[Term] spawned shell pid=%d fd=%d\n", pid, master);
    return true;
}

static bool term_read(Terminal *t) {
    char buf[4096];
    bool got_data = false;
    for(;;) {
        ssize_t n = read(t->pty_fd, buf, sizeof(buf));
        if (n > 0) { term_feed(t, buf, (int)n); got_data = true; }
        else break;
    }
    return got_data;
}

static void term_write(Terminal *t, const char *s, int n) {
    if (t->pty_fd >= 0) { ssize_t r = write(t->pty_fd, s, n); (void)r; }
}


// ============================================================================
// CONTEXT MENU
// ============================================================================

#define MENU_ID_NEW_TERMINAL  0
#define MENU_ID_COPY          2
#define MENU_ID_COPY_HTML     3
#define MENU_ID_COPY_ANSI     4
#define MENU_ID_PASTE         5
#define MENU_ID_RESET         7
#define MENU_ID_THEMES        9
#define MENU_ID_OPACITY      10
#define MENU_ID_QUIT         12

struct MenuItem {
    const char *label;
    bool        separator;
};

static const MenuItem MENU_ITEMS[] = {
    { "New Terminal",    false },   // 0
    { nullptr,           true  },   // 1
    { "Copy",            false },   // 2
    { "Copy as HTML",    false },   // 3
    { "Copy as ANSI",    false },   // 4
    { "Paste",           false },   // 5
    { nullptr,           true  },   // 6
    { "Reset",           false },   // 7
    { nullptr,           true  },   // 8
    { "Color Theme  >",  false },   // 9
    { "Transparency  >", false },   // 10
    { nullptr,           true  },   // 11
    { "Quit",            false },   // 12
};
static const int MENU_COUNT = (int)(sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0]));

// Opacity presets
static const float OPACITY_LEVELS[] = { 1.0f, 0.85f, 0.7f, 0.5f, 0.3f, 0.1f };
static const char* OPACITY_NAMES[]  = { "100%", "85%", "70%", "50%", "30%", "10%" };
static const int   OPACITY_COUNT    = 6;

struct ContextMenu {
    bool  visible;
    int   x, y;
    int   hovered;
    int   item_h, sep_h, pad_x, width;
    // Submenu state
    int   sub_open;    // MENU_ID of open submenu, -1 = none
    int   sub_x, sub_y, sub_w, sub_h;
    int   sub_hovered;
};

static ContextMenu g_menu = {};

static void menu_layout(ContextMenu *m, int font_size) {
    m->item_h = (int)(font_size * 1.8f);
    m->sep_h  = 8;
    m->pad_x  = (int)(font_size * 0.8f);
    m->width  = (int)(font_size * 14.0f);
}

static int menu_total_height(ContextMenu *m) {
    int h = 4; // top padding
    for (int i = 0; i < MENU_COUNT; i++)
        h += MENU_ITEMS[i].separator ? m->sep_h : m->item_h;
    h += 4; // bottom padding
    return h;
}

// Returns item index under pixel (px,py), or -1
static int menu_hit(ContextMenu *m, int px, int py) {
    if (!m->visible) return -1;
    if (px < m->x || px > m->x + m->width) return -1;
    int y = m->y + 4;
    for (int i = 0; i < MENU_COUNT; i++) {
        int h = MENU_ITEMS[i].separator ? m->sep_h : m->item_h;
        if (!MENU_ITEMS[i].separator && py >= y && py < y + h) return i;
        y += h;
    }
    return -1;
}

static void menu_open(ContextMenu *m, int x, int y, int win_w, int win_h) {
    menu_layout(m, g_font_size);
    m->visible   = true;
    m->hovered   = -1;
    m->sub_open  = -1;
    m->sub_hovered = -1;
    int th = menu_total_height(m);
    m->x = SDL_min(x, win_w - m->width - 2);
    m->y = SDL_min(y, win_h - th - 2);
    if (m->x < 0) m->x = 0;
    if (m->y < 0) m->y = 0;
}

// Returns which sub-item is hit in the open submenu, or -1
static int submenu_hit(ContextMenu *m, int px, int py) {
    if (m->sub_open < 0) return -1;
    if (px < m->sub_x || px > m->sub_x + m->sub_w) return -1;
    if (py < m->sub_y || py > m->sub_y + m->sub_h) return -1;
    int count = (m->sub_open == MENU_ID_THEMES) ? THEME_COUNT : OPACITY_COUNT;
    int idx = (py - m->sub_y) / m->item_h;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

static void draw_menu_panel(float mx, float my, float mw, float mh) {
    draw_rect(mx+3, my+3, mw, mh, 0,0,0, 0.35f);                          // shadow
    draw_rect(mx, my, mw, mh, 0.13f, 0.13f, 0.16f, 0.96f);               // bg
    draw_rect(mx, my, mw, 1, 0.35f,0.35f,0.45f, 1.f);                    // border top
    draw_rect(mx, my+mh-1, mw, 1, 0.35f,0.35f,0.45f, 1.f);              // border bot
    draw_rect(mx, my, 1, mh, 0.35f,0.35f,0.45f, 1.f);                   // border left
    draw_rect(mx+mw-1, my, 1, mh, 0.35f,0.35f,0.45f, 1.f);             // border right
}

static void menu_render(ContextMenu *m) {
    if (!m->visible) return;
    menu_layout(m, g_font_size);
    int th = menu_total_height(m);
    float mx = (float)m->x, my = (float)m->y;
    float mw = (float)m->width;

    draw_menu_panel(mx, my, mw, (float)th);

    float y = my + 4;
    for (int i = 0; i < MENU_COUNT; i++) {
        if (MENU_ITEMS[i].separator) {
            draw_rect(mx+4, y + m->sep_h*0.5f, mw-8, 1, 0.35f,0.35f,0.45f, 1.f);
            y += m->sep_h;
            continue;
        }
        float ih = (float)m->item_h;
        bool hov = (i == m->hovered);
        (void)(i == MENU_ID_THEMES || i == MENU_ID_OPACITY); // sub-menu parent
        bool sub_open = (m->sub_open == i);

        if (hov || sub_open)
            draw_rect(mx+2, y, mw-4, ih, 0.25f, 0.45f, 0.85f, 0.85f);

        float tr = (hov || sub_open) ? 1.f : 0.88f;
        float tg = (hov || sub_open) ? 1.f : 0.88f;
        float tb = (hov || sub_open) ? 1.f : 0.92f;
        draw_text(MENU_ITEMS[i].label, mx + m->pad_x, y + ih*0.72f,
                  g_font_size, g_font_size, tr, tg, tb, 1.f);

        y += ih;
    }

    // ── Draw submenu ──────────────────────────────────────────────────────────
    if (m->sub_open == MENU_ID_THEMES || m->sub_open == MENU_ID_OPACITY) {
        int count   = (m->sub_open == MENU_ID_THEMES) ? THEME_COUNT : OPACITY_COUNT;
        float sw    = (float)(m->width + (int)(g_font_size * 2));
        float sh    = (float)(count * m->item_h + 8);
        float sx    = (float)m->sub_x;
        float sy    = (float)m->sub_y;
        m->sub_w    = (int)sw; m->sub_h = (int)sh;

        draw_menu_panel(sx, sy, sw, sh);

        for (int j = 0; j < count; j++) {
            const char *lbl = (m->sub_open == MENU_ID_THEMES)
                              ? THEMES[j].name : OPACITY_NAMES[j];
            float iy = sy + 4 + j * m->item_h;
            float ih = (float)m->item_h;
            bool  hov = (j == m->sub_hovered);
            bool  active = (m->sub_open == MENU_ID_THEMES)
                           ? (j == g_theme_idx)
                           : (fabsf(OPACITY_LEVELS[j]-g_opacity)<0.01f);

            if (hov)    draw_rect(sx+2, iy, sw-4, ih, 0.25f,0.45f,0.85f, 0.85f);
            if (active && !hov) draw_rect(sx+2, iy, sw-4, ih, 0.2f,0.35f,0.6f, 0.6f);

            float tr = hov ? 1.f : (active ? 0.7f : 0.88f);
            float tg = hov ? 1.f : (active ? 0.9f : 0.88f);
            float tb = hov ? 1.f : (active ? 1.0f : 0.92f);
            if (active) {
                draw_text("\xe2\x9c\x93", sx + 4, iy + ih*0.72f, g_font_size, g_font_size, 0.4f,0.8f,0.4f,1.f);
            }
            draw_text(lbl, sx + m->pad_x + g_font_size, iy + ih*0.72f,
                      g_font_size, g_font_size, tr, tg, tb, 1.f);
        }
    }
}

// ============================================================================
// MENU ACTIONS
// ============================================================================

// Spawn a new terminal window as a detached child process
static void action_new_terminal() {
    // Re-exec ourselves
    extern char **environ; (void)environ;
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(self, self, nullptr);
        _exit(1);
    }
    // parent continues normally; don't waitpid on this child
}

// ============================================================================
// RENDERING
// ============================================================================

static void term_render(Terminal *t, int ox, int oy) {
    float cw = t->cell_w, ch = t->cell_h;
    bool scrolled = (t->sb_offset > 0);

    // Helper to resolve a cell pointer for a given row/col
    Cell blank = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    auto resolve_cell = [&](int row, int col) -> Cell* {
        if (scrolled) {
            int sb_row_idx = t->sb_count - t->sb_offset + row;
            if (sb_row_idx < 0) return &blank;
            if (sb_row_idx < t->sb_count) return sb_row(t, sb_row_idx) + col;
            int live_row = sb_row_idx - t->sb_count;
            return (live_row < t->rows) ? &CELL(t, live_row, col) : &blank;
        }
        return &CELL(t, row, col);
    };

    // Pass 1: draw all backgrounds first so no bg rect can paint over a glyph
    // that spills outside its cell (e.g. wide emoji).
    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor bc = tcolor_resolve(bg);
            if (!scrolled && cell_in_sel(t, row, col)) {
                draw_rect(px, py, cw, ch, 0.3f, 0.5f, 1.0f, 0.5f);
            } else {
                draw_rect(px, py, cw, ch, bc.r, bc.g, bc.b, 1.f);
            }
        }
    }

    // Pass 2: draw all glyphs and decorations on top
    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell(row, col);

            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor fc = tcolor_resolve(fg);
            if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
                fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg)+8));
            if (c->attrs & ATTR_DIM) { fc.r *= 0.5f; fc.g *= 0.5f; fc.b *= 0.5f; }

            uint32_t cp = c->cp;
            bool blink_hidden = (c->attrs & ATTR_BLINK) && !g_blink_text_on;
            if (cp && cp != ' ' && !blink_hidden) {
                char tmp[5] = {};
                cp_to_utf8(cp, tmp);
                float baseline = py + ch * 0.82f;
                draw_text(tmp, px, baseline, g_font_size, (int)ch, fc.r, fc.g, fc.b, 1.f, c->attrs);
            }

            if ((c->attrs & ATTR_UNDERLINE) && !blink_hidden)
                draw_rect(px, py+ch-2, cw, 2, fc.r, fc.g, fc.b, 1.f);
            if ((c->attrs & ATTR_STRIKE) && !blink_hidden)
                draw_rect(px, py+ch*0.45f, cw, 1, fc.r, fc.g, fc.b, 1.f);
            if ((c->attrs & ATTR_OVERLINE) && !blink_hidden)
                draw_rect(px, py+1, cw, 1, fc.r, fc.g, fc.b, 1.f);
        }
    }

    // Cursor — only when live
    if (!scrolled && t->cursor_on) {
        float cx = ox + t->cur_col * cw;
        float cy = oy + t->cur_row * ch;
        switch (t->cursor_shape) {
        case 0: // block
            draw_rect(cx, cy, cw, ch, 1,1,1, 0.3f);
            break;
        case 2: // beam (1px left edge)
            draw_rect(cx, cy, 2, ch, 1,1,1, 0.85f);
            break;
        default: // 1 = underline bar (original)
            draw_rect(cx, cy+ch-3, cw, 3, 1,1,1, 0.85f);
            break;
        }
    }

    // Scrollbar on right edge when scrolled
    if (scrolled && t->sb_count > 0) {
        float win_h   = t->rows * ch;
        int   total_rows = t->sb_count + t->rows;
        float bar_h   = win_h * t->rows / total_rows;
        if (bar_h < 8) bar_h = 8;
        float bar_y   = oy + (win_h - bar_h) * (float)(total_rows - t->rows - t->sb_offset) / (total_rows - t->rows);
        float bar_x   = ox + t->cols * cw - 4;
        draw_rect(bar_x, oy, 4, win_h, 0,0,0, 0.3f);
        draw_rect(bar_x, bar_y, 4, bar_h, 0.6f, 0.6f, 0.7f, 0.8f);
    }
}

// ============================================================================
// INIT
// ============================================================================

static void term_init(Terminal *t) {
    memset(t, 0, sizeof(*t));
    t->cur_fg    = 7;
    t->pty_fd    = -1;
    t->child     = -1;
    t->state     = PS_NORMAL;
    t->cursor_on            = true;
    t->cursor_blink_enabled = true;
    t->cursor_shape         = 1;   // underline bar (original behaviour)
    t->autowrap             = true;
    t->mouse_report         = false;
    t->bracketed_paste      = false;
    t->app_cursor_keys      = false;
    t->mouse_sgr            = false;
    t->saved7_row = t->saved7_col = 0;
    t->saved7_fg  = TCOLOR_PALETTE(7);
    t->saved7_bg  = TCOLOR_PALETTE(0);
    t->saved7_attrs = 0;
    t->osc_len              = 0;

    // Measure cell size from monospace '0' glyph
    if (s_ft_face) {
        FT_Set_Pixel_Sizes(s_ft_face, 0, g_font_size);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, '0');
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT)) {
            t->cell_w = (float)(s_ft_face->glyph->advance.x >> 6);
            t->cell_h = (float)(int)(g_font_size * 1.4f);
        }
    }
    if (t->cell_w < 1) t->cell_w = 10;
    if (t->cell_h < 1) t->cell_h = 20;

    t->sb_cap = SCROLLBACK_LINES;
    t->cols   = TERM_COLS_DEFAULT;
    t->rows   = TERM_ROWS_DEFAULT;

    // Allocate scrollback buffer
    t->sb_buf = (Cell*)calloc(t->sb_cap * t->cols, sizeof(Cell));

    // Allocate initial grid
    t->cells = (Cell*)malloc(sizeof(Cell) * t->cols * t->rows);
    for (int i = 0; i < t->cols * t->rows; i++)
        t->cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};

    SDL_Log("[Term] init: %dx%d cells %.0fx%.0f px\n", t->cols, t->rows, t->cell_w, t->cell_h);
}

// Resize terminal: reallocate grid, notify PTY
static void term_resize(Terminal *t, int win_w, int win_h) {
    int new_cols = (int)((win_w - 4) / t->cell_w);
    int new_rows = (int)((win_h - 4) / t->cell_h);
    if (new_cols < 2)  new_cols = 2;
    if (new_rows < 2)  new_rows = 2;
    if (new_cols > TERM_MAX_COLS) new_cols = TERM_MAX_COLS;
    if (new_rows > TERM_MAX_ROWS) new_rows = TERM_MAX_ROWS;
    if (new_cols == t->cols && new_rows == t->rows) return;

    // Allocate new grid and copy as much content as fits
    Cell *new_cells = (Cell*)malloc(sizeof(Cell) * new_cols * new_rows);
    for (int i = 0; i < new_cols * new_rows; i++)
        new_cells[i] = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};

    int copy_rows = (t->rows < new_rows) ? t->rows : new_rows;
    int copy_cols = (t->cols < new_cols) ? t->cols : new_cols;
    for (int r = 0; r < copy_rows; r++)
        for (int c = 0; c < copy_cols; c++)
            new_cells[r * new_cols + c] = CELL(t, r, c);

    free(t->cells);
    t->cells = new_cells;

    // Discard alt screen buffer on resize (simpler than trying to reflow it)
    if (t->alt_cells) { free(t->alt_cells); t->alt_cells = nullptr; t->in_alt_screen = false; }

    // Reallocate scrollback for new column count (lose content on col change — acceptable)
    if (t->sb_buf) free(t->sb_buf);
    t->sb_buf   = (Cell*)calloc(t->sb_cap * new_cols, sizeof(Cell));
    t->sb_head  = 0;
    t->sb_count = 0;
    t->sb_offset = 0;
    t->cols  = new_cols;
    t->rows  = new_rows;

    // Clamp cursor
    if (t->cur_row >= t->rows) t->cur_row = t->rows - 1;
    if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;

    // Tell the PTY about the new size
    if (t->pty_fd >= 0) {
        struct winsize ws = {
            .ws_row    = (unsigned short)new_rows,
            .ws_col    = (unsigned short)new_cols,
            .ws_xpixel = (unsigned short)(new_cols * (int)t->cell_w),
            .ws_ypixel = (unsigned short)(new_rows * (int)t->cell_h),
        };
        ioctl(t->pty_fd, TIOCSWINSZ, &ws);
    }

    t->scroll_top = 0;
    t->scroll_bot = new_rows - 1;
    SDL_Log("[Term] resized to %dx%d\n", new_cols, new_rows);
}

// Change font size, remeasure cells, reflow grid to fill the same window
static void term_set_font_size(Terminal *t, int new_size, int win_w, int win_h) {
    if (new_size < FONT_SIZE_MIN) new_size = FONT_SIZE_MIN;
    if (new_size > FONT_SIZE_MAX) new_size = FONT_SIZE_MAX;
    if (new_size == g_font_size) return;

    g_font_size = new_size;

    // Remeasure cell from font
    if (s_ft_face) {
        FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)g_font_size);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, '0');
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT)) {
            t->cell_w = (float)(s_ft_face->glyph->advance.x >> 6);
            t->cell_h = (float)(int)(g_font_size * 1.4f);
        }
    }
    if (t->cell_w < 1) t->cell_w = 6;
    if (t->cell_h < 1) t->cell_h = 8;

    // Reflow grid to new cell size within same window
    term_resize(t, win_w, win_h);
    SDL_Log("[Term] font size %d, cell %.0fx%.0f, grid %dx%d\n",
            g_font_size, t->cell_w, t->cell_h, t->cols, t->rows);
}

// ============================================================================
// KEYBOARD → VT100 SEQUENCES
// ============================================================================

static void handle_key(Terminal *t, SDL_Keysym ks, const char *text) {
    SDL_Keymod mod = (SDL_Keymod)ks.mod;
    bool shift = (mod & KMOD_SHIFT) != 0;
    bool ctrl  = (mod & KMOD_CTRL)  != 0;
    bool alt   = (mod & KMOD_ALT)   != 0;

    // Ctrl+Shift shortcuts (copy/paste — handled before this function for copy,
    // but paste lives here)
    if (ctrl && shift) {
        if (ks.sym == SDLK_v) { term_paste(t); return; }
        if (ks.sym == SDLK_c) { return; } // copy handled in event loop
    }

    // Arrow keys — respect application cursor key mode and modifiers
    // Modifier code: 1=plain,2=shift,3=alt,4=alt+shift,5=ctrl,6=ctrl+shift,7=ctrl+alt
    auto arrow = [&](const char *normal, const char *app, char letter) {
        if (!shift && !ctrl && !alt) {
            term_write(t, t->app_cursor_keys ? app : normal,
                          t->app_cursor_keys ? 3   : 3);
        } else {
            int m = 1 + (shift?1:0) + (alt?2:0) + (ctrl?4:0);
            char seq[16];
            int n = snprintf(seq, sizeof(seq), "\x1b[1;%d%c", m, letter);
            term_write(t, seq, n);
        }
    };

    switch (ks.sym) {
    case SDLK_UP:    arrow("\x1b[A", "\x1bOA", 'A'); return;
    case SDLK_DOWN:  arrow("\x1b[B", "\x1bOB", 'B'); return;
    case SDLK_RIGHT: arrow("\x1b[C", "\x1bOC", 'C'); return;
    case SDLK_LEFT:  arrow("\x1b[D", "\x1bOD", 'D'); return;
    case SDLK_HOME:
        term_write(t, t->app_cursor_keys ? "\x1bOH" : "\x1b[H", 3); return;
    case SDLK_END:
        term_write(t, t->app_cursor_keys ? "\x1bOF" : "\x1b[F", 3); return;
    default: break;
    }

    switch (ks.sym) {
    case SDLK_RETURN:    term_write(t, "\r",      1); break;
    case SDLK_BACKSPACE: term_write(t, "\x7f",    1); break;
    case SDLK_TAB:
        if (shift) term_write(t, "\x1b[Z", 3);   // Shift+Tab = backtab
        else       term_write(t, "\t",     1);
        break;
    case SDLK_ESCAPE:    term_write(t, "\x1b",    1); break;
    case SDLK_INSERT:    term_write(t, "\x1b[2~", 4); break;
    case SDLK_DELETE:    term_write(t, "\x1b[3~", 4); break;
    case SDLK_PAGEUP:    term_write(t, "\x1b[5~", 4); break;
    case SDLK_PAGEDOWN:  term_write(t, "\x1b[6~", 4); break;
    // Function keys F1-F4 (SS3 sequences)
    case SDLK_F1:  term_write(t, "\x1bOP",   3); break;
    case SDLK_F2:  term_write(t, "\x1bOQ",   3); break;
    case SDLK_F3:  term_write(t, "\x1bOR",   3); break;
    case SDLK_F4:  term_write(t, "\x1bOS",   3); break;
    // Function keys F5-F12 (CSI ~ sequences)
    case SDLK_F5:  term_write(t, "\x1b[15~", 5); break;
    case SDLK_F6:  term_write(t, "\x1b[17~", 5); break;
    case SDLK_F7:  term_write(t, "\x1b[18~", 5); break;
    case SDLK_F8:  term_write(t, "\x1b[19~", 5); break;
    case SDLK_F9:  term_write(t, "\x1b[20~", 5); break;
    case SDLK_F10: term_write(t, "\x1b[21~", 5); break;
    case SDLK_F11: term_write(t, "\x1b[23~", 5); break;
    case SDLK_F12: term_write(t, "\x1b[24~", 5); break;
    default:
        if (ctrl && ks.sym >= SDLK_a && ks.sym <= SDLK_z) {
            char c = (char)(ks.sym - SDLK_a + 1);
            if (alt) { term_write(t, "\x1b", 1); } // Alt sends ESC prefix
            term_write(t, &c, 1);
        } else if (alt && text && text[0]) {
            // Alt+printable = ESC prefix
            term_write(t, "\x1b", 1);
            term_write(t, text, (int)strlen(text));
        } else if (text && text[0]) {
            term_write(t, text, (int)strlen(text));
        }
        break;
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    const char *shell = (argc > 1) ? argv[1] : "/bin/bash";

    SDL_Init(SDL_INIT_VIDEO);

    // GL attributes MUST be set before SDL_CreateWindow
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    int win_w = 800, win_h = 480;
    SDL_Window *window = SDL_CreateWindow(
        WIN_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    g_sdl_window = window;
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, ctx);
    SDL_GL_SetSwapInterval(1);

    // Init theme (must be after globals are set up)
    apply_theme(0);

    ft_init();

    Terminal term;
    term_init(&term);

    // Use a sensible default window size (80x24 at cell size)
    win_w = (int)(term.cell_w * term.cols) + 4;
    win_h = (int)(term.cell_h * term.rows) + 4;
    SDL_SetWindowSize(window, win_w, win_h);
    SDL_GetWindowSize(window, &win_w, &win_h);

    gl_init_renderer(win_w, win_h);
    glViewport(0, 0, win_w, win_h);

    // Sync grid to actual window size immediately (in case WM overrode our size)
    term_resize(&term, win_w, win_h);
    SDL_Log("[Scroll] after resize: sb_cap=%d sb_buf=%p sb_count=%d\n",
        term.sb_cap, (void*)term.sb_buf, term.sb_count);

    if (!term_spawn(&term, shell)) {
        SDL_Log("[Term] Failed to spawn shell. Exiting.\n");
        return 1;
    }

    // Give bash ~200ms to write its prompt
    SDL_Delay(200);
    term_read(&term);
    // Clear any garbage that arrived during init (e.g. from .bashrc sequences)
    // without sending reset to the shell — just wipe our grid and home the cursor
    for (int i = 0; i < term.rows * term.cols; i++)
        term.cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    term.cur_row = term.cur_col = 0;
    term.scroll_top = 0; term.scroll_bot = term.rows - 1;
    term.state = PS_NORMAL;
    // Now ask bash to redraw its prompt
    term_write(&term, "\n", 1);
    SDL_Delay(100);
    term_read(&term);

    uint32_t last_ticks = SDL_GetTicks();
    bool running = true;

    while (running) {
        uint32_t now = SDL_GetTicks();
        double dt = (now - last_ticks) / 1000.0;
        last_ticks = now;

        bool needs_render = false;

        // Text blink (500ms)
        term.blink += dt;
        if (term.blink >= 0.5) {
            term.blink = 0;
            g_blink_text_on = !g_blink_text_on;
            needs_render = true;
        }

        // Cursor blink (independent 600ms period)
        if (term.cursor_blink_enabled) {
            term.cursor_blink += dt;
            if (term.cursor_blink >= 0.6) {
                term.cursor_blink = 0;
                term.cursor_on    = !term.cursor_on;
                needs_render = true;
            }
        }

        // Read PTY (clears selection if output arrives)
        {
            bool had_sel = term.sel_exists || term.sel_active;
            int old_sb_count = term.sb_count;
            bool got_data = term_read(&term);
            if (got_data) {
                needs_render = true;
                // Only snap back to live view if new lines were actually pushed to
                // scrollback (i.e. real output scrolled the terminal), not just
                // cursor movement or in-place redraws (e.g. prompt, vim, etc.)
                bool new_lines = (term.sb_count != old_sb_count);
                if (new_lines) {
                    if (term.sb_offset != 0)
                        SDL_Log("[Scroll] PTY new_lines reset sb_offset %d->0\n", term.sb_offset);
                    term.sb_offset = 0;
                    if (had_sel) { term.sel_exists = false; term.sel_active = false; }
                }
            }
        }

        // Check if child exited
        int status;
        if (waitpid(term.child, &status, WNOHANG) == term.child) {
            SDL_Log("[Term] Shell exited.\n");
            running = false;
        }

        // Events
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            needs_render = true;
            switch (ev.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN: {
                if (g_menu.visible) { g_menu.visible = false; break; }
                SDL_Keymod mod = SDL_GetModState();
                // Scrollback navigation: Shift+PageUp/Down only
                {
                    int page = term.rows - 1;
                    if (ev.key.keysym.sym == SDLK_PAGEUP && (mod & KMOD_SHIFT)) {
                        SDL_Log("[Scroll] Shift+PageUp: sb_count=%d sb_offset %d->%d\n",
                            term.sb_count, term.sb_offset,
                            SDL_min(term.sb_offset + page, term.sb_count));
                        term.sb_offset = SDL_min(term.sb_offset + page, term.sb_count);
                        break;
                    }
                    if (ev.key.keysym.sym == SDLK_PAGEDOWN && (mod & KMOD_SHIFT)) {
                        SDL_Log("[Scroll] Shift+PageDown: sb_count=%d sb_offset %d->%d\n",
                            term.sb_count, term.sb_offset,
                            SDL_max(term.sb_offset - page, 0));
                        term.sb_offset = SDL_max(term.sb_offset - page, 0);
                        break;
                    }
                }
                // Any other key snaps back to live view
                term.sb_offset = 0;
                if (mod & KMOD_CTRL) {
                    if (ev.key.keysym.sym == SDLK_c && term.sel_exists) {
                        // Ctrl+C with selection = copy (no SIGINT)
                        term_copy_selection(&term);
                        break;
                    }
                    if (ev.key.keysym.sym == SDLK_v) {
                        term_paste(&term);
                        break;
                    }
                    if (ev.key.keysym.sym == SDLK_LSHIFT ||
                        ev.key.keysym.sym == SDLK_RSHIFT) break;
                }
                handle_key(&term, ev.key.keysym, NULL);
                break;
            }
            case SDL_TEXTINPUT: {
                SDL_Keymod mod = SDL_GetModState();
                if (!(mod & KMOD_CTRL) && !(mod & KMOD_ALT))
                    term_write(&term, ev.text.text, (int)strlen(ev.text.text));
                break;
            }
            // Mouse selection
            case SDL_MOUSEBUTTONDOWN: {
                int r, c;
                pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2, &r, &c);
                // Report mouse if active (right-click goes to app, not our menu)
                if (term.mouse_report && !g_menu.visible) {
                    int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                              (ev.button.button == SDL_BUTTON_MIDDLE) ? 1 :
                              (ev.button.button == SDL_BUTTON_RIGHT)  ? 2 : -1;
                    if (btn >= 0) {
                        char seq[32]; int slen;
                        if (term.mouse_sgr)
                            slen = snprintf(seq, sizeof(seq), "\x1b[<%d;%d;%dM", btn, c+1, r+1);
                        else
                            slen = snprintf(seq, sizeof(seq), "\x1b[M%c%c%c",
                                            (char)(32+btn), (char)(32+c+1), (char)(32+r+1));
                        term_write(&term, seq, slen);
                    }
                    break;
                }
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    SDL_Log("[Menu] right-click at %d,%d win %dx%d\n", ev.button.x, ev.button.y, win_w, win_h);
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    menu_open(&g_menu, ev.button.x, ev.button.y, win_w, win_h);
                    SDL_Log("[Menu] opened at %d,%d visible=%d\n", g_menu.x, g_menu.y, g_menu.visible);
                } else if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (g_menu.visible) {
                        int sub_hit = submenu_hit(&g_menu, ev.button.x, ev.button.y);
                        if (sub_hit >= 0) {
                            if (g_menu.sub_open == MENU_ID_THEMES)       apply_theme(sub_hit);
                            else if (g_menu.sub_open == MENU_ID_OPACITY) {
                                g_opacity = OPACITY_LEVELS[sub_hit];
                                SDL_SetWindowOpacity(window, g_opacity);
                            }
                            g_menu.visible = false;
                        } else {
                            int hit = menu_hit(&g_menu, ev.button.x, ev.button.y);
                            bool is_sub_parent = (hit==MENU_ID_THEMES||hit==MENU_ID_OPACITY);
                            if (!is_sub_parent) g_menu.visible = false;
                            switch (hit) {
                            case MENU_ID_NEW_TERMINAL: action_new_terminal(); break;
                            case MENU_ID_COPY:      term_copy_selection(&term); break;
                            case MENU_ID_COPY_HTML: term_copy_selection_html(&term); break;
                            case MENU_ID_COPY_ANSI: term_copy_selection_ansi(&term); break;
                            case MENU_ID_PASTE:     term_paste(&term); break;
                            case MENU_ID_RESET:
                                for(int r=0;r<term.rows;r++)
                                    for(int c=0;c<term.cols;c++)
                                        CELL(&term,r,c)={' ',TCOLOR_PALETTE(7),TCOLOR_PALETTE(0),0,{0,0,0}};
                                term.cur_row=term.cur_col=0;
                                term.scroll_top=0; term.scroll_bot=term.rows-1;
                                term.state=PS_NORMAL;
                                term_write(&term,"reset\n",6);
                                break;
                            case MENU_ID_QUIT: running = false; break;
                            default:
                                if (hit < 0) g_menu.visible = false;
                                break;
                            }
                        }
                    } else {
                        term.sel_start_row = term.sel_end_row = r;
                        term.sel_start_col = term.sel_end_col = c;
                        term.sel_active = true;
                        term.sel_exists = false;
                    }
                } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    if (g_menu.visible) g_menu.visible = false;
                    else term_paste(&term);
                }
                break;
            }
            case SDL_MOUSEMOTION:
                if (g_menu.visible) {
                    int hit = menu_hit(&g_menu, ev.motion.x, ev.motion.y);
                    if (hit >= 0) g_menu.hovered = hit;
                    // Open submenu on hover over submenu items
                    if (hit == MENU_ID_THEMES || hit == MENU_ID_OPACITY) {
                        if (g_menu.sub_open != hit) {
                            g_menu.sub_open = hit;
                            g_menu.sub_hovered = -1;
                            // Position submenu to the right of parent
                            g_menu.sub_x = g_menu.x + g_menu.width + 2;
                            // Align vertically with hovered item
                            int item_y = g_menu.y + 4;
                            for (int i=0;i<hit;i++)
                                item_y += MENU_ITEMS[i].separator ? g_menu.sep_h : g_menu.item_h;
                            g_menu.sub_y = item_y;
                            // Clamp right edge
                            int count = (hit==MENU_ID_THEMES)?THEME_COUNT:OPACITY_COUNT;
                            int sw = g_menu.width + g_font_size*2;
                            int sh = count * g_menu.item_h + 8;
                            SDL_GetWindowSize(window, &win_w, &win_h);
                            if (g_menu.sub_x + sw > win_w) g_menu.sub_x = g_menu.x - sw - 2;
                            if (g_menu.sub_y + sh > win_h) g_menu.sub_y = win_h - sh - 2;
                        }
                    } else if (hit >= 0) {
                        g_menu.sub_open = -1;
                    }
                    // Track submenu hover
                    g_menu.sub_hovered = submenu_hit(&g_menu, ev.motion.x, ev.motion.y);
                } else if (term.sel_active && (ev.motion.state & SDL_BUTTON_LMASK)) {
                    pixel_to_cell(&term, ev.motion.x, ev.motion.y, 2, 2,
                                  &term.sel_end_row, &term.sel_end_col);
                    term.sel_exists = true;
                }
                break;
            case SDL_MOUSEBUTTONUP: {
                int r, c;
                pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2, &r, &c);
                if (term.mouse_report && !g_menu.visible) {
                    int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                              (ev.button.button == SDL_BUTTON_MIDDLE) ? 1 :
                              (ev.button.button == SDL_BUTTON_RIGHT)  ? 2 : -1;
                    if (btn >= 0) {
                        char seq[32]; int slen;
                        if (term.mouse_sgr)
                            slen = snprintf(seq, sizeof(seq), "\x1b[<%d;%d;%dm", btn, c+1, r+1); // lowercase m = release
                        else
                            slen = snprintf(seq, sizeof(seq), "\x1b[M%c%c%c",
                                            (char)(32+3), (char)(32+c+1), (char)(32+r+1));
                        term_write(&term, seq, slen);
                    }
                } else if (ev.button.button == SDL_BUTTON_LEFT && term.sel_active) {
                    pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2,
                                  &term.sel_end_row, &term.sel_end_col);
                    term.sel_active = false;
                    // Only keep selection if user actually dragged
                    bool same = (term.sel_start_row == term.sel_end_row &&
                                 term.sel_start_col == term.sel_end_col);
                    term.sel_exists = !same;
                    if (term.sel_exists) term_copy_selection(&term); // auto-copy on release
                }
                break;
            }
            case SDL_MOUSEWHEEL: {
                SDL_Keymod mod = SDL_GetModState();
                if (mod & KMOD_CTRL) {
                    int delta = (ev.wheel.y > 0) ? 1 : -1;
                    if (mod & KMOD_SHIFT) delta *= 4;
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    term_set_font_size(&term, g_font_size + delta, win_w, win_h);
                    G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
                } else if (term.mouse_report) {
                    // Send wheel as mouse buttons 64 (up) / 65 (down) in SGR or X10
                    int r, c;
                    pixel_to_cell(&term, ev.motion.x, ev.motion.y, 2, 2, &r, &c);
                    int btn = (ev.wheel.y > 0) ? 64 : 65;
                    char seq[32]; int slen;
                    if (term.mouse_sgr)
                        slen = snprintf(seq, sizeof(seq), "\x1b[<%d;%d;%dM", btn, c+1, r+1);
                    else
                        slen = snprintf(seq, sizeof(seq), "\x1b[M%c%c%c",
                                        (char)(32+btn), (char)(32+c+1), (char)(32+r+1));
                    term_write(&term, seq, slen);
                } else {
                    // Scroll up (y>0) = go back in history = increase offset
                    int delta = (ev.wheel.y > 0) ? 3 : -3;
                    int new_off = SDL_clamp(term.sb_offset + delta, 0, term.sb_count);
                    SDL_Log("[Scroll] MouseWheel y=%d delta=%d sb_count=%d sb_offset %d->%d\n",
                        ev.wheel.y, delta, term.sb_count, term.sb_offset, new_off);
                    term.sb_offset = new_off;
                }
                break;
            }
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    glViewport(0, 0, win_w, win_h);
                    G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
                    term_resize(&term, win_w, win_h);
                }
                break;
            }
        }

        // Render only when something changed
        if (needs_render) {
            glClearColor(
                THEMES[g_theme_idx].bg_r,
                THEMES[g_theme_idx].bg_g,
                THEMES[g_theme_idx].bg_b,
                g_opacity);
            glClear(GL_COLOR_BUFFER_BIT);

            glViewport(0, 0, win_w, win_h);
            term_render(&term, 2, 2);   // 2px margin
            menu_render(&g_menu);

            SDL_GL_SwapWindow(window);
        }

        // Frame cap: sleep any remaining time to target ~60fps
        // (vsync may not work on all platforms/virtual displays)
        uint32_t frame_ms = SDL_GetTicks() - now;
        if (frame_ms < 16) SDL_Delay(16 - frame_ms);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (s_emoji_face)    FT_Done_Face(s_emoji_face);
    if (s_ft_face_bobl)  FT_Done_Face(s_ft_face_bobl);
    if (s_ft_face_obl)   FT_Done_Face(s_ft_face_obl);
    if (s_ft_face_reg)   FT_Done_Face(s_ft_face_reg);
    if (s_ft_face)       FT_Done_Face(s_ft_face);
    if (s_ft_lib)        FT_Done_FreeType(s_ft_lib);
    if (s_emoji_buf)     free(s_emoji_buf);
    if (s_font_buf_bobl) free(s_font_buf_bobl);
    if (s_font_buf_obl)  free(s_font_buf_obl);
    if (s_font_buf_reg)  free(s_font_buf_reg);
    if (s_font_buf)      free(s_font_buf);

    return 0;
}
