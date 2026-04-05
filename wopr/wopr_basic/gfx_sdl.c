/*
 * gfx_sdl.c — SDL2 pixel-graphics window for BASIC SCREEN 1-9.
 *
 * Implements gfx.h.  This file is compiled ONLY when HAVE_SDL is defined.
 * display_ansi.c remains the primary display/keyboard backend for SCREEN 0;
 * this file handles everything that happens after SCREEN 1-9 is called.
 *
 * Screen mode table (GW-BASIC / QBASIC):
 *   1  320×200  4-colour  CGA
 *   2  640×200  2-colour  CGA
 *   3  720×348  2-colour  Hercules (approximated)
 *   4  320×200  4-colour  (same as 1, alt palette)
 *   5  320×200  4-colour  EGA
 *   6  640×200  2-colour  EGA
 *   7  320×200 16-colour  EGA
 *   8  640×200 16-colour  EGA
 *   9  640×350 16-colour  EGA
 */
#ifdef HAVE_SDL

#include "gfx.h"
#include "basic_print.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <signal.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================
 * IBM CP437 8×8 bitmap font (printable ASCII + box-drawing)
 * Non-const so font_init() can patch the upper half at runtime.
 * ================================================================ */
static unsigned char g_font[256][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 00 */
    {0x7E,0x81,0xA5,0x81,0xBD,0x99,0x81,0x7E}, /* 01 */
    {0x7E,0xFF,0xDB,0xFF,0xC3,0xE7,0xFF,0x7E}, /* 02 */
    {0x6C,0xFE,0xFE,0xFE,0x7C,0x38,0x10,0x00}, /* 03 */
    {0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00}, /* 04 */
    {0x38,0x7C,0x38,0xFE,0xFE,0x10,0x10,0x7C}, /* 05 */
    {0x00,0x18,0x3C,0x7E,0xFF,0x7E,0x18,0x00}, /* 06 */
    {0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00}, /* 07 */
    {0xFF,0xFF,0xE7,0xC3,0xC3,0xE7,0xFF,0xFF}, /* 08 */
    {0x00,0x3C,0x66,0x42,0x42,0x66,0x3C,0x00}, /* 09 */
    {0xFF,0xC3,0x99,0xBD,0xBD,0x99,0xC3,0xFF}, /* 0A */
    {0x0F,0x07,0x0F,0x7D,0xCC,0xCC,0xCC,0x78}, /* 0B */
    {0x3C,0x66,0x66,0x66,0x3C,0x18,0x7E,0x18}, /* 0C */
    {0x3F,0x33,0x3F,0x30,0x30,0x70,0xF0,0xE0}, /* 0D */
    {0x7F,0x63,0x7F,0x63,0x63,0x67,0xE6,0xC0}, /* 0E */
    {0x18,0xDB,0x3C,0xE7,0xE7,0x3C,0xDB,0x18}, /* 0F */
    {0x80,0xE0,0xF8,0xFE,0xF8,0xE0,0x80,0x00}, /* 10 */
    {0x02,0x0E,0x3E,0xFE,0x3E,0x0E,0x02,0x00}, /* 11 */
    {0x18,0x3C,0x7E,0x18,0x18,0x7E,0x3C,0x18}, /* 12 */
    {0x66,0x66,0x66,0x66,0x66,0x00,0x66,0x00}, /* 13 */
    {0x7F,0xDB,0xDB,0x7B,0x1B,0x1B,0x1B,0x00}, /* 14 */
    {0x3E,0x63,0x38,0x6C,0x6C,0x38,0xCC,0x78}, /* 15 */
    {0x00,0x00,0x00,0x00,0x7E,0x7E,0x7E,0x00}, /* 16 */
    {0x18,0x3C,0x7E,0x18,0x7E,0x3C,0x18,0xFF}, /* 17 */
    {0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x00}, /* 18 */
    {0x18,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00}, /* 19 */
    {0x00,0x18,0x0C,0xFE,0x0C,0x18,0x00,0x00}, /* 1A */
    {0x00,0x30,0x60,0xFE,0x60,0x30,0x00,0x00}, /* 1B */
    {0x00,0x00,0xC0,0xC0,0xC0,0xFE,0x00,0x00}, /* 1C */
    {0x00,0x24,0x66,0xFF,0x66,0x24,0x00,0x00}, /* 1D */
    {0x00,0x18,0x3C,0x7E,0xFF,0xFF,0x00,0x00}, /* 1E */
    {0x00,0xFF,0xFF,0x7E,0x3C,0x18,0x00,0x00}, /* 1F */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 20 space */
    {0x30,0x78,0x78,0x30,0x30,0x00,0x30,0x00}, /* 21 ! */
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, /* 22 " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* 23 # */
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, /* 24 $ */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* 25 % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* 26 & */
    {0x60,0x60,0xC0,0x00,0x00,0x00,0x00,0x00}, /* 27 ' */
    {0x18,0x30,0x60,0x60,0x60,0x30,0x18,0x00}, /* 28 ( */
    {0x60,0x30,0x18,0x18,0x18,0x30,0x60,0x00}, /* 29 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 2A * */
    {0x00,0x30,0x30,0xFC,0x30,0x30,0x00,0x00}, /* 2B + */
    {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x60}, /* 2C , */
    {0x00,0x00,0x00,0xFC,0x00,0x00,0x00,0x00}, /* 2D - */
    {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00}, /* 2E . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* 2F / */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 30 0 */
    {0x30,0x70,0x30,0x30,0x30,0x30,0xFC,0x00}, /* 31 1 */
    {0x78,0xCC,0x0C,0x38,0x60,0xCC,0xFC,0x00}, /* 32 2 */
    {0x78,0xCC,0x0C,0x38,0x0C,0xCC,0x78,0x00}, /* 33 3 */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 34 4 */
    {0xFC,0xC0,0xF8,0x0C,0x0C,0xCC,0x78,0x00}, /* 35 5 */
    {0x38,0x60,0xC0,0xF8,0xCC,0xCC,0x78,0x00}, /* 36 6 */
    {0xFC,0xCC,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 37 7 */
    {0x78,0xCC,0xCC,0x78,0xCC,0xCC,0x78,0x00}, /* 38 8 */
    {0x78,0xCC,0xCC,0x7C,0x0C,0x18,0x70,0x00}, /* 39 9 */
    {0x00,0x30,0x30,0x00,0x00,0x30,0x30,0x00}, /* 3A : */
    {0x00,0x30,0x30,0x00,0x00,0x30,0x30,0x60}, /* 3B ; */
    {0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x00}, /* 3C < */
    {0x00,0x00,0xFC,0x00,0x00,0xFC,0x00,0x00}, /* 3D = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* 3E > */
    {0x78,0xCC,0x0C,0x18,0x30,0x00,0x30,0x00}, /* 3F ? */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* 40 @ */
    {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00}, /* 41 A */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* 42 B */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* 43 C */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* 44 D */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* 45 E */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* 46 F */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* 47 G */
    {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00}, /* 48 H */
    {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* 49 I */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* 4A J */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* 4B K */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* 4C L */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* 4D M */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 4E N */
    {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 4F O */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 50 P */
    {0x78,0xCC,0xCC,0xCC,0xDC,0x78,0x1C,0x00}, /* 51 Q */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* 52 R */
    {0x78,0xCC,0xE0,0x70,0x1C,0xCC,0x78,0x00}, /* 53 S */
    {0xFC,0xB4,0x30,0x30,0x30,0x30,0x78,0x00}, /* 54 T */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x00}, /* 55 U */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* 56 V */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* 57 W */
    {0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00}, /* 58 X */
    {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x78,0x00}, /* 59 Y */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* 5A Z */
    {0x78,0x60,0x60,0x60,0x60,0x60,0x78,0x00}, /* 5B [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* 5C \ */
    {0x78,0x18,0x18,0x18,0x18,0x18,0x78,0x00}, /* 5D ] */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* 5E ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 5F _ */
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00}, /* 60 ` */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* 61 a */
    {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00}, /* 62 b */
    {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00}, /* 63 c */
    {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00}, /* 64 d */
    {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00}, /* 65 e */
    {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00}, /* 66 f */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 67 g */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* 68 h */
    {0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00}, /* 69 i */
    {0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0xCC,0x78}, /* 6A j */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* 6B k */
    {0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* 6C l */
    {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00}, /* 6D m */
    {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xCC,0x00}, /* 6E n */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0x00}, /* 6F o */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* 70 p */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* 71 q */
    {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00}, /* 72 r */
    {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00}, /* 73 s */
    {0x10,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, /* 74 t */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* 75 u */
    {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* 76 v */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* 77 w */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* 78 x */
    {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 79 y */
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00}, /* 7A z */
    {0x1C,0x30,0x30,0xE0,0x30,0x30,0x1C,0x00}, /* 7B { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 7C | */
    {0xE0,0x30,0x30,0x1C,0x30,0x30,0xE0,0x00}, /* 7D } */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* 7E ~ */
    {0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0x00}, /* 7F */
    /* 0x80-0xFF: filled by font_init() */
};

static void font_init(void)
{
    /* Default upper half: solid block */
    for (int c = 0x80; c <= 0xFF; c++) memset(g_font[c], 0xFF, 8);

    /* Horizontal line ─ (0xC4) */
    memset(g_font[0xC4], 0, 8); g_font[0xC4][4] = 0xFF;
    /* Vertical line │ (0xB3) */
    memset(g_font[0xB3], 0, 8);
    for (int i = 0; i < 8; i++) g_font[0xB3][i] = 0x18;
    /* Corners ┌┐└┘ = DA BF C0 D9 */
    memset(g_font[0xDA],0,8); g_font[0xDA][4]=0xF8; for(int i=5;i<8;i++) g_font[0xDA][i]=0x18;
    memset(g_font[0xBF],0,8); g_font[0xBF][4]=0x1F; for(int i=5;i<8;i++) g_font[0xBF][i]=0x18;
    memset(g_font[0xC0],0,8); g_font[0xC0][4]=0xF8; for(int i=0;i<4;i++) g_font[0xC0][i]=0x18;
    memset(g_font[0xD9],0,8); g_font[0xD9][4]=0x1F; for(int i=0;i<4;i++) g_font[0xD9][i]=0x18;
    /* Cross + (0xC5) */
    memset(g_font[0xC5],0,8);
    for(int i=0;i<8;i++) g_font[0xC5][i]=0x18; g_font[0xC5][4]=0xFF;
    /* T-pieces ├┤┬┴ = C3 B4 C2 C1 */
    memset(g_font[0xC3],0,8); g_font[0xC3][4]=0xF8; for(int i=0;i<8;i++) g_font[0xC3][i]|=0x18;
    memset(g_font[0xB4],0,8); g_font[0xB4][4]=0x1F; for(int i=0;i<8;i++) g_font[0xB4][i]|=0x18;
    memset(g_font[0xC2],0,8); g_font[0xC2][4]=0xFF; for(int i=5;i<8;i++) g_font[0xC2][i]=0x18;
    memset(g_font[0xC1],0,8); g_font[0xC1][4]=0xFF; for(int i=0;i<4;i++) g_font[0xC1][i]=0x18;
    /* Shade blocks ░▒▓ = B0 B1 B2 */
    for(int r=0;r<8;r++){
        g_font[0xB0][r]=(r&1)?0x55:0xAA;
        g_font[0xB1][r]=(r&1)?0x77:0xDD;
        g_font[0xB2][r]=0xFF;
    }
    /* Full block █ (0xDB) */
    memset(g_font[0xDB], 0xFF, 8);
    /* Lower half block ▄ (0xDC) */
    memset(g_font[0xDC], 0, 8);
    for(int i=4;i<8;i++) g_font[0xDC][i]=0xFF;
    /* Upper half block ▀ (0xDF) */
    memset(g_font[0xDF], 0, 8);
    for(int i=0;i<4;i++) g_font[0xDF][i]=0xFF;
}

/* ================================================================
 * Screen mode descriptors
 * ================================================================ */
typedef struct { int w, h, cols, rows, colours; } ModeDesc;
static const ModeDesc g_modes[10] = {
    {  640,400, 80,25,16}, /* 0 — not used here, just for indexing */
    {  320,200, 40,25, 4},
    {  640,200, 80,25, 2},
    {  720,348, 90,43, 2},
    {  320,200, 40,25, 4},
    {  320,200, 40,25, 4},
    {  640,200, 80,25, 2},
    {  320,200, 40,25,16},
    {  640,200, 80,25,16},
    {  640,350, 80,43,16},
};

/* ================================================================
 * CGA 16-colour palette
 * ================================================================ */
static SDL_Color g_palette[16] = {
    {  0,  0,  0,255},{  0,  0,170,255},{  0,170,  0,255},{  0,170,170,255},
    {170,  0,  0,255},{170,  0,170,255},{170,170,  0,255},{170,170,170,255},
    { 85, 85, 85,255},{ 85, 85,255,255},{ 85,255, 85,255},{ 85,255,255,255},
    {255, 85, 85,255},{255, 85,255,255},{255,255, 85,255},{255,255,255,255},
};

/* ================================================================
 * State
 * ================================================================ */
static SDL_Window   *g_win    = NULL;
static SDL_Renderer *g_ren    = NULL;
static SDL_Texture  *g_tex    = NULL;
static uint8_t      *g_pixels = NULL;   /* 8bpp, palette-indexed */
static int           g_mode   = 0;
static int           g_w      = 0, g_h = 0;

static int  g_pen_x = 0, g_pen_y = 0, g_pen_col = 1;
static int  g_fg = 15, g_bg = 0;
static int  g_cur_row = 0, g_cur_col = 0;
static int  g_cursor_visible = 1;

/* g_break is owned by main.c — we set it on Ctrl+C */
extern volatile sig_atomic_t g_break;

/* Key ring buffer — polled by gfx_inkey() */
#define KBUF 32
static char g_kbuf[KBUF];
static int  g_khead = 0, g_ktail = 0;

/* ================================================================
 * Internal helpers
 * ================================================================ */
static inline void put_px(int x, int y, int c)
{
    if (x<0||x>=g_w||y<0||y>=g_h) return;
    g_pixels[y*g_w+x] = (uint8_t)(c&15);
}
static inline int get_px(int x, int y)
{
    if (x<0||x>=g_w||y<0||y>=g_h) return 0;
    return g_pixels[y*g_w+x];
}

static void do_flush(void)
{
    if (!g_tex||!g_pixels) return;
    uint32_t *px; int pitch;
    if (SDL_LockTexture(g_tex,NULL,(void**)&px,&pitch)!=0) return;
    for (int i=0;i<g_w*g_h;i++) {
        SDL_Color c = g_palette[g_pixels[i]&15];
        px[i] = 0xFF000000u|((uint32_t)c.r<<16)|((uint32_t)c.g<<8)|c.b;
    }
    /* Draw cursor: a 2-pixel-tall underline at the current text position */
    if (g_cursor_visible && g_mode >= 1 && g_mode <= 9) {
        const ModeDesc *m = &g_modes[g_mode];
        int cw = g_w / m->cols;
        int rh = g_h / m->rows;
        int cx = g_cur_col * cw;
        int cy = g_cur_row * rh + rh - 2;
        SDL_Color cc = g_palette[g_fg & 15];
        uint32_t ccol = 0xFF000000u|((uint32_t)cc.r<<16)|((uint32_t)cc.g<<8)|cc.b;
        for (int row = 0; row < 2; row++)
            for (int col = 0; col < cw; col++) {
                int idx = (cy + row) * g_w + cx + col;
                if (idx >= 0 && idx < g_w * g_h) px[idx] = ccol;
            }
    }
    SDL_UnlockTexture(g_tex);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren,g_tex,NULL,NULL);
    SDL_RenderPresent(g_ren);
}

static void kbuf_push(char c)
{
    int n = (g_ktail+1) % KBUF;
    if (n != g_khead) { g_kbuf[g_ktail] = c; g_ktail = n; }
}

static void pump(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);

        /* SDL_TEXTINPUT gives us the correct shifted/locale character
           for any printable key — handles shift, caps lock, AltGr, etc. */
        if (e.type == SDL_TEXTINPUT) {
            /* text is UTF-8; pass through ASCII bytes only */
            for (const char *p = e.text.text; *p; p++) {
                unsigned char uc = (unsigned char)*p;
                if (uc >= 32 && uc < 127) kbuf_push((char)uc);
            }
        }

        /* KEYDOWN only for non-printable control keys */
        if (e.type == SDL_KEYDOWN) {
            SDL_Keycode sym = e.key.keysym.sym;
            /* Ctrl+C → break */
            if (sym == SDLK_c &&
                (e.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))) {
                g_break = 1;
                continue;
            }
            char c = 0;
            switch (sym) {
            case SDLK_RETURN:    case SDLK_KP_ENTER: c = '\r'; break;
            case SDLK_BACKSPACE:                      c = '\b'; break;
            case SDLK_ESCAPE:                         c = 27;   break;
            case SDLK_TAB:                            c = '\t'; break;
            case SDLK_UP:                             c = 'H';  break;
            case SDLK_DOWN:                           c = 'P';  break;
            case SDLK_LEFT:                           c = 'K';  break;
            case SDLK_RIGHT:                          c = 'M';  break;
            case SDLK_DELETE:                         c = 127;  break;
            default: break;
            }
            if (c) kbuf_push(c);
        }
    }
}

