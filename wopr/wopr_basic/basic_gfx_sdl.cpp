/*
 * basic_gfx_sdl.cpp — SDL2 implementation of basic_gfx.h
 *
 * Owns:
 *   • A 32-bit ARGB pixel buffer sized to the active SCREEN mode
 *   • An SDL_Texture (streaming) uploaded each frame
 *   • The 16-colour CGA palette, overridable via gfx_palette()
 *   • Sprite store for GET / PUT
 *   • FreeType text-mode glyph rendering (DejaVu embedded font)
 *   • The SDL window, renderer, and event loop
 *
 * The display_* functions (display.h) are also implemented here,
 * replacing display_ansi.cpp entirely when USE_SDL_WINDOW is defined.
 *
 * Build:
 *   Compile this file instead of display_ansi.cpp, plus basic_gfx_sdl.cpp.
 *   Add -DUSE_SDL_WINDOW -DHAVE_SDL to the compiler flags.
 */

#ifdef USE_SDL_WINDOW

#include "basic_gfx.h"
#include "basic_gfx_sdl.h"
#include "display.h"
#include "basic.h"       // g_break, BASIC_NS
#include "DejaVuMono.h"  // DEJAVU_REGULAR_FONT_B64 / _SIZE

#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <string>

#include "basic_ns.h"

// ============================================================================
// CGA palette  (ARGB)
// ============================================================================
static Uint32 s_pal[16] = {
    0xFF000000, // 0  Black
    0xFF0000AA, // 1  Blue
    0xFF00AA00, // 2  Green
    0xFF00AAAA, // 3  Cyan
    0xFFAA0000, // 4  Red
    0xFFAA00AA, // 5  Magenta
    0xFFAA5500, // 6  Brown
    0xFFAAAAAA, // 7  Light Grey
    0xFF555555, // 8  Dark Grey
    0xFF5555FF, // 9  Light Blue
    0xFF55FF55, // 10 Light Green
    0xFF55FFFF, // 11 Light Cyan
    0xFFFF5555, // 12 Light Red
    0xFFFF55FF, // 13 Light Magenta
    0xFFFFFF55, // 14 Yellow
    0xFFFFFFFF, // 15 White
};

static inline Uint8 pal_r(int i) { i&=15; return (s_pal[i]>>16)&0xFF; }
static inline Uint8 pal_g(int i) { i&=15; return (s_pal[i]>> 8)&0xFF; }
static inline Uint8 pal_b(int i) { i&=15; return (s_pal[i]    )&0xFF; }
static int s_font_gen;

// ============================================================================
// SDL globals
// ============================================================================
static SDL_Window   *s_window   = nullptr;
static SDL_Renderer *s_renderer = nullptr;
static bool          s_needs_render = true;
static int           s_win_w = 800, s_win_h = 600;
static Uint32        s_last_dirty = 0;

// ============================================================================
// Graphics surface
// ============================================================================
static SDL_Texture         *s_gfx_tex = nullptr;
static std::vector<Uint32>  s_pixels;          // ARGB, gfx_w × gfx_h
static int                  s_gfx_w = 0, s_gfx_h = 0;
static bool                 s_gfx_active = false;

// ============================================================================
// Sprite store
// ============================================================================
struct Sprite { int w, h; std::vector<Uint32> px; };
static std::unordered_map<int, Sprite> s_sprites;

// ============================================================================
// Text grid
// ============================================================================
#define TEXT_ROWS     25
#define TEXT_COLS_80  80
#define TEXT_COLS_40  40

struct Cell { char ch; Uint8 fg, bg; };
static Cell s_grid[TEXT_ROWS][TEXT_COLS_80];
static int  s_text_cols  = TEXT_COLS_80;
static int  s_cur_row    = 0, s_cur_col = 0;
static int  s_cur_fg     = 7, s_cur_bg  = 0;
static bool s_cursor_vis = true;

// ============================================================================
// FreeType / glyph cache
// ============================================================================
static FT_Library s_ft_lib  = nullptr;
static FT_Face    s_ft_face = nullptr;
static int        s_cell_w  = 8;
static int        s_cell_h  = 16;

#define TEXT_PAD  8   // px padding inside window on each side

struct Glyph {
    std::vector<Uint8> bm;  // grayscale w×h
    int bm_w, bm_h;
    int bearing_x, bearing_y;
};
static std::unordered_map<int, Glyph> s_glyph_cache;

