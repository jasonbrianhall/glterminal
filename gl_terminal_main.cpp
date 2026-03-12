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
#include <errno.h>
#include <vector>
#include <unordered_map>

// Pull in the embedded font
#include "Monospace.h"

// ============================================================================
// CONFIG
// ============================================================================

#define TERM_COLS       80
#define TERM_ROWS       24
#define FONT_SIZE       16
#define WIN_TITLE       "GL Terminal"

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

struct TermColor { float r,g,b; };
static const TermColor PALETTE[16] = {
    {0.f,    0.f,    0.f},    // 0 black
    {0.8f,   0.1f,   0.1f},   // 1 red
    {0.1f,   0.8f,   0.1f},   // 2 green
    {0.8f,   0.8f,   0.1f},   // 3 yellow
    {0.2f,   0.2f,   0.9f},   // 4 blue
    {0.8f,   0.1f,   0.8f},   // 5 magenta
    {0.1f,   0.8f,   0.8f},   // 6 cyan
    {0.75f,  0.75f,  0.75f},  // 7 white
    {0.4f,   0.4f,   0.4f},   // 8 bright black
    {1.f,    0.3f,   0.3f},   // 9 bright red
    {0.3f,   1.f,    0.3f},   // 10 bright green
    {1.f,    1.f,    0.3f},   // 11 bright yellow
    {0.3f,   0.4f,   1.f},    // 12 bright blue
    {1.f,    0.3f,   1.f},    // 13 bright magenta
    {0.3f,   1.f,    1.f},    // 14 bright cyan
    {1.f,    1.f,    1.f},    // 15 bright white
};

struct Cell {
    uint32_t cp;
    uint8_t  fg, bg, attrs, _pad;
};

typedef enum { PS_NORMAL, PS_ESC, PS_CSI, PS_OSC } ParseState;

struct Terminal {
    Cell        cells[TERM_ROWS][TERM_COLS];
    int         cur_row, cur_col;
    uint8_t     cur_fg, cur_bg, cur_attrs;
    ParseState  state;
    char        csi[64];
    int         csi_len;
    int         pty_fd;
    pid_t       child;
    float       cell_w, cell_h;
    double      blink;
    bool        cursor_on;
};

// ============================================================================
// PARSER
// ============================================================================

static void scroll_up(Terminal *t) {
    memmove(t->cells[0], t->cells[1], sizeof(Cell)*TERM_COLS*(TERM_ROWS-1));
    for (int c = 0; c < TERM_COLS; c++) {
        t->cells[TERM_ROWS-1][c] = {' ', t->cur_fg, t->cur_bg, 0, 0};
    }
}

static void newline(Terminal *t) {
    if (++t->cur_row >= TERM_ROWS) { t->cur_row = TERM_ROWS-1; scroll_up(t); }
}