static void draw_char_at(int row, int col, unsigned char ch, int fg, int bg)
{
    int cw = g_w / g_modes[g_mode].cols;
    int rh = g_h / g_modes[g_mode].rows;
    int px = col*cw, py = row*rh;
    for (int y=0;y<rh;y++) {
        uint8_t bits = g_font[ch][y<8?y:7];
        for (int x=0;x<cw;x++) {
            int bit = 7-(x*8/cw);
            put_px(px+x, py+y, (bits>>bit)&1 ? fg : bg);
        }
    }
}

/* ================================================================
 * gfx.h implementation
 * ================================================================ */

void gfx_open(int mode)
{
    if (mode<1||mode>9) mode=1;
    font_init();

    const ModeDesc *m = &g_modes[mode];
    g_mode = mode; g_w = m->w; g_h = m->h;
    g_fg = m->colours-1; g_bg = 0;
    g_cur_row=0; g_cur_col=0;
    g_pen_x=0; g_pen_y=0; g_pen_col=1;

    /* Reset palette to CGA defaults */
    SDL_Color cga[16] = {
        {0,0,0,255},{0,0,170,255},{0,170,0,255},{0,170,170,255},
        {170,0,0,255},{170,0,170,255},{170,170,0,255},{170,170,170,255},
        {85,85,85,255},{85,85,255,255},{85,255,85,255},{85,255,255,255},
        {255,85,85,255},{255,85,255,255},{255,255,85,255},{255,255,255,255},
    };
    memcpy(g_palette, cga, sizeof(cga));

    if (!g_win) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO|SDL_INIT_EVENTS) < 0) {
            basic_stderr("gfx: SDL_Init: %s\n", SDL_GetError()); return;
        }
        g_win = SDL_CreateWindow("WOPR BASIC",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            g_w*2, g_h*2,
            SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
        if (!g_win) { basic_stderr("gfx: SDL_CreateWindow: %s\n",SDL_GetError()); return; }
        g_ren = SDL_CreateRenderer(g_win,-1,
            SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if (!g_ren)
            g_ren = SDL_CreateRenderer(g_win,-1,SDL_RENDERER_SOFTWARE);
    } else {
        SDL_SetWindowSize(g_win, g_w*2, g_h*2);
    }
    SDL_RenderSetLogicalSize(g_ren, g_w, g_h);
    SDL_StartTextInput();   /* enable SDL_TEXTINPUT events for correct shifted chars */

    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex=NULL; }
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, g_w, g_h);
    free(g_pixels);
    g_pixels = calloc(g_w*g_h, 1);
    gfx_cls(g_bg);
}