// base64 decode (for embedded font)
static std::vector<Uint8> b64_decode(const char *src, size_t len) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<Uint8> out;
    out.reserve(len * 3 / 4);
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int8_t v = T[(uint8_t)src[i]];
        if (v < 0) continue;
        acc = (acc << 6) | (uint8_t)v;
        if ((bits += 6) >= 8) { bits -= 8; out.push_back((acc >> bits) & 0xFF); }
    }
    return out;
}

// Kept alive for FT_New_Memory_Face
static std::vector<Uint8> s_font_data;

void gfx_maybe_mark_dirty() {
    Uint32 now = SDL_GetTicks();
    if (now - s_last_dirty >= 16) {   // ~60 FPS
        s_needs_render = true;
        s_last_dirty = now;
    }
}

static void ft_set_size_for_window() {
    if (!s_ft_face) return;
    // Pick the largest font size where 80 cols × 25 rows fits with padding
    int avail_w = s_win_w - 2 * TEXT_PAD;
    int avail_h = s_win_h - 2 * TEXT_PAD;
    // Binary search for best pixel height
    int lo = 8, hi = avail_h / TEXT_ROWS;
    if (hi < lo) hi = lo;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)mid);
        int adv = (int)(s_ft_face->size->metrics.max_advance >> 6);
        int ht  = (int)(s_ft_face->size->metrics.height >> 6);
        if (adv > 0 && ht > 0 && adv * TEXT_COLS_80 <= avail_w && ht * TEXT_ROWS <= avail_h)
            lo = mid;
        else
            hi = mid - 1;
    }
    FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)lo);
    int adv = (int)(s_ft_face->size->metrics.max_advance >> 6);
    int ht  = (int)(s_ft_face->size->metrics.height >> 6);
    if (adv > 0) s_cell_w = adv;
    if (ht  > 0) s_cell_h = ht;
    s_glyph_cache.clear();
    s_font_gen++;
}

static void ft_init() {
    if (!s_ft_lib) FT_Init_FreeType(&s_ft_lib);
    s_font_data = b64_decode(DEJAVU_REGULAR_FONT_B64, DEJAVU_REGULAR_FONT_B64_SIZE);
    FT_New_Memory_Face(s_ft_lib, s_font_data.data(), (FT_Long)s_font_data.size(), 0, &s_ft_face);
    ft_set_size_for_window();
    s_glyph_cache.clear();
}

bool gfx_sdl_load_font(const char *path) {
    if (!s_ft_lib) FT_Init_FreeType(&s_ft_lib);
    if (s_ft_face) { FT_Done_Face(s_ft_face); s_ft_face = nullptr; }
    if (FT_New_Face(s_ft_lib, path, 0, &s_ft_face) != 0) {
        ft_init();  // fallback to embedded
        return false;
    }
    ft_set_size_for_window();
    return true;
}

static const Glyph &glyph_get(int cp) {
    auto it = s_glyph_cache.find(cp);
    if (it != s_glyph_cache.end()) return it->second;

    Glyph g{}; g.bm_w = s_cell_w; g.bm_h = s_cell_h;
    g.bearing_x = 0; g.bearing_y = s_cell_h - 2;
    g.bm.assign((size_t)(s_cell_w * s_cell_h), 0);

    if (s_ft_face) {
        FT_UInt idx = FT_Get_Char_Index(s_ft_face, (FT_ULong)cp);
        if (idx && FT_Load_Glyph(s_ft_face, idx, FT_LOAD_RENDER) == 0) {
            FT_Bitmap &b = s_ft_face->glyph->bitmap;
            g.bm_w = (int)b.width; g.bm_h = (int)b.rows;
            g.bearing_x = s_ft_face->glyph->bitmap_left;
            g.bearing_y = s_ft_face->glyph->bitmap_top;
            g.bm.assign(b.buffer, b.buffer + b.width * b.rows);
        }
    }
    s_glyph_cache[cp] = std::move(g);
    return s_glyph_cache[cp];
}

// ============================================================================
// Keyboard queue
// ============================================================================
static std::queue<int> s_keys;
static std::mutex      s_keys_mx;

static void key_push(int c) { std::lock_guard<std::mutex> lk(s_keys_mx); s_keys.push(c); }
static int  key_pop()  {
    std::lock_guard<std::mutex> lk(s_keys_mx);
    if (s_keys.empty()) return -1;
    int c = s_keys.front(); s_keys.pop(); return c;
}