static void sgr(Terminal *t, const char *p) {
    char buf[64]; strncpy(buf, p, 63); buf[63]='\0';
    int  params[16]; int pc = 0;
    char *tok = strtok(buf, ";");
    while (tok && pc < 16) { params[pc++] = atoi(tok); tok = strtok(NULL, ";"); }
    if (!pc) { params[0]=0; pc=1; }
    for (int i = 0; i < pc; i++) {
        int v = params[i];
        if      (v == 0)              { t->cur_fg=7; t->cur_bg=0; t->cur_attrs=0; }
        else if (v == 1)              t->cur_attrs |= ATTR_BOLD;
        else if (v == 4)              t->cur_attrs |= ATTR_UNDERLINE;
        else if (v == 7)              t->cur_attrs |= ATTR_REVERSE;
        else if (v == 22)             t->cur_attrs &= ~ATTR_BOLD;
        else if (v == 24)             t->cur_attrs &= ~ATTR_UNDERLINE;
        else if (v == 27)             t->cur_attrs &= ~ATTR_REVERSE;
        else if (v>=30 && v<=37)      t->cur_fg = (uint8_t)(v-30);
        else if (v == 39)             t->cur_fg = 7;
        else if (v>=40 && v<=47)      t->cur_bg = (uint8_t)(v-40);
        else if (v == 49)             t->cur_bg = 0;
        else if (v>=90 && v<=97)      t->cur_fg = (uint8_t)(v-90+8);
        else if (v>=100 && v<=107)    t->cur_bg = (uint8_t)(v-100+8);
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
        t->cur_row = SDL_clamp(row-1, 0, TERM_ROWS-1);
        t->cur_col = SDL_clamp(col-1, 0, TERM_COLS-1);
        break;
    }
    case 'A': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_max(0,t->cur_row-n); break; }
    case 'B': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_min(TERM_ROWS-1,t->cur_row+n); break; }
    case 'C': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_min(TERM_COLS-1,t->cur_col+n); break; }
    case 'D': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_max(0,t->cur_col-n); break; }
    case 'G': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_clamp(n-1,0,TERM_COLS-1); break; }
    case 'J': {
        int n=atoi(p);
        if (n==2||n==3) {
            for(int r=0;r<TERM_ROWS;r++) for(int c=0;c<TERM_COLS;c++) t->cells[r][c]={' ',t->cur_fg,t->cur_bg,0,0};
            t->cur_row=t->cur_col=0;
        } else if(n==1) {
            for(int r=0;r<t->cur_row;r++) for(int c=0;c<TERM_COLS;c++) t->cells[r][c]={' ',t->cur_fg,t->cur_bg,0,0};
        } else {
            for(int r=t->cur_row;r<TERM_ROWS;r++)
                for(int c=(r==t->cur_row?t->cur_col:0);c<TERM_COLS;c++) t->cells[r][c]={' ',t->cur_fg,t->cur_bg,0,0};
        }
        break;
    }
    case 'K': {
        int n=atoi(p);
        int s=(n==1)?0:t->cur_col, e=(n==0)?TERM_COLS:t->cur_col+1;
        for(int c=s;c<e&&c<TERM_COLS;c++) t->cells[t->cur_row][c]={' ',t->cur_fg,t->cur_bg,0,0};
        break;
    }
    case 'P': { // DCH delete chars
        int n=atoi(p); if(n<1)n=1;
        int r=t->cur_row, c=t->cur_col;
        memmove(&t->cells[r][c], &t->cells[r][c+n], sizeof(Cell)*(TERM_COLS-c-n));
        for(int i=TERM_COLS-n;i<TERM_COLS;i++) t->cells[r][i]={' ',t->cur_fg,t->cur_bg,0,0};
        break;
    }
    case 'h': case 'l': break; // mode sets — ignore
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
            else if (ch == '\t') { t->cur_col=(t->cur_col+8)&~7; if(t->cur_col>=TERM_COLS)t->cur_col=TERM_COLS-1; }
            else if (ch == 0x07) { /* BEL */ }
            else if (ch == 0x0e || ch == 0x0f) { /* charset shifts — ignore */ }
            else if (ch >= 0x20) {
                if (t->cur_col >= TERM_COLS) { t->cur_col=0; newline(t); }
                t->cells[t->cur_row][t->cur_col++] = {ch, t->cur_fg, t->cur_bg, t->cur_attrs, 0};
            }
            break;
        case PS_ESC:
            if (ch == '[') {
                t->state=PS_CSI; t->csi_len=0; memset(t->csi,0,sizeof(t->csi));
            } else if (ch == ']') {
                t->state=PS_OSC;
            } else if (ch == '(' || ch == ')') {
                // charset designator - consume one more byte then back to normal
                // simplest: just go to a skip-one state; we reuse OSC for now
                // Actually just skip: next byte is charset code, eat it via a flag
                t->state=PS_NORMAL; // will eat next char as part of normal (harmless)
            } else {
                if (ch == 'c') {
                    for(int r=0;r<TERM_ROWS;r++) for(int c=0;c<TERM_COLS;c++) t->cells[r][c]={' ',7,0,0,0};
                    t->cur_row=t->cur_col=0; t->cur_fg=7; t->cur_bg=0; t->cur_attrs=0;
                } else if (ch == 'M') {
                    if (t->cur_row > 0) { t->cur_row--; }
                    else {
                        memmove(&t->cells[1], &t->cells[0], sizeof(Cell)*TERM_COLS*(TERM_ROWS-1));
                        for(int c=0;c<TERM_COLS;c++) t->cells[0][c]={' ',t->cur_fg,t->cur_bg,0,0};
                    }
                }
                t->state = PS_NORMAL;
            }
            break;
        case PS_CSI:
            if (ch >= 0x40 && ch <= 0x7e) {
                if (t->csi_len < 62) t->csi[t->csi_len++] = (char)ch;
                t->csi[t->csi_len] = '\0';
                dispatch_csi(t);
                t->state = PS_NORMAL; t->csi_len = 0;
            } else {
                if (t->csi_len < 62) t->csi[t->csi_len++] = (char)ch;
            }
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
        .ws_row=(unsigned short)TERM_ROWS, .ws_col=(unsigned short)TERM_COLS,
        .ws_xpixel=(unsigned short)(TERM_COLS*(int)t->cell_w),
        .ws_ypixel=(unsigned short)(TERM_ROWS*(int)t->cell_h)
    };
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return false; }
    if (pid == 0) {
        setenv("TERM","xterm-color",1);
        setenv("COLUMNS","80",1);
        setenv("LINES","24",1);
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

    for (int row = 0; row < TERM_ROWS; row++) {
        for (int col = 0; col < TERM_COLS; col++) {
            Cell *c = &t->cells[row][col];
            float px = ox + col*cw, py = oy + row*ch;

            uint8_t fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { uint8_t tmp=fg; fg=bg; bg=tmp; }
            TermColor fc = PALETTE[fg&15], bc = PALETTE[bg&15];
            if ((c->attrs & ATTR_BOLD) && fg < 8) fc = PALETTE[fg+8];

            // Background
            draw_rect(px, py, cw, ch, bc.r, bc.g, bc.b, 1.f);

            // Glyph
            uint32_t cp = c->cp;
            if (cp && cp != ' ') {
                char tmp[2] = { (char)(cp & 0x7f), 0 };
                float baseline = py + ch * 0.82f;
                draw_text(tmp, px, baseline, FONT_SIZE, fc.r, fc.g, fc.b, 1.f);
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
    t->cur_fg   = 7;
    t->cur_bg   = 0;
    t->pty_fd   = -1;
    t->child    = -1;
    t->state    = PS_NORMAL;
    t->cursor_on = true;

    // Measure monospace cell from '0'
    if (s_ft_face) {
        FT_Set_Pixel_Sizes(s_ft_face, 0, FONT_SIZE);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, '0');
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT)) {
            t->cell_w = (float)(s_ft_face->glyph->advance.x >> 6);
            t->cell_h = (float)(int)(FONT_SIZE * 1.4f);
        }
    }
    if (t->cell_w < 1) t->cell_w = 10;
    if (t->cell_h < 1) t->cell_h = 20;

    SDL_Log("[Term] cell size: %.0fx%.0f\n", t->cell_w, t->cell_h);

    for (int r=0;r<TERM_ROWS;r++)
        for (int c=0;c<TERM_COLS;c++)
            t->cells[r][c] = {' ', 7, 0, 0, 0};
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

    // Resize window to exact terminal size BEFORE init_renderer so projection is correct
    win_w = (int)(term.cell_w * TERM_COLS) + 4;
    win_h = (int)(term.cell_h * TERM_ROWS) + 4;
    SDL_SetWindowSize(window, win_w, win_h);
    SDL_GetWindowSize(window, &win_w, &win_h);

    gl_init_renderer(win_w, win_h);
    glViewport(0, 0, win_w, win_h);

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

        // Read PTY
        term_read(&term);

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
            case SDL_KEYDOWN:
                handle_key(&term, ev.key.keysym, NULL);
                break;
            case SDL_TEXTINPUT: {
                SDL_Keymod mod = SDL_GetModState();
                if (!(mod & KMOD_CTRL))
                    term_write(&term, ev.text.text, (int)strlen(ev.text.text));
                break;
            }
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    win_w = ev.window.data1;
                    win_h = ev.window.data2;
                    glViewport(0, 0, win_w, win_h);
                    G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
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