void gfx_close(void)
{
    SDL_StopTextInput();
    if (g_tex)  { SDL_DestroyTexture(g_tex);  g_tex=NULL; }
    if (g_ren)  { SDL_DestroyRenderer(g_ren); g_ren=NULL; }
    if (g_win)  { SDL_DestroyWindow(g_win);   g_win=NULL; }
    free(g_pixels); g_pixels=NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO|SDL_INIT_EVENTS);
    g_mode=0;
}

int gfx_is_open(void)  { return g_win != NULL; }
int gfx_get_mode(void) { return g_mode; }
int gfx_get_cols(void) { return g_mode >= 1 && g_mode <= 9 ? g_modes[g_mode].cols : 80; }

void gfx_palette(int idx, int r, int g, int b)
{
    if (idx<0||idx>15) return;
    g_palette[idx] = (SDL_Color){(uint8_t)r,(uint8_t)g,(uint8_t)b,255};
    do_flush();
}

void gfx_cls(int bg)
{
    if (!g_pixels) return;
    g_bg = bg & 15;
    memset(g_pixels, (uint8_t)g_bg, g_w*g_h);
    g_cur_row=0; g_cur_col=0;
    do_flush();
}

void gfx_flush(void) { pump(); do_flush(); }

void gfx_pset(int x, int y, int c)
{
    if (c<0) c=g_pen_col;
    put_px(x,y,c);
    g_pen_x=x; g_pen_y=y;
    do_flush();
}