// ============================================================================
// SDL init / shutdown
// ============================================================================
bool gfx_sdl_init(const char *title, int w, int h) {
    s_win_w = w; s_win_h = h;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return false;
    }
    s_window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!s_window) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return false; }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED);
    if (!s_renderer)
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);

    ft_init();

    for (auto &row : s_grid)
        for (auto &c : row) c = {' ', 7, 0};

    s_needs_render = true;
    return true;
}

void gfx_sdl_shutdown() {
    if (s_gfx_tex)  { SDL_DestroyTexture(s_gfx_tex); s_gfx_tex = nullptr; }
    if (s_ft_face)  { FT_Done_Face(s_ft_face);       s_ft_face = nullptr; }
    if (s_ft_lib)   { FT_Done_FreeType(s_ft_lib);    s_ft_lib  = nullptr; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = nullptr; }
    if (s_window)   { SDL_DestroyWindow(s_window);     s_window   = nullptr; }
    SDL_Quit();
}

// ============================================================================
// Event pump  (must run on main thread)
// ============================================================================
static bool s_quit = false;   // set on SDL_QUIT, checked everywhere

bool gfx_sdl_pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            s_quit = true;
            // Signal the interpreter to stop cleanly
            BASIC_NS::g_break = 1;
            return false;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                s_win_w = e.window.data1;
                s_win_h = e.window.data2;
                ft_set_size_for_window();   // re-fit font to new window size
                s_needs_render = true;
            }
            break;
        case SDL_KEYDOWN: {
            SDL_Keycode sym = e.key.keysym.sym;
            if (sym == SDLK_c && (e.key.keysym.mod & KMOD_CTRL)) {
                // Ctrl+C — break like SIGINT
                BASIC_NS::g_break = 1;
                key_push(3);
            } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                key_push('\n');
            } else if (sym == SDLK_BACKSPACE) {
                key_push(8);
            } else if (sym == SDLK_TAB) {
                key_push(9);
            } else if (sym == SDLK_ESCAPE) {
                key_push(27);
            } else if (sym == SDLK_UP)   { key_push(0x48); }
            else if (sym == SDLK_DOWN)   { key_push(0x50); }
            else if (sym == SDLK_LEFT)   { key_push(0x4B); }
            else if (sym == SDLK_RIGHT)  { key_push(0x4D); }
            break;
        }
        case SDL_TEXTINPUT:
            for (const char *p = e.text.text; *p; p++)
                key_push((unsigned char)*p);
            break;
        default: break;
        }
    }
    return !s_quit;
}

// ============================================================================
// Rendering
// ============================================================================
static void render_text_cell(int row, int col) {
    const Cell &cell = s_grid[row][col];
    unsigned char ch = (unsigned char)cell.ch;

    int cx = col * s_cell_w;
    int cy = row * s_cell_h;

    // Draw cell background only for non-space cells so graphics show through.
    // In graphics mode, text background is transparent (like real QBasic SCREEN 9).
    if (ch >= 0x20 && ch != ' ' && !s_gfx_active) {
        Uint32 bg = s_pal[cell.bg & 15];
        SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(s_renderer,
            (bg>>16)&0xFF, (bg>>8)&0xFF, bg&0xFF, 255);
        SDL_Rect bgr = { cx, cy, s_cell_w, s_cell_h };
        SDL_RenderFillRect(s_renderer, &bgr);
    }

    if (ch < 0x20)
        return;

    const Glyph &g = glyph_get(ch);
    if (g.bm.empty())
        return;

    // Cache keyed by (codepoint, font generation)
    struct Key {
        int cp;
        int gen;
        bool operator==(const Key &o) const {
            return cp == o.cp && gen == o.gen;
        }
    };

    struct KeyHash {
        size_t operator()(const Key &k) const {
            return (size_t)k.cp ^ ((size_t)k.gen << 16);
        }
    };

    static std::unordered_map<Key, SDL_Texture*, KeyHash> tex_cache;

    Key key{ ch, s_font_gen };
    SDL_Texture *tex = nullptr;

    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) {
        tex = it->second;
    } else {
        // Build ARGB texture exactly like pixel renderer
        tex = SDL_CreateTexture(
            s_renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STATIC,
            g.bm_w,
            g.bm_h
        );
        if (!tex) return;

        std::vector<Uint32> argb((size_t)(g.bm_w * g.bm_h));

        Uint32 fgcol = s_pal[cell.fg & 15]; // ARGB from palette

        for (int i = 0; i < g.bm_w * g.bm_h; i++) {
            Uint8 a = g.bm[i];
            // ARGB: palette RGB, glyph alpha
            argb[i] = (fgcol & 0x00FFFFFFu) | ((Uint32)a << 24);
        }

        SDL_UpdateTexture(tex, nullptr, argb.data(), g.bm_w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

        tex_cache[key] = tex;
    }

    // Baseline alignment
    int ascender = s_ft_face
        ? (int)(s_ft_face->size->metrics.ascender >> 6)
        : s_cell_h - 2;

    int gx = cx + g.bearing_x;
    int gy = cy + ascender - g.bearing_y;

    SDL_Rect dst = { gx, gy, g.bm_w, g.bm_h };
    SDL_RenderCopy(s_renderer, tex, nullptr, &dst);
}



