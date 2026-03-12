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

// Pull in the embedded font
#include "Monospace.h"

// ============================================================================
// CONFIG
// ============================================================================

#define TERM_COLS_DEFAULT  80
#define TERM_ROWS_DEFAULT  24
#define TERM_MAX_COLS      512
#define TERM_MAX_ROWS      256
#define FONT_SIZE_DEFAULT  16
#define FONT_SIZE_MIN      6
#define FONT_SIZE_MAX     72
#define WIN_TITLE       "GL Terminal"

static int g_font_size = FONT_SIZE_DEFAULT;

// ============================================================================
// VERTEX / GL STATE  (same pattern as cometbuster_render_gl.cpp)
// ============================================================================

typedef struct { float x, y, r, g, b, a; } Vertex;

typedef struct {
    float m[16];
} Mat4;

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

#define MAX_VERTS 400000

struct GLState {
    GLuint prog, vao, vbo;
    GLint  proj_loc;
    Mat4   proj;
    float  cr, cg, cb, ca;   // current colour
};
static GLState G = {};

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
static FT_Face    s_ft_face = NULL;
static unsigned char *s_font_buf = NULL;

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

// Draw a string at pixel position using FreeType bitmap → triangles (your technique)
static float draw_text(const char *text, float x, float y, int font_px,
                       float r, float g, float b, float a) {
    if (!s_ft_face || !text || !*text) return x;

    FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)font_px);

    static std::vector<Vertex> verts;
    verts.clear();

    float cx = x;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_codepoint(&p);
        if (!cp) continue;

        FT_UInt gi = FT_Get_Char_Index(s_ft_face, cp);
        if (FT_Load_Glyph(s_ft_face, gi, FT_LOAD_RENDER)) continue;

        FT_GlyphSlot slot = s_ft_face->glyph;
        FT_Bitmap *bm = &slot->bitmap;

        float gx = cx + slot->bitmap_left;
        float gy = y  - slot->bitmap_top;

        for (int row = 0; row < (int)bm->rows; row++) {
            for (int col = 0; col < (int)bm->width; col++) {
                unsigned char pv = bm->buffer[row * bm->pitch + col];
                if (pv < 4) continue;
                float fa = a * (pv / 255.f);
                float px = gx + col, py = gy + row;
                verts.push_back({px,      py,      r,g,b,fa});
                verts.push_back({px+1.f,  py,      r,g,b,fa});
                verts.push_back({px+1.f,  py+1.f,  r,g,b,fa});
                verts.push_back({px,      py,      r,g,b,fa});
                verts.push_back({px+1.f,  py+1.f,  r,g,b,fa});
                verts.push_back({px,      py+1.f,  r,g,b,fa});
            }
        }
        cx += (float)(slot->advance.x >> 6);
    }

    if (!verts.empty())
        draw_verts(verts.data(), (int)verts.size(), GL_TRIANGLES);

    return cx;
}

// Measure text width without drawing
static float measure_text(const char *text, int font_px) {
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

// ============================================================================
// TERMINAL DATA
// ============================================================================

#define ATTR_BOLD      (1<<0)
#define ATTR_UNDERLINE (1<<1)
#define ATTR_REVERSE   (1<<2)
#define ATTR_BLINK     (1<<3)

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
    // System 16
    static const TermColor P16[16] = {
        {0.f,    0.f,    0.f},
        {0.8f,   0.1f,   0.1f},
        {0.1f,   0.8f,   0.1f},
        {0.8f,   0.8f,   0.1f},
        {0.2f,   0.2f,   0.9f},
        {0.8f,   0.1f,   0.8f},
        {0.1f,   0.8f,   0.8f},
        {0.75f,  0.75f,  0.75f},
        {0.4f,   0.4f,   0.4f},
        {1.f,    0.3f,   0.3f},
        {0.3f,   1.f,    0.3f},
        {1.f,    1.f,    0.3f},
        {0.3f,   0.4f,   1.f},
        {1.f,    0.3f,   1.f},
        {0.3f,   1.f,    1.f},
        {1.f,    1.f,    1.f},
    };
    if (idx < 16) return P16[idx];
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
    int           pty_fd;
    pid_t         child;
    float         cell_w, cell_h;
    double      blink;
    bool        cursor_on;
    // Scroll region (DECSTBM) — rows are 0-based inclusive
    int         scroll_top, scroll_bot;   // default: 0, rows-1
    // Selection
    int         sel_start_row, sel_start_col;
    int         sel_end_row,   sel_end_col;
    bool        sel_active;    // mouse is down, selection in progress
    bool        sel_exists;    // a completed selection exists
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
        // Send bracketed paste if we wanted to, but plain paste is fine for VT100
        term_write(t, text, (int)strlen(text));
    }
    SDL_free(text);
}