int gfx_point(int x, int y) { return get_px(x,y); }

void gfx_line(int x1, int y1, int x2, int y2, int colour, int style)
{
    if (colour<0) colour=g_pen_col;
    if (style==1) { /* BOX outline */
        gfx_line(x1,y1,x2,y1,colour,0);
        gfx_line(x2,y1,x2,y2,colour,0);
        gfx_line(x2,y2,x1,y2,colour,0);
        gfx_line(x1,y2,x1,y1,colour,0);
        do_flush(); return;
    }
    if (style==2) { /* BF filled box */
        if(x1>x2){int t=x1;x1=x2;x2=t;} if(y1>y2){int t=y1;y1=y2;y2=t;}
        for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++) put_px(x,y,colour);
        do_flush(); return;
    }
    /* Bresenham */
    int dx=abs(x2-x1), dy=abs(y2-y1);
    int sx=x1<x2?1:-1, sy=y1<y2?1:-1, err=dx-dy;
    for(;;) {
        put_px(x1,y1,colour);
        if(x1==x2&&y1==y2) break;
        int e2=2*err;
        if(e2>-dy){err-=dy;x1+=sx;}
        if(e2< dx){err+=dx;y1+=sy;}
    }
    g_pen_x=x2; g_pen_y=y2;
    do_flush();
}