void gfx_sdl_render() {
    if (!s_needs_render) return;
    s_needs_render = false;

    // Clear full window
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);

    //
    // 1. Draw graphics framebuffer (if active)
    //
    if (s_gfx_active && s_gfx_tex) {
        // Always reset viewport before drawing graphics
        SDL_RenderSetViewport(s_renderer, nullptr);

        SDL_UpdateTexture(s_gfx_tex, nullptr, s_pixels.data(), s_gfx_w * 4);

        // Scale graphics to fit window (letterbox only graphics)
        float sx = (float)s_win_w / s_gfx_w;
        float sy = (float)s_win_h / s_gfx_h;
        float scale = std::min(sx, sy);
        int dw = (int)(s_gfx_w * scale);
        int dh = (int)(s_gfx_h * scale);

        SDL_Rect dst = {
            (s_win_w - dw) / 2,
            (s_win_h - dh) / 2,
            dw,
            dh
        };

        SDL_RenderCopy(s_renderer, s_gfx_tex, nullptr, &dst);
    }

    //
    // 2. Draw text grid overlay (ALWAYS, even in graphics modes)
    //
    // QBASIC draws text at the top-left, never centered.
    //
    SDL_Rect vp = { 0, 0, s_text_cols * s_cell_w, TEXT_ROWS * s_cell_h };
    SDL_RenderSetViewport(s_renderer, &vp);
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);

    for (int r = 0; r < TEXT_ROWS; r++)
        for (int c = 0; c < s_text_cols; c++)
            render_text_cell(r, c);

    //
    // 3. Draw cursor (in ALL modes)
    //
    if (s_cursor_vis &&
        s_cur_row < TEXT_ROWS &&
        s_cur_col < s_text_cols)
    {
        int cx = s_cur_col * s_cell_w;
        int cy = s_cur_row * s_cell_h + s_cell_h - 3;

        SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(s_renderer, 200, 200, 200, 255);

        SDL_RenderDrawLine(s_renderer, cx, cy, cx + s_cell_w - 1, cy);
        SDL_RenderDrawLine(s_renderer, cx, cy + 1, cx + s_cell_w - 1, cy + 1);
    }

    // Reset viewport back to full window
    SDL_RenderSetViewport(s_renderer, nullptr);

    //
    // 4. Present final composed frame
    //
    SDL_RenderPresent(s_renderer);
}


void gfx_sdl_mark_dirty() { s_needs_render = true; }

// ============================================================================
// Pixel helpers
// ============================================================================
static inline void px_set(int x, int y, int color) {
    if (x < 0 || y < 0 || x >= s_gfx_w || y >= s_gfx_h) return;
    s_pixels[(size_t)(y * s_gfx_w + x)] = s_pal[color & 15] | 0xFF000000u;
}

static inline Uint32 px_get(int x, int y) {
    if (x < 0 || y < 0 || x >= s_gfx_w || y >= s_gfx_h) return 0;
    return s_pixels[(size_t)(y * s_gfx_w + x)];
}

// ============================================================================
// Text grid helpers
// ============================================================================
static void text_scroll_up() {
    for (int r = 0; r < TEXT_ROWS - 1; r++)
        memcpy(s_grid[r], s_grid[r + 1], sizeof(s_grid[0]));
    for (auto &c : s_grid[TEXT_ROWS - 1]) c = {' ', (Uint8)s_cur_fg, (Uint8)s_cur_bg};
    s_needs_render = true;
}