// ============================================================================
// PARSER
// ============================================================================

static void scroll_up(Terminal *t) {
    int top = t->scroll_top;
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
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
        else if (v == 4)  t->cur_attrs |= ATTR_UNDERLINE;
        else if (v == 5)  t->cur_attrs |= ATTR_BLINK;
        else if (v == 7)  t->cur_attrs |= ATTR_REVERSE;
        else if (v == 22) t->cur_attrs &= ~ATTR_BOLD;
        else if (v == 24) t->cur_attrs &= ~ATTR_UNDERLINE;
        else if (v == 27) t->cur_attrs &= ~ATTR_REVERSE;
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
    case 'h': case 'l': break; // mode sets — ignore
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
    default: break;
    }
}

static void term_feed(Terminal *t, const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        switch (t->state) {
        case PS_NORMAL:
            if      (ch == 0x1b) { t->state = PS_ESC; }
            else if (ch == '\r') { t->cur_col = 0; }
            else if (ch == '\n') { newline(t); }
            else if (ch == '\b') { if(t->cur_col>0) t->cur_col--; }
            else if (ch == '\t') { t->cur_col=(t->cur_col+8)&~7; if(t->cur_col>=t->cols)t->cur_col=t->cols-1; }
            else if (ch == 0x07) { /* BEL */ }
            else if (ch == 0x0e || ch == 0x0f) { /* charset shifts — ignore */ }
            else if (ch >= 0x20) {
                if (t->cur_col >= t->cols) { t->cur_col=0; newline(t); }
                CELL(t,t->cur_row,t->cur_col++) = {ch, t->cur_fg, t->cur_bg, t->cur_attrs, {0,0,0}};
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
            // Consume everything until BEL (0x07) or ESC \ (ST)
            if (ch == 0x07) { t->state = PS_NORMAL; }
            else if (ch == 0x1b) { t->state = PS_ESC; } // ESC \ sequence
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

static void term_read(Terminal *t) {
    char buf[4096];
    for(;;) {
        ssize_t n = read(t->pty_fd, buf, sizeof(buf));
        if (n > 0) term_feed(t, buf, (int)n);
        else break;
    }
}

static void term_write(Terminal *t, const char *s, int n) {
    if (t->pty_fd >= 0) { ssize_t r = write(t->pty_fd, s, n); (void)r; }
}

// ============================================================================
// RENDERING
// ============================================================================

static void term_render(Terminal *t, int ox, int oy) {
    float cw = t->cell_w, ch = t->cell_h;

    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            Cell *c = &CELL(t,row,col);
            float px = ox + col*cw, py = oy + row*ch;

            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor fc = tcolor_resolve(fg), bc = tcolor_resolve(bg);
            // Bold on low palette colors -> use bright variant
            if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
                fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg)+8));

            // Background (highlight if selected)
            if (cell_in_sel(t, row, col)) {
                draw_rect(px, py, cw, ch, 0.3f, 0.5f, 1.0f, 0.5f);
            } else {
                draw_rect(px, py, cw, ch, bc.r, bc.g, bc.b, 1.f);
            }

            // Glyph
            uint32_t cp = c->cp;
            if (cp && cp != ' ') {
                char tmp[2] = { (char)(cp & 0x7f), 0 };
                float baseline = py + ch * 0.82f;
                draw_text(tmp, px, baseline, g_font_size, fc.r, fc.g, fc.b, 1.f);
            }

            // Underline
            if (c->attrs & ATTR_UNDERLINE)
                draw_rect(px, py+ch-2, cw, 2, fc.r, fc.g, fc.b, 1.f);
        }
    }

    // Cursor (blinking underline bar)
    if (t->cursor_on) {
        float cx = ox + t->cur_col * cw;
        float cy = oy + t->cur_row * ch;
        draw_rect(cx, cy+ch-3, cw, 3, 1,1,1, 0.85f);
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
    t->cursor_on = true;

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

    // Allocate initial grid
    t->cols  = TERM_COLS_DEFAULT;
    t->rows  = TERM_ROWS_DEFAULT;
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
        new_cells[i] = {' ', t->cur_fg, t->cur_bg, 0, 0};

    int copy_rows = (t->rows < new_rows) ? t->rows : new_rows;
    int copy_cols = (t->cols < new_cols) ? t->cols : new_cols;
    for (int r = 0; r < copy_rows; r++)
        for (int c = 0; c < copy_cols; c++)
            new_cells[r * new_cols + c] = CELL(t, r, c);

    free(t->cells);
    t->cells = new_cells;
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
    switch (ks.sym) {
    case SDLK_RETURN:    term_write(t, "\r",     1); break;
    case SDLK_BACKSPACE: term_write(t, "\x7f",   1); break;
    case SDLK_TAB:       term_write(t, "\t",     1); break;
    case SDLK_ESCAPE:    term_write(t, "\x1b",   1); break;
    case SDLK_UP:        term_write(t, "\x1b[A", 3); break;
    case SDLK_DOWN:      term_write(t, "\x1b[B", 3); break;
    case SDLK_RIGHT:     term_write(t, "\x1b[C", 3); break;
    case SDLK_LEFT:      term_write(t, "\x1b[D", 3); break;
    case SDLK_HOME:      term_write(t, "\x1b[H", 3); break;
    case SDLK_END:       term_write(t, "\x1b[F", 3); break;
    case SDLK_DELETE:    term_write(t, "\x1b[3~",4); break;
    case SDLK_PAGEUP:    term_write(t, "\x1b[5~",4); break;
    case SDLK_PAGEDOWN:  term_write(t, "\x1b[6~",4); break;
    case SDLK_F1:        term_write(t, "\x1bOP", 3); break;
    case SDLK_F2:        term_write(t, "\x1bOQ", 3); break;
    case SDLK_F3:        term_write(t, "\x1bOR", 3); break;
    case SDLK_F4:        term_write(t, "\x1bOS", 3); break;
    default:
        // Ctrl+key
        if ((ks.mod & KMOD_CTRL) && ks.sym >= SDLK_a && ks.sym <= SDLK_z) {
            char ctrl[1] = { (char)(ks.sym - SDLK_a + 1) };
            term_write(t, ctrl, 1);
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

    // We'll size the window to fit exactly 80x24 cells at our font size
    // We calculate after ft_init, so use a placeholder first
    int win_w = 800, win_h = 480;

    SDL_Window *window = SDL_CreateWindow(
        WIN_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, ctx);
    SDL_GL_SetSwapInterval(1);

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

    if (!term_spawn(&term, shell)) {
        SDL_Log("[Term] Failed to spawn shell. Exiting.\n");
        return 1;
    }

    // Give bash ~150ms to write its prompt before the first render
    SDL_Delay(150);
    term_read(&term);

    uint32_t last_ticks = SDL_GetTicks();
    bool running = true;

    while (running) {
        uint32_t now = SDL_GetTicks();
        double dt = (now - last_ticks) / 1000.0;
        last_ticks = now;

        // Cursor blink
        term.blink += dt;
        if (term.blink >= 0.5) { term.blink = 0; term.cursor_on = !term.cursor_on; }

        // Read PTY (clears selection if output arrives)
        {
            bool had_sel = term.sel_exists || term.sel_active;
            int old_row = term.cur_row, old_col = term.cur_col;
            term_read(&term);
            // If cursor moved, new output arrived — clear selection
            if (had_sel && (term.cur_row != old_row || term.cur_col != old_col)) {
                term.sel_exists = false;
                term.sel_active = false;
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
            switch (ev.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN: {
                SDL_Keymod mod = SDL_GetModState();
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
                if (!(mod & KMOD_CTRL))
                    term_write(&term, ev.text.text, (int)strlen(ev.text.text));
                break;
            }
            // Mouse selection
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int r, c;
                    pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2, &r, &c);
                    term.sel_start_row = term.sel_end_row = r;
                    term.sel_start_col = term.sel_end_col = c;
                    term.sel_active = true;
                    term.sel_exists = false;
                } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    // Middle-click paste
                    term_paste(&term);
                }
                break;
            case SDL_MOUSEMOTION:
                if (term.sel_active && (ev.motion.state & SDL_BUTTON_LMASK)) {
                    pixel_to_cell(&term, ev.motion.x, ev.motion.y, 2, 2,
                                  &term.sel_end_row, &term.sel_end_col);
                    term.sel_exists = true;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT && term.sel_active) {
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
            case SDL_MOUSEWHEEL: {
                SDL_Keymod mod = SDL_GetModState();
                if (mod & KMOD_CTRL) {
                    // Ctrl+scroll: zoom font size
                    int delta = (ev.wheel.y > 0) ? 1 : -1;
                    // Shift held = bigger steps
                    if (mod & KMOD_SHIFT) delta *= 4;
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    term_set_font_size(&term, g_font_size + delta, win_w, win_h);
                    // Update projection in case cell count changed
                    G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
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

        // Render
        glClearColor(0.04f, 0.04f, 0.08f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        glViewport(0, 0, win_w, win_h);
        term_render(&term, 2, 2);   // 2px margin

        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (s_ft_face)    FT_Done_Face(s_ft_face);
    if (s_ft_lib)     FT_Done_FreeType(s_ft_lib);
    if (s_font_buf)   free(s_font_buf);

    return 0;
}