/* Midpoint ellipse — 4-fold symmetry */
#define PUT4(cx,cy,dx,dy,col) \
    put_px((cx)+(dx),(cy)+(dy),(col)); put_px((cx)-(dx),(cy)+(dy),(col)); \
    put_px((cx)+(dx),(cy)-(dy),(col)); put_px((cx)-(dx),(cy)-(dy),(col))

void gfx_circle(int cx, int cy, int r, int colour,
                double aspect, double sa, double ea)
{
    if (colour<0) colour=g_pen_col;
    const ModeDesc *m = &g_modes[g_mode];

    /* Default aspect: correct for non-square pixels */
    if (aspect<=0) aspect = (double)m->h/m->w * ((double)m->cols/m->rows);

    int rx=r, ry=(int)(r*aspect+0.5);
    if (rx<1) rx=1; if (ry<1) ry=1;

    if (sa<0 && ea<0) {
        /* Full ellipse via midpoint algorithm */
        long rx2=(long)rx*rx, ry2=(long)ry*ry;
        int x=0, y=ry;
        long p = ry2 - rx2*ry + rx2/4;
        while (2*ry2*x < 2*rx2*y) {
            PUT4(cx,cy,x,y,colour);
            if(p<0) p+=2*ry2*(2*x+3);
            else { p+=2*ry2*(2*x+3)-4*rx2*(y-1); y--; }
            x++;
        }
        p=(long)(ry2*(x+0.5)*(x+0.5))+(long)(rx2*(y-1)*(y-1))-(long)(rx2*ry2);
        while (y>=0) {
            PUT4(cx,cy,x,y,colour);
            if(p>0) p+=-2*rx2*(2*y-3);
            else { p+=2*ry2*(2*x+2)-2*rx2*(2*y-3); x++; }
            y--;
        }
    } else {
        /* Arc: sample by angle */
        if (sa<0) sa=0; if (ea<0) ea=2*M_PI;
        double step=1.0/(rx>ry?rx:ry);
        for (double a=sa; a<=ea+step/2; a+=step) {
            double ang = a>ea?ea:a;
            put_px(cx+(int)(rx*cos(ang)), cy-(int)(ry*sin(ang)), colour);
        }
        /* Radii for pie slices (negative angles in GW-BASIC convention) */
        int xs=cx+(int)(rx*cos(sa)), ys=cy-(int)(ry*sin(sa));
        int xe=cx+(int)(rx*cos(ea)), ye=cy-(int)(ry*sin(ea));
        gfx_line(cx,cy,xs,ys,colour,0);
        gfx_line(cx,cy,xe,ye,colour,0);
    }
    do_flush();
}
#undef PUT4