static void text_newline() {
    s_cur_col = 0;
    if (++s_cur_row >= TEXT_ROWS) { text_scroll_up(); s_cur_row = TEXT_ROWS - 1; }
}

// ESC-sequence filter state — swallows ESC + any following non-alpha bytes
// then one alpha terminator.  Handles ESC E (CP/M clear), ESC [ ... (ANSI).
static int s_esc_state = 0;  // 0=normal, 1=saw ESC, 2=inside CSI

static void text_putchar(char c) {
    unsigned char uc = (unsigned char)c;

    if (s_esc_state == 1) {
        if (uc == '[') { s_esc_state = 2; return; }  // CSI sequence
        s_esc_state = 0; return;                      // single-char ESC (ESC E etc.) — discard
    }
    if (s_esc_state == 2) {
        // Inside CSI: consume until alpha terminator
        if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z')) s_esc_state = 0;
        return;
    }
    if (uc == 0x1B) { s_esc_state = 1; return; }

    if (c == '\n') { text_newline(); return; }
    if (c == '\r') { s_cur_col = 0;  return; }
    if (uc < 0x20) return;
    if (s_cur_col < s_text_cols && s_cur_row < TEXT_ROWS)
        s_grid[s_cur_row][s_cur_col] = {c, (Uint8)s_cur_fg, (Uint8)s_cur_bg};
    if (++s_cur_col >= s_text_cols) text_newline();
    s_needs_render = true;
}

// ============================================================================
// basic_gfx.h implementation
// ============================================================================

// Screen mode dims table
static void screen_mode_dims(int mode, int *w, int *h) {
    static const struct { int m, w, h; } MODES[] = {
        {1,320,200},{2,640,200},{3,720,348},{4,640,400},{5,160,100},
        {6,160,200},{7,320,200},{8,640,200},{9,640,350},{10,640,350},
        {11,640,480},{12,640,480},{13,320,200},{14,320,200},{15,640,200},
        {16,640,480},{17,640,480},{18,640,480},{19,640,480},{20,512,480},
        {21,640,400},{22,640,480},{23,800,600},{24,160,200},{25,320,200},
        {26,640,200},{27,640,200},{28,720,350},{0,0,0}
    };
    *w = 640; *h = 350;
    for (int i = 0; MODES[i].m || MODES[i].w; i++)
        if (MODES[i].m == mode) { *w = MODES[i].w; *h = MODES[i].h; return; }
}

void gfx_screen(int mode) {
    if (mode == 0) {
        s_gfx_active = false;
        if (s_gfx_tex) { SDL_DestroyTexture(s_gfx_tex); s_gfx_tex = nullptr; }
        s_pixels.clear(); s_gfx_w = s_gfx_h = 0;
        s_needs_render = true;
        return;
    }
    int gw, gh;
    screen_mode_dims(mode, &gw, &gh);
    s_gfx_w = gw; s_gfx_h = gh;
    s_pixels.assign((size_t)(gw * gh), s_pal[0] | 0xFF000000u);
    if (s_gfx_tex) SDL_DestroyTexture(s_gfx_tex);
    s_gfx_tex = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, gw, gh);
    SDL_SetTextureBlendMode(s_gfx_tex, SDL_BLENDMODE_BLEND);
    s_gfx_active = true;
    s_needs_render = true;
}

void gfx_palette(int idx, int r, int g, int b) {
    idx &= 15;
    s_pal[idx] = 0xFF000000u
               | ((Uint32)(r & 0xFF) << 16)
               | ((Uint32)(g & 0xFF) <<  8)
               | ((Uint32)(b & 0xFF)      );
}

void gfx_cls(int color) {
    // 1. Clear graphics buffer if active
    if (s_gfx_active) {
        std::fill(s_pixels.begin(), s_pixels.end(),
                  s_pal[color & 15] | 0xFF000000u);
    }

    // 2. Clear text grid to the same background colour
    for (int r = 0; r < TEXT_ROWS; r++) {
        for (int c = 0; c < s_text_cols; c++) {
            s_grid[r][c] = { ' ', (Uint8)s_cur_fg, (Uint8)(color & 15) };
        }
    }

    // 3. Reset cursor and visibility
    s_cur_row    = 0;
    s_cur_col    = 0;
    s_cursor_vis = true;

    // 4. Mark dirty so it actually redraws
    s_needs_render = true;
}