void gfx_paint(int x, int y, int pc, int bc)
{
    int target=get_px(x,y);
    if (target==pc) return;
    typedef struct{short x,y;} Pt;
    int cap=g_w*g_h;
    Pt *stk=malloc(cap*sizeof(Pt));
    if(!stk) return;
    int top=0;
    stk[top++]=(Pt){(short)x,(short)y};
    while(top>0) {
        Pt p=stk[--top];
        int cx=p.x, cy=p.y;
        if(cx<0||cx>=g_w||cy<0||cy>=g_h) continue;
        int cur=get_px(cx,cy);
        if(cur==pc||cur==bc) continue;
        put_px(cx,cy,pc);
        if(top+4<cap) {
            stk[top++]=(Pt){(short)(cx+1),(short)cy};
            stk[top++]=(Pt){(short)(cx-1),(short)cy};
            stk[top++]=(Pt){(short)cx,(short)(cy+1)};
            stk[top++]=(Pt){(short)cx,(short)(cy-1)};
        }
    }
    free(stk);
    do_flush();
}

void gfx_draw(const char *s)
{
    int x=g_pen_x, y=g_pen_y, colour=g_pen_col, scale=1;
    const char *p=s;
    while (*p) {
        while(isspace((unsigned char)*p)||*p==';') p++;
        if(!*p) break;
        char cmd=(char)toupper((unsigned char)*p++);
        int n=1, has_n=0;
        if(*p=='-'||isdigit((unsigned char)*p)){
            has_n=1; n=(int)strtol(p,(char**)&p,10);
        }
        (void)has_n;
        int d=n*scale, nx=x, ny=y, plot=1;
        switch(cmd) {
        case 'U': ny=y-d; break; case 'D': ny=y+d; break;
        case 'L': nx=x-d; break; case 'R': nx=x+d; break;
        case 'E': nx=x+d; ny=y-d; break;
        case 'F': nx=x+d; ny=y+d; break;
        case 'G': nx=x-d; ny=y+d; break;
        case 'H': nx=x-d; ny=y-d; break;
        case 'M': {
            int rel=0;
            if(*p=='+'||*p=='-'){rel=(*p=='-')?-1:1;p++;}
            int mx=(int)strtol(p,(char**)&p,10);
            if(*p==',')p++;
            int my=(int)strtol(p,(char**)&p,10);
            nx=rel?x+mx*rel:mx; ny=rel?y+my*rel:my; break;
        }
        case 'B': { /* Blind move: next command, no draw */
            plot=0;
            while(isspace((unsigned char)*p))p++;
            if(*p){
                char c2=(char)toupper((unsigned char)*p++);
                int n2=isdigit((unsigned char)*p)?(int)strtol(p,(char**)&p,10):1;
                int d2=n2*scale;
                switch(c2){
                case 'U':ny=y-d2;break;case 'D':ny=y+d2;break;
                case 'L':nx=x-d2;break;case 'R':nx=x+d2;break;
                case 'E':nx=x+d2;ny=y-d2;break;case 'F':nx=x+d2;ny=y+d2;break;
                case 'G':nx=x-d2;ny=y+d2;break;case 'H':nx=x-d2;ny=y-d2;break;
                default:break;
                }
            }
            break;
        }
        case 'N': { /* Draw and return */
            while(isspace((unsigned char)*p))p++;
            if(*p){
                char c2=(char)toupper((unsigned char)*p++);
                int n2=isdigit((unsigned char)*p)?(int)strtol(p,(char**)&p,10):1;
                int d2=n2*scale, rx2=x, ry2=y;
                switch(c2){
                case 'U':ry2=y-d2;break;case 'D':ry2=y+d2;break;
                case 'L':rx2=x-d2;break;case 'R':rx2=x+d2;break;
                case 'E':rx2=x+d2;ry2=y-d2;break;case 'F':rx2=x+d2;ry2=y+d2;break;
                case 'G':rx2=x-d2;ry2=y+d2;break;case 'H':rx2=x-d2;ry2=y-d2;break;
                default:break;
                }
                gfx_line(x,y,rx2,ry2,colour,0);
            }
            continue;
        }
        case 'C': colour=n&15; g_pen_col=colour; continue;
        case 'S': scale=(n<1)?1:n; continue;
        case 'P': {
            int pc=n&15, bc=pc;
            if(*p==','){p++;bc=(int)strtol(p,(char**)&p,10)&15;}
            gfx_paint(x,y,pc,bc); continue;
        }
        case 'A': case 'T': continue; /* angle stub */
        case 'X': continue;           /* execute substring stub */
        default:  continue;
        }
        if(plot) gfx_line(x,y,nx,ny,colour,0);
        x=nx; y=ny;
        if(x<0)x=0; if(x>=g_w)x=g_w-1;
        if(y<0)y=0; if(y>=g_h)y=g_h-1;
    }
    g_pen_x=x; g_pen_y=y;
    do_flush();
}