void gfx_pset(int x, int y, int color) {
    px_set(x, y, color);
}

void gfx_line(int x0, int y0, int x1, int y1, int color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px_set(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_box(int x1, int y1, int x2, int y2, int color) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    gfx_line(x1, y1, x2, y1, color);
    gfx_line(x2, y1, x2, y2, color);
    gfx_line(x2, y2, x1, y2, color);
    gfx_line(x1, y2, x1, y1, color);
}

void gfx_boxfill(int x1, int y1, int x2, int y2, int color) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(x1, 0); y1 = std::max(y1, 0);
    x2 = std::min(x2, s_gfx_w - 1); y2 = std::min(y2, s_gfx_h - 1);
    Uint32 col = s_pal[color & 15] | 0xFF000000u;
    for (int y = y1; y <= y2; y++) {
        Uint32 *row = s_pixels.data() + (size_t)(y * s_gfx_w);
        std::fill(row + x1, row + x2 + 1, col);
    }
    s_needs_render = true;
}

void gfx_circle(int cx, int cy, int radius, int color) {
    int x = radius, y = 0, err = 0;
    while (x >= y) {
        px_set(cx+x, cy+y, color); px_set(cx+y, cy+x, color);
        px_set(cx-y, cy+x, color); px_set(cx-x, cy+y, color);
        px_set(cx-x, cy-y, color); px_set(cx-y, cy-x, color);
        px_set(cx+y, cy-x, color); px_set(cx+x, cy-y, color);
        y++;
        if (err <= 0) err += 2*y + 1;
        else          { x--; err += 2*(y - x) + 1; }
    }
}

void gfx_paint(int x, int y, int fill_color, int border_color) {
    if (x < 0 || y < 0 || x >= s_gfx_w || y >= s_gfx_h) return;
    Uint32 target = px_get(x, y);
    Uint32 fill   = s_pal[fill_color   & 15] | 0xFF000000u;
    Uint32 border = s_pal[border_color & 15] | 0xFF000000u;
    if (target == fill || target == border) return;

    // Scanline flood fill — avoids stack overflow for large areas
    struct Span { int x0, x1, y, dy; };
    std::vector<Span> stack;
    stack.push_back({x, x, y, 1});
    stack.push_back({x, x, y - 1, -1});

    while (!stack.empty()) {
        auto [x0, x1, sy, dy] = stack.back(); stack.pop_back();
        int nx0 = x0;
        // extend left
        while (nx0 > 0 && px_get(nx0 - 1, sy) == target) nx0--;
        // extend right
        int nx1 = x1;
        while (nx1 < s_gfx_w - 1 && px_get(nx1 + 1, sy) == target) nx1++;
        // fill span
        for (int i = nx0; i <= nx1; i++) {
            s_pixels[(size_t)(sy * s_gfx_w + i)] = fill;
        }
        // push child spans
        int ny = sy + dy;
        if (ny >= 0 && ny < s_gfx_h) {
            int i = nx0;
            while (i <= nx1) {
                while (i <= nx1 && px_get(i, ny) != target) i++;
                if (i > nx1) break;
                int js = i;
                while (i <= nx1 && px_get(i, ny) == target) i++;
                stack.push_back({js, i - 1, ny, dy});
            }
        }
        // also push opposite spans for leaky areas
        ny = sy - dy;
        if (ny >= 0 && ny < s_gfx_h) {
            int i = nx0;
            while (i <= nx1) {
                while (i <= nx1 && px_get(i, ny) != target) i++;
                if (i > nx1) break;
                int js = i;
                while (i <= nx1 && px_get(i, ny) == target) i++;
                if (js < x0 || i - 1 > x1)
                    stack.push_back({js, i - 1, ny, -dy});
            }
        }
    }
    s_needs_render = true;
}

void gfx_get(int id, int x1, int y1, int x2, int y2) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    Sprite &sp = s_sprites[id];
    sp.w = w; sp.h = h;
    sp.px.resize((size_t)(w * h));
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            sp.px[(size_t)(row * w + col)] = px_get(x1 + col, y1 + row);
}