void gfx_get_pen(int *x, int *y)          { *x=g_pen_x; *y=g_pen_y; }
void gfx_set_pen(int x, int y, int colour) {
    g_pen_x=x; g_pen_y=y; if(colour>=0) g_pen_col=colour;
}

void gfx_color(int fg, int bg)
{
    if(fg>=0&&fg<=15) g_fg=fg;
    if(bg>=0&&bg<=15) g_bg=bg;
}

void gfx_cursor(int visible)
{
    g_cursor_visible = visible;
    do_flush();
}

void gfx_locate(int row, int col)
{
    const ModeDesc *m=&g_modes[g_mode];
    g_cur_row=(row-1<0)?0:(row-1>=m->rows?m->rows-1:row-1);
    g_cur_col=(col-1<0)?0:(col-1>=m->cols?m->cols-1:col-1);
}

void gfx_print_char(unsigned char ch, int fg, int bg)
{
    if (fg < 0) fg = g_fg;
    if (bg < 0) bg = g_bg;
    const ModeDesc *m=&g_modes[g_mode];
    if (ch=='\n') {
        g_cur_col=0; g_cur_row++;
        if (g_cur_row>=m->rows) {
            /* Scroll */
            int rh=g_h/m->rows;
            memmove(g_pixels, g_pixels+g_w*rh, g_w*(m->rows-1)*rh);
            memset(g_pixels+g_w*(m->rows-1)*rh, (uint8_t)g_bg, g_w*rh);
            g_cur_row=m->rows-1;
        }
        do_flush(); return;
    }
    if (ch=='\r') { g_cur_col=0; return; }
    if (ch=='\b') { if(g_cur_col>0) g_cur_col--; return; }

    draw_char_at(g_cur_row, g_cur_col, ch, fg, bg);
    g_cur_col++;
    if (g_cur_col>=m->cols) {
        g_cur_col=0; g_cur_row++;
        if (g_cur_row>=m->rows) {
            int rh=g_h/m->rows;
            memmove(g_pixels, g_pixels+g_w*rh, g_w*(m->rows-1)*rh);
            memset(g_pixels+g_w*(m->rows-1)*rh,(uint8_t)g_bg,g_w*rh);
            g_cur_row=m->rows-1;
        }
    }
    pump();
    do_flush();
}

int gfx_inkey(void)
{
    pump();
    if (g_khead==g_ktail) return 0;
    char c=g_kbuf[g_khead]; g_khead=(g_khead+1)%KBUF;
    return (unsigned char)c;
}

int gfx_getchar(void)
{
    for (;;) {
        int c = gfx_inkey();
        if (c) return c;
        SDL_Delay(10);
    }
}

int gfx_getline(char *buf, int bufsz)
{
    int len = 0;
    buf[0] = '\0';
    for (;;) {
        int c = gfx_getchar();
        if (c == '\r' || c == '\n') {
            gfx_print_char('\n', g_fg, g_bg);
            break;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                /* Erase on screen: back up cursor and overwrite with space */
                if (g_cur_col > 0) g_cur_col--;
                draw_char_at(g_cur_row, g_cur_col, ' ', g_fg, g_bg);
                do_flush();
            }
        } else if (c >= 32 && len < bufsz - 1) {
            buf[len++] = (char)c;
            buf[len]   = '\0';
            gfx_print_char((unsigned char)c, g_fg, g_bg);
        }
    }
    return len;
}

#endif /* HAVE_SDL */