void gfx_put(int id, int x, int y, int xor_mode) {
    auto it = s_sprites.find(id);
    if (it == s_sprites.end()) return;
    const Sprite &sp = it->second;
    for (int row = 0; row < sp.h; row++) {
        for (int col = 0; col < sp.w; col++) {
            int dx = x + col, dy = y + row;
            if (dx < 0 || dy < 0 || dx >= s_gfx_w || dy >= s_gfx_h) continue;
            Uint32 src = sp.px[(size_t)(row * sp.w + col)];
            size_t idx = (size_t)(dy * s_gfx_w + dx);
            if (xor_mode) s_pixels[idx] ^= (src & 0x00FFFFFFu);
            else          s_pixels[idx]  =  src;
        }
    }
    s_needs_render = true;
}

int gfx_active(void) { return s_gfx_active ? 1 : 0; }
int gfx_width(void)  { return s_gfx_w; }
int gfx_height(void) { return s_gfx_h; }

// ============================================================================
// display.h implementation  (replaces display_ansi.cpp)
// ============================================================================
BASIC_NS_BEGIN

void display_init(void) {
    gfx_sdl_init("WOPR BASIC", 800, 600);
}

void display_shutdown(void) {
    gfx_sdl_shutdown();
}

void display_cls(void) {
    gfx_cls(s_cur_bg);
}

void display_locate(int row, int col) {
    if (row > 0) s_cur_row = std::min(row - 1, TEXT_ROWS - 1);
    if (col > 0) s_cur_col = std::min(col - 1, s_text_cols - 1);
}

void display_color(int fg, int bg) {
    s_cur_fg = fg & 15;
    s_cur_bg = bg & 15;
}

void display_width(int cols) {
    s_text_cols = (cols <= 40) ? TEXT_COLS_40 : TEXT_COLS_80;
    s_needs_render = true;
}

void display_print(char *s) {
    for (; *s; s++) text_putchar(*s);
    // Pump events periodically so SDL_QUIT is handled even during long output
    // (render only when dirty to avoid wasted frames)
    gfx_sdl_pump();
    gfx_sdl_render();
}

void display_putchar(int c) {
    text_putchar((char)c);
    gfx_sdl_pump();
    gfx_sdl_render();
}

void display_newline(void) {
    text_newline();
    gfx_sdl_pump();
    gfx_sdl_render();
}

void display_spc(int n) {
    for (int i = 0; i < n; i++) text_putchar(' ');
}

void display_cursor(int visible) {
    s_cursor_vis = (visible != 0);
    s_needs_render = true;
}

int display_get_width(void) {
    return s_text_cols;
}

// Non-blocking key poll (INKEY$)
int display_inkey(void) {
    gfx_sdl_pump();
    gfx_sdl_render();
    int c = key_pop();
    if (c < 0) {
        SDL_Delay(5);   // yield to OS — prevents 100% CPU in polling loops
        return 0;
    }
    return c;
}

// Blocking single char
int display_getchar(void) {
    int c = -1;
    while (c < 0) {
        if (!gfx_sdl_pump()) return 0;
        gfx_sdl_render();
        c = key_pop();
        if (c < 0) SDL_Delay(10);
    }
    return c;
}

// Blocking line input with echo
int display_getline(char *buf, int bufsz) {
    int len = 0;
    s_cursor_vis = true;
    s_needs_render = true;
    for (;;) {
        // Blink cursor: render every ~500ms toggle
        static Uint32 blink_t = 0;
        Uint32 now = SDL_GetTicks();
        if (now - blink_t > 500) {
            s_cursor_vis = !s_cursor_vis;
            blink_t = now;
            s_needs_render = true;
        }
        gfx_sdl_render();

        if (!gfx_sdl_pump()) { buf[len] = '\0'; return len; }

        int c = key_pop();
        if (c < 0) { SDL_Delay(10); continue; }

        if (c == '\n' || c == '\r') break;
        if (c == 8 || c == 127) {  // backspace
            if (len > 0) {
                len--;
                s_cur_col = std::max(0, s_cur_col - 1);
                s_grid[s_cur_row][s_cur_col] = {' ', (Uint8)s_cur_fg, (Uint8)s_cur_bg};
                s_needs_render = true;
            }
            continue;
        }
        if (len < bufsz - 1) {
            buf[len++] = (char)c;
            text_putchar((char)c);
        }
    }
    buf[len] = '\0';
    s_cursor_vis = false;
    text_newline();
    gfx_sdl_render();
    return len;
}

BASIC_NS_END

#endif /* USE_SDL_WINDOW */
