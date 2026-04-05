/*
 * display_sdl.c — SDL2 display backend for the BASIC interpreter.
 *
 * Implements display.h for both text mode (SCREEN 0) and the standard
 * CGA/EGA/VGA graphics modes (SCREEN 1-28).
 *
 * Screen mode table (matching GW-BASIC / QBASIC):
 *   0  — 80×25 text, 16 colours (default)
 *   1  — 320×200 graphics, 4-colour CGA palette
 *   2  — 640×200 graphics, 2-colour (BW)
 *   3  — 720×348 Hercules (approximated)
 *   4  — 320×200, 4-colour (same as 1, alternate palette)
 *   5  — 320×200, 4-colour EGA
 *   6  — 640×200, 2-colour EGA
 *   7  — 320×200, 16-colour EGA
 *   8  — 640×200, 16-colour EGA
 *   9  — 640×350, 16-colour EGA
 *   ...
 * Text rendering uses a built-in 8×8 IBM CP437 bitmap font.
 *
 * Build: compiled automatically when HAVE_SDL is defined (via Makefile).
 */
#ifdef HAVE_SDL

#include "display.h"
#include "sound.h"
#include "basic_print.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <math.h>
#include <ctype.h>

/* ----------------------------------------------------------------
 * Suppress the printf→basic_printf redirection inside this file
 * so our own debug paths still work (they use basic_stderr anyway).
 * ---------------------------------------------------------------- */

/* ================================================================
 * IBM CP437 8×8 bitmap font — 256 chars, 8 bytes each
 * Only the ASCII printable range (0x20–0x7E) plus a few extras
 * are filled in here; everything else falls back to a solid block.
 * ================================================================ */
/* The full 2KB table below was generated from the classic IBM BIOS
   ROM font (public domain reconstruction). */
static unsigned char font8x8[256][8] = {
    /* 0x00 */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x01 */ {0x7E,0x81,0xA5,0x81,0xBD,0x99,0x81,0x7E},
    /* 0x02 */ {0x7E,0xFF,0xDB,0xFF,0xC3,0xE7,0xFF,0x7E},
    /* 0x03 */ {0x6C,0xFE,0xFE,0xFE,0x7C,0x38,0x10,0x00},
    /* 0x04 */ {0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00},
    /* 0x05 */ {0x38,0x7C,0x38,0xFE,0xFE,0x10,0x10,0x7C},
    /* 0x06 */ {0x00,0x18,0x3C,0x7E,0xFF,0x7E,0x18,0x00},
    /* 0x07 */ {0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00},
    /* 0x08 */ {0xFF,0xFF,0xE7,0xC3,0xC3,0xE7,0xFF,0xFF},
    /* 0x09 */ {0x00,0x3C,0x66,0x42,0x42,0x66,0x3C,0x00},
    /* 0x0A */ {0xFF,0xC3,0x99,0xBD,0xBD,0x99,0xC3,0xFF},
    /* 0x0B */ {0x0F,0x07,0x0F,0x7D,0xCC,0xCC,0xCC,0x78},
    /* 0x0C */ {0x3C,0x66,0x66,0x66,0x3C,0x18,0x7E,0x18},
    /* 0x0D */ {0x3F,0x33,0x3F,0x30,0x30,0x70,0xF0,0xE0},
    /* 0x0E */ {0x7F,0x63,0x7F,0x63,0x63,0x67,0xE6,0xC0},
    /* 0x0F */ {0x18,0xDB,0x3C,0xE7,0xE7,0x3C,0xDB,0x18},
    /* 0x10 */ {0x80,0xE0,0xF8,0xFE,0xF8,0xE0,0x80,0x00},
    /* 0x11 */ {0x02,0x0E,0x3E,0xFE,0x3E,0x0E,0x02,0x00},
    /* 0x12 */ {0x18,0x3C,0x7E,0x18,0x18,0x7E,0x3C,0x18},
    /* 0x13 */ {0x66,0x66,0x66,0x66,0x66,0x00,0x66,0x00},
    /* 0x14 */ {0x7F,0xDB,0xDB,0x7B,0x1B,0x1B,0x1B,0x00},
    /* 0x15 */ {0x3E,0x63,0x38,0x6C,0x6C,0x38,0xCC,0x78},
    /* 0x16 */ {0x00,0x00,0x00,0x00,0x7E,0x7E,0x7E,0x00},
    /* 0x17 */ {0x18,0x3C,0x7E,0x18,0x7E,0x3C,0x18,0xFF},
    /* 0x18 */ {0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x00},
    /* 0x19 */ {0x18,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00},
    /* 0x1A */ {0x00,0x18,0x0C,0xFE,0x0C,0x18,0x00,0x00},
    /* 0x1B */ {0x00,0x30,0x60,0xFE,0x60,0x30,0x00,0x00},
    /* 0x1C */ {0x00,0x00,0xC0,0xC0,0xC0,0xFE,0x00,0x00},
    /* 0x1D */ {0x00,0x24,0x66,0xFF,0x66,0x24,0x00,0x00},
    /* 0x1E */ {0x00,0x18,0x3C,0x7E,0xFF,0xFF,0x00,0x00},
    /* 0x1F */ {0x00,0xFF,0xFF,0x7E,0x3C,0x18,0x00,0x00},
    /* 0x20 */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    /* 0x21 */ {0x30,0x78,0x78,0x30,0x30,0x00,0x30,0x00}, /* ! */
    /* 0x22 */ {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, /* " */
    /* 0x23 */ {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* # */
    /* 0x24 */ {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, /* $ */
    /* 0x25 */ {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* % */
    /* 0x26 */ {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* & */
    /* 0x27 */ {0x60,0x60,0xC0,0x00,0x00,0x00,0x00,0x00}, /* ' */
    /* 0x28 */ {0x18,0x30,0x60,0x60,0x60,0x30,0x18,0x00}, /* ( */
    /* 0x29 */ {0x60,0x30,0x18,0x18,0x18,0x30,0x60,0x00}, /* ) */
    /* 0x2A */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    /* 0x2B */ {0x00,0x30,0x30,0xFC,0x30,0x30,0x00,0x00}, /* + */
    /* 0x2C */ {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x60}, /* , */
    /* 0x2D */ {0x00,0x00,0x00,0xFC,0x00,0x00,0x00,0x00}, /* - */
    /* 0x2E */ {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00}, /* . */
    /* 0x2F */ {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* / */
    /* 0x30 */ {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 0 */
    /* 0x31 */ {0x30,0x70,0x30,0x30,0x30,0x30,0xFC,0x00}, /* 1 */
    /* 0x32 */ {0x78,0xCC,0x0C,0x38,0x60,0xCC,0xFC,0x00}, /* 2 */
    /* 0x33 */ {0x78,0xCC,0x0C,0x38,0x0C,0xCC,0x78,0x00}, /* 3 */
    /* 0x34 */ {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 4 */
    /* 0x35 */ {0xFC,0xC0,0xF8,0x0C,0x0C,0xCC,0x78,0x00}, /* 5 */
    /* 0x36 */ {0x38,0x60,0xC0,0xF8,0xCC,0xCC,0x78,0x00}, /* 6 */
    /* 0x37 */ {0xFC,0xCC,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 */
    /* 0x38 */ {0x78,0xCC,0xCC,0x78,0xCC,0xCC,0x78,0x00}, /* 8 */
    /* 0x39 */ {0x78,0xCC,0xCC,0x7C,0x0C,0x18,0x70,0x00}, /* 9 */
    /* 0x3A */ {0x00,0x30,0x30,0x00,0x00,0x30,0x30,0x00}, /* : */
    /* 0x3B */ {0x00,0x30,0x30,0x00,0x00,0x30,0x30,0x60}, /* ; */
    /* 0x3C */ {0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x00}, /* < */
    /* 0x3D */ {0x00,0x00,0xFC,0x00,0x00,0xFC,0x00,0x00}, /* = */
    /* 0x3E */ {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* > */
    /* 0x3F */ {0x78,0xCC,0x0C,0x18,0x30,0x00,0x30,0x00}, /* ? */
    /* 0x40 */ {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* @ */
    /* 0x41 */ {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00}, /* A */
    /* 0x42 */ {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* B */
    /* 0x43 */ {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* C */
    /* 0x44 */ {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* D */
    /* 0x45 */ {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* E */
    /* 0x46 */ {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* F */
    /* 0x47 */ {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* G */
    /* 0x48 */ {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00}, /* H */
    /* 0x49 */ {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* I */
    /* 0x4A */ {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* J */
    /* 0x4B */ {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* K */
    /* 0x4C */ {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* L */
    /* 0x4D */ {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* M */
    /* 0x4E */ {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* N */
    /* 0x4F */ {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* O */
    /* 0x50 */ {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* P */
    /* 0x51 */ {0x78,0xCC,0xCC,0xCC,0xDC,0x78,0x1C,0x00}, /* Q */
    /* 0x52 */ {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* R */
    /* 0x53 */ {0x78,0xCC,0xE0,0x70,0x1C,0xCC,0x78,0x00}, /* S */
    /* 0x54 */ {0xFC,0xB4,0x30,0x30,0x30,0x30,0x78,0x00}, /* T */
    /* 0x55 */ {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x00}, /* U */
    /* 0x56 */ {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* V */
    /* 0x57 */ {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* W */
    /* 0x58 */ {0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00}, /* X */
    /* 0x59 */ {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x78,0x00}, /* Y */
    /* 0x5A */ {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* Z */
    /* 0x5B */ {0x78,0x60,0x60,0x60,0x60,0x60,0x78,0x00}, /* [ */
    /* 0x5C */ {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* \ */
    /* 0x5D */ {0x78,0x18,0x18,0x18,0x18,0x18,0x78,0x00}, /* ] */
    /* 0x5E */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* ^ */
    /* 0x5F */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    /* 0x60 */ {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    /* 0x61 */ {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* a */
    /* 0x62 */ {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00}, /* b */
    /* 0x63 */ {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00}, /* c */
    /* 0x64 */ {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00}, /* d */
    /* 0x65 */ {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00}, /* e */
    /* 0x66 */ {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00}, /* f */
    /* 0x67 */ {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* g */
    /* 0x68 */ {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* h */
    /* 0x69 */ {0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00}, /* i */
    /* 0x6A */ {0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0xCC,0x78}, /* j */
    /* 0x6B */ {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* k */
    /* 0x6C */ {0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* l */
    /* 0x6D */ {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00}, /* m */
    /* 0x6E */ {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xCC,0x00}, /* n */
    /* 0x6F */ {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0x00}, /* o */
    /* 0x70 */ {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* p */
    /* 0x71 */ {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* q */
    /* 0x72 */ {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00}, /* r */
    /* 0x73 */ {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00}, /* s */
    /* 0x74 */ {0x10,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, /* t */
    /* 0x75 */ {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* u */
    /* 0x76 */ {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* v */
    /* 0x77 */ {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* w */
    /* 0x78 */ {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* x */
    /* 0x79 */ {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8}, /* y */
    /* 0x7A */ {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00}, /* z */
    /* 0x7B */ {0x1C,0x30,0x30,0xE0,0x30,0x30,0x1C,0x00}, /* { */
    /* 0x7C */ {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    /* 0x7D */ {0xE0,0x30,0x30,0x1C,0x30,0x30,0xE0,0x00}, /* } */
    /* 0x7E */ {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
    /* 0x7F */ {0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0x00}, /* DEL */
    /* 0x80-0xFF filled by font_fill_upper() at init */
};

static void font_fill_upper(void)
{
    /* Fill 0x80-0xFF with solid block by default */
    for (int c = 0x80; c <= 0xFF; c++)
        memset(font8x8[c], 0xFF, 8);
    /* horizontal line ─ 0xC4 */
    memset(font8x8[0xC4], 0, 8);
    font8x8[0xC4][4] = 0xFF;
    /* vertical line │ 0xB3 */
    memset(font8x8[0xB3], 0, 8);
    for (int i = 0; i < 8; i++) font8x8[0xB3][i] = 0x18;
    /* corners: ┌0xDA ┐0xBF └0xC0 ┘0xD9 */
    memset(font8x8[0xDA], 0, 8);
    font8x8[0xDA][4] = 0xF8;
    for (int i = 5; i < 8; i++) font8x8[0xDA][i] = 0x18;
    memset(font8x8[0xBF], 0, 8);
    font8x8[0xBF][4] = 0x1F;
    for (int i = 5; i < 8; i++) font8x8[0xBF][i] = 0x18;
    memset(font8x8[0xC0], 0, 8);
    for (int i = 0; i < 4; i++) font8x8[0xC0][i] = 0x18;
    font8x8[0xC0][4] = 0xF8;
    memset(font8x8[0xD9], 0, 8);
    for (int i = 0; i < 4; i++) font8x8[0xD9][i] = 0x18;
    font8x8[0xD9][4] = 0x1F;
    /* cross + 0xC5 */
    memset(font8x8[0xC5], 0, 8);
    for (int i = 0; i < 8; i++) font8x8[0xC5][i] = 0x18;
    font8x8[0xC5][4] = 0xFF;
    /* T-pieces: ├0xC3 ┤0xB4 ┬0xC2 ┴0xC1 */
    memset(font8x8[0xC3], 0, 8);
    for (int i = 0; i < 8; i++) font8x8[0xC3][i] = 0x18;
    font8x8[0xC3][4] = 0xF8;
    memset(font8x8[0xB4], 0, 8);
    for (int i = 0; i < 8; i++) font8x8[0xB4][i] = 0x18;
    font8x8[0xB4][4] = 0x1F;
    memset(font8x8[0xC2], 0, 8);
    font8x8[0xC2][4] = 0xFF;
    for (int i = 5; i < 8; i++) font8x8[0xC2][i] = 0x18;
    memset(font8x8[0xC1], 0, 8);
    font8x8[0xC1][4] = 0xFF;
    for (int i = 0; i < 4; i++) font8x8[0xC1][i] = 0x18;
    /* shade blocks ░▒▓ — 0xB0 0xB1 0xB2 */
    for (int r = 0; r < 8; r++) {
        font8x8[0xB0][r] = (r & 1) ? 0x55 : 0xAA;
        font8x8[0xB1][r] = (r & 1) ? 0x77 : 0xDD;
        font8x8[0xB2][r] = (r & 1) ? 0xFF : 0xFF;
    }
}

/* ================================================================
 * Screen mode descriptors
 * ================================================================ */
typedef struct {
    int gfx_w, gfx_h;   /* pixel dimensions */
    int text_cols, text_rows;
    int colours;         /* max colours */
    int text_mode;       /* 1 = text-only mode */
} ScreenMode;

static const ScreenMode screen_modes[29] = {
    /*  0 */ { 720, 350, 80, 25, 16, 1 },   /* Text mode (All) - 720x350 effective, 16/64 colors */
    /*  1 */ { 320, 200, 40, 25,  4, 0 },   /* CGA/EGA/VGA/MCGA */
    /*  2 */ { 640, 200, 80, 25,  2, 0 },   /* CGA/EGA/VGA/MCGA */
    /*  3 */ { 720, 348, 90, 43,  2, 0 },   /* Hercules (HGC) / HICC InColor */
    /*  4 */ { 640, 400, 80, 25,  2, 0 },   /* Olivetti / AT&T */
    /*  5 */ { 160, 100, 20, 12, 16, 0 },   /* CGA (rare low-res) */
    /*  6 */ { 160, 200, 20, 25, 16, 0 },   /* CGA (rare low-res) */
    /*  7 */ { 320, 200, 40, 25, 16, 0 },   /* EGA/VGA */
    /*  8 */ { 640, 200, 80, 25, 16, 0 },   /* EGA/VGA */
    /*  9 */ { 640, 350, 80, 43, 16, 0 },   /* EGA/VGA - 64 colors with >64KB VRAM */
    /* 10 */ { 640, 350, 80, 43,  2, 0 },   /* EGA/VGA - Monochrome */
    /* 11 */ { 640, 480, 80, 30,  2, 0 },   /* VGA/MCGA - Monochrome */
    /* 12 */ { 640, 480, 80, 30, 16, 0 },   /* VGA */
    /* 13 */ { 320, 200, 40, 25, 256, 0 },  /* VGA/MCGA */
    /* 14 */ { 320, 200, 40, 25, 16, 0 },   /* Plantronics Colorplus (PCP) */
    /* 15 */ { 640, 200, 80, 25,  4, 0 },   /* Plantronics Colorplus (PCP) */
    /* 16 */ { 640, 480, 80, 30, 256, 0 },  /* Professional Graphics Controller (PGC) */
    /* 17 */ { 640, 480, 80, 30, 256, 0 },  /* IBM 8514/A */
    /* 18 */ { 640, 480, 80, 30, 16, 0 },   /* JEGA */
    /* 19 */ { 640, 480, 80, 30, 16, 1 },   /* JEGA - Text mode variant */
    /* 20 */ { 512, 480, 64, 30, 256, 0 },  /* TIGA */
    /* 21 */ { 640, 400, 80, 25, 256, 0 },  /* SVGA */
    /* 22 */ { 640, 480, 80, 30, 256, 0 },  /* SVGA */
    /* 23 */ { 800, 600, 100, 37, 256, 0 }, /* SVGA */
    /* 24 */ { 160, 200, 20, 25, 16, 0 },   /* Tandy / PCjr */
    /* 25 */ { 320, 200, 40, 25, 16, 0 },   /* Tandy / PCjr */
    /* 26 */ { 640, 200, 80, 25,  4, 0 },   /* Tandy / PCjr */
    /* 27 */ { 640, 200, 80, 25, 16, 0 },   /* Tandy Video II or ETGA */
    /* 28 */ { 720, 350, 80, 25,  2, 0 }    /* OGA */
};

/* ================================================================
 * CGA palette (standard 16-colour)
 * ================================================================ */
static const SDL_Color cga16[16] = {
    {  0,   0,   0, 255}, /* 0  black        */
    {  0,   0, 170, 255}, /* 1  blue         */
    {  0, 170,   0, 255}, /* 2  green        */
    {  0, 170, 170, 255}, /* 3  cyan         */
    {170,   0,   0, 255}, /* 4  red          */
    {170,   0, 170, 255}, /* 5  magenta      */
    {170, 170,   0, 255}, /* 6  brown        */
    {170, 170, 170, 255}, /* 7  light grey   */
    { 85,  85,  85, 255}, /* 8  dark grey    */
    { 85,  85, 255, 255}, /* 9  light blue   */
    { 85, 255,  85, 255}, /* 10 light green  */
    { 85, 255, 255, 255}, /* 11 light cyan   */
    {255,  85,  85, 255}, /* 12 light red    */
    {255,  85, 255, 255}, /* 13 light magenta*/
    {255, 255,  85, 255}, /* 14 yellow       */
    {255, 255, 255, 255}, /* 15 white        */
};

/* CGA 4-colour palette sets (mode 1) */
static const int cga4_pal[2][2][4] = {
    /* palette 0 */ { {0,2,4,6}, {0,10,12,14} },
    /* palette 1 */ { {0,3,5,7}, {0,11,13,15} },
};

/* ================================================================
 * Global SDL state
 * ================================================================ */
static SDL_Window   *g_win    = NULL;
static SDL_Renderer *g_ren    = NULL;
static SDL_Texture  *g_tex    = NULL;   /* pixel framebuffer texture */

/* Logical pixel buffer (width × height, 8bpp palette index) */
static uint8_t      *g_pixels = NULL;
static int           g_tex_w  = 0;
static int           g_tex_h  = 0;

/* Palette remapping (allow BASIC PALETTE command) */
static SDL_Color g_palette[16];

/* Current screen mode (0–9) */
static int g_screen = 0;

/* Text cursor */
static int g_cur_row = 0;   /* 0-based row */
static int g_cur_col = 0;   /* 0-based col */

/* Current fg/bg colour indices */
static int g_fg = 7;
static int g_bg = 0;

/* Graphics pen position (for DRAW) */
static int g_pen_x = 0;
static int g_pen_y = 0;
static int g_pen_color = 1;

/* INKEY$ ring buffer */
#define KEY_BUF_SZ 32
static char g_keybuf[KEY_BUF_SZ];
static int  g_key_head = 0, g_key_tail = 0;

/* Width setting (text) */
static int g_text_cols = 80;

/* ================================================================
 * Forward declarations
 * ================================================================ */
static void sdl_flush(void);
static void sdl_put_pixel(int x, int y, int colour);
static void sdl_draw_char(int row, int col, unsigned char ch, int fg, int bg);
static void recreate_texture(void);

/* ================================================================
 * Pixel helpers
 * ================================================================ */
static inline void sdl_put_pixel(int x, int y, int colour)
{
    const ScreenMode *m = &screen_modes[g_screen];
    if (x < 0 || x >= m->gfx_w || y < 0 || y >= m->gfx_h) return;
    g_pixels[y * m->gfx_w + x] = (uint8_t)(colour & 0xFF);
}

static inline int sdl_get_pixel(int x, int y)
{
    const ScreenMode *m = &screen_modes[g_screen];
    if (x < 0 || x >= m->gfx_w || y < 0 || y >= m->gfx_h) return 0;
    return g_pixels[y * m->gfx_w + x];
}

/* ================================================================
 * Texture / window creation
 * ================================================================ */
static void recreate_texture(void)
{
    const ScreenMode *m = &screen_modes[g_screen];
    g_tex_w = m->gfx_w;
    g_tex_h = m->gfx_h;

    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; }

    /* Palette texture: 8bpp indexed rendered to ARGB8888 */
    g_tex = SDL_CreateTexture(g_ren,
                              SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              g_tex_w, g_tex_h);
    if (!g_tex) {
        basic_stderr("display_sdl: SDL_CreateTexture: %s\n", SDL_GetError());
        return;
    }

    free(g_pixels);
    g_pixels = calloc(g_tex_w * g_tex_h, 1);
}

/* ================================================================
 * Flush pixel buffer → screen
 * ================================================================ */
static void sdl_flush(void)
{
    if (!g_tex || !g_pixels) return;

    /* Convert 8bpp palette → ARGB8888 */
    uint32_t *px32;
    int pitch;
    if (SDL_LockTexture(g_tex, NULL, (void**)&px32, &pitch) != 0) return;

    int n = g_tex_w * g_tex_h;
    for (int i = 0; i < n; i++) {
        SDL_Color c = g_palette[g_pixels[i] & 15];
        px32[i] = (0xFF000000u)
                | ((uint32_t)c.r << 16)
                | ((uint32_t)c.g <<  8)
                | ((uint32_t)c.b);
    }
    SDL_UnlockTexture(g_tex);

    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

/* ================================================================
 * Text character drawing (works in both text and gfx modes)
 * ================================================================ */
static void sdl_draw_char(int row, int col, unsigned char ch, int fg, int bg)
{
    const ScreenMode *m = &screen_modes[g_screen];
    int cw = m->gfx_w  / m->text_cols;
    int ch8= m->gfx_h  / m->text_rows;
    int px = col * cw;
    int py = row * ch8;

    for (int y = 0; y < ch8; y++) {
        uint8_t bits = font8x8[ch][y < 8 ? y : 7];
        for (int x = 0; x < cw; x++) {
            /* scale x into font bit index */
            int bit = 7 - (x * 8 / cw);
            int colour = (bits >> bit) & 1 ? fg : bg;
            sdl_put_pixel(px + x, py + y, colour);
        }
    }
}

/* ================================================================
 * Event pump — feed keyboard events into ring buffer
 * ================================================================ */
static void pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        if (e.type == SDL_KEYDOWN) {
            /* Map key to ASCII for INKEY$ */
            SDL_Keycode sym = e.key.keysym.sym;
            char c = 0;
            if (sym >= 32 && sym < 127) c = (char)sym;
            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) c = '\r';
            if (sym == SDLK_BACKSPACE) c = '\b';
            if (sym == SDLK_ESCAPE)   c = 27;
            if (sym == SDLK_UP)    c = 'H'; /* simplistic arrow mapping */
            if (sym == SDLK_DOWN)  c = 'P';
            if (sym == SDLK_LEFT)  c = 'K';
            if (sym == SDLK_RIGHT) c = 'M';
            /* Apply shift for letters */
            if (e.key.keysym.mod & KMOD_SHIFT) {
                if (c >= 'a' && c <= 'z') c -= 32;
            }
            if (c) {
                int next = (g_key_tail + 1) % KEY_BUF_SZ;
                if (next != g_key_head) {
                    g_keybuf[g_key_tail] = c;
                    g_key_tail = next;
                }
            }
        }
    }
}

/* ================================================================
 * Public API — display.h
 * ================================================================ */

void display_init(void)
{
    font_fill_upper();
    memcpy(g_palette, cga16, sizeof(cga16));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        basic_stderr("display_sdl: SDL_Init: %s\n", SDL_GetError());
        return;
    }

    const ScreenMode *m = &screen_modes[0];
    /* Scale window up 2× for readability */
    g_win = SDL_CreateWindow("WOPR BASIC",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             m->gfx_w * 2, m->gfx_h * 2,
                             SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_win) { basic_stderr("display_sdl: SDL_CreateWindow: %s\n", SDL_GetError()); return; }

    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);

    SDL_RenderSetLogicalSize(g_ren, m->gfx_w, m->gfx_h);

    recreate_texture();
    display_cls();
    sound_init();
}

void display_shutdown(void)
{
    sound_shutdown();
    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; }
    if (g_ren) { SDL_DestroyRenderer(g_ren); g_ren = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
    free(g_pixels); g_pixels = NULL;
    SDL_Quit();
}

void display_cls(void)
{
    const ScreenMode *m = &screen_modes[g_screen];
    memset(g_pixels, (uint8_t)g_bg, m->gfx_w * m->gfx_h);
    g_cur_row = 0; g_cur_col = 0;
    sdl_flush();
}

void display_locate(int row, int col)
{
    const ScreenMode *m = &screen_modes[g_screen];
    g_cur_row = (row - 1 < 0) ? 0 : (row - 1 >= m->text_rows ? m->text_rows - 1 : row - 1);
    g_cur_col = (col - 1 < 0) ? 0 : (col - 1 >= m->text_cols ? m->text_cols - 1 : col - 1);
}

void display_color(int fg, int bg)
{
    if (fg < 0 || fg > 15) fg = 7;
    if (bg < 0 || bg > 15) bg = 0;
    g_fg = fg; g_bg = bg;
}

void display_width(int cols)
{
    g_text_cols = cols;
    /* Resize is cosmetic only — we keep the fixed framebuffer resolution */
}

void display_print(const char *s)
{
    const ScreenMode *m = &screen_modes[g_screen];
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n') {
            g_cur_col = 0; g_cur_row++;
            if (g_cur_row >= m->text_rows) {
                /* Scroll up one row */
                int cw = m->gfx_w / m->text_cols;
                int rh = m->gfx_h / m->text_rows;
                memmove(g_pixels, g_pixels + m->gfx_w * rh,
                        m->gfx_w * (m->text_rows - 1) * rh);
                memset(g_pixels + m->gfx_w * (m->text_rows - 1) * rh,
                       (uint8_t)g_bg, m->gfx_w * rh);
                (void)cw;
                g_cur_row = m->text_rows - 1;
            }
        } else if (ch == '\r') {
            g_cur_col = 0;
        } else if (ch == '\b') {
            if (g_cur_col > 0) g_cur_col--;
        } else {
            sdl_draw_char(g_cur_row, g_cur_col, ch, g_fg, g_bg);
            g_cur_col++;
            if (g_cur_col >= m->text_cols) {
                g_cur_col = 0; g_cur_row++;
                if (g_cur_row >= m->text_rows) {
                    int rh = m->gfx_h / m->text_rows;
                    memmove(g_pixels, g_pixels + m->gfx_w * rh,
                            m->gfx_w * (m->text_rows - 1) * rh);
                    memset(g_pixels + m->gfx_w * (m->text_rows - 1) * rh,
                           (uint8_t)g_bg, m->gfx_w * rh);
                    g_cur_row = m->text_rows - 1;
                }
            }
        }
    }
    pump_events();
    sdl_flush();
}

void display_putchar(int c)
{
    char s[2] = { (char)c, 0 };
    display_print(s);
}

void display_newline(void)
{
    display_print("\n");
}

void display_cursor(int visible)
{
    /* Draw/erase a simple underscore cursor */
    const ScreenMode *m = &screen_modes[g_screen];
    if (g_cur_row >= m->text_rows || g_cur_col >= m->text_cols) return;
    int cw = m->gfx_w / m->text_cols;
    int rh = m->gfx_h / m->text_rows;
    int px = g_cur_col * cw;
    int py = g_cur_row * rh + rh - 2;
    int colour = visible ? g_fg : g_bg;
    for (int x = 0; x < cw; x++) sdl_put_pixel(px + x, py, colour);
    sdl_flush();
    (void)visible;
}

void display_spc(int n)
{
    for (int i = 0; i < n; i++) display_putchar(' ');
}

int display_get_width(void)
{
    return screen_modes[g_screen].text_cols;
}

/* ================================================================
 * Keyboard input
 * ================================================================ */
int display_inkey(void)
{
    pump_events();
    if (g_key_head == g_key_tail) return 0;
    char c = g_keybuf[g_key_head];
    g_key_head = (g_key_head + 1) % KEY_BUF_SZ;
    return (unsigned char)c;
}

int display_getchar(void)
{
    while (1) {
        pump_events();
        if (g_key_head != g_key_tail) {
            char c = g_keybuf[g_key_head];
            g_key_head = (g_key_head + 1) % KEY_BUF_SZ;
            return (unsigned char)c;
        }
        SDL_Delay(10);
    }
}

int display_getline(char *buf, int bufsz)
{
    int len = 0;
    buf[0] = '\0';
    /* Echo prompt cursor */
    display_cursor(1);
    for (;;) {
        pump_events();
        if (g_key_head == g_key_tail) { SDL_Delay(10); continue; }
        char c = g_keybuf[g_key_head];
        g_key_head = (g_key_head + 1) % KEY_BUF_SZ;

        if (c == '\r' || c == '\n') {
            display_newline();
            break;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                /* Erase character on screen */
                if (g_cur_col > 0) {
                    g_cur_col--;
                    sdl_draw_char(g_cur_row, g_cur_col, ' ', g_fg, g_bg);
                    sdl_flush();
                }
            }
        } else if (c >= 32 && len < bufsz - 1) {
            buf[len++] = c;
            buf[len]   = '\0';
            display_putchar(c);
        }
    }
    return len;
}

/* ================================================================
 * Graphics mode switching — called by cmd_screen
 * ================================================================ */
void display_set_screen(int mode)
{
    if (mode < 0 || mode > 28) mode = 0;
    g_screen = mode;
    memcpy(g_palette, cga16, sizeof(cga16));

    const ScreenMode *m = &screen_modes[mode];

    if (g_win) {
        SDL_SetWindowSize(g_win, m->gfx_w * 2, m->gfx_h * 2);
        SDL_RenderSetLogicalSize(g_ren, m->gfx_w, m->gfx_h);
    }
    recreate_texture();
    g_fg = (mode == 0) ? 7 : (m->colours - 1);
    g_bg = 0;
    g_cur_row = g_cur_col = 0;
    g_pen_x = 0; g_pen_y = 0; g_pen_color = 1;
    display_cls();
}

int display_get_screen(void) { return g_screen; }

/* ================================================================
 * Graphics primitives
 * ================================================================ */

void display_pset(int x, int y, int colour)
{
    if (colour < 0) colour = g_pen_color;
    sdl_put_pixel(x, y, colour);
    g_pen_x = x; g_pen_y = y;
    sdl_flush();
}

int display_point(int x, int y)
{
    return sdl_get_pixel(x, y);
}

/* Bresenham line */
void display_line(int x1, int y1, int x2, int y2, int colour, int style)
{
    if (colour < 0) colour = g_pen_color;
    /* style: 0=normal line, 1=box outline, 2=filled box */
    if (style == 1) {
        /* BOX */
        display_line(x1,y1,x2,y1,colour,0);
        display_line(x2,y1,x2,y2,colour,0);
        display_line(x2,y2,x1,y2,colour,0);
        display_line(x1,y2,x1,y1,colour,0);
        sdl_flush();
        return;
    }
    if (style == 2) {
        /* BF — filled box */
        if (x1 > x2) { int t=x1;x1=x2;x2=t; }
        if (y1 > y2) { int t=y1;y1=y2;y2=t; }
        for (int y=y1; y<=y2; y++)
            for (int x=x1; x<=x2; x++)
                sdl_put_pixel(x,y,colour);
        sdl_flush();
        return;
    }
    /* Plain line — Bresenham */
    int dx = abs(x2-x1), dy = abs(y2-y1);
    int sx = x1<x2?1:-1, sy = y1<y2?1:-1;
    int err = dx-dy;
    for (;;) {
        sdl_put_pixel(x1, y1, colour);
        if (x1==x2 && y1==y2) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 <  dx) { err += dx; y1 += sy; }
    }
    g_pen_x = x2; g_pen_y = y2;
    sdl_flush();
}

/* Midpoint ellipse (used for CIRCLE — radii rx,ry) */
#define ELLIPSE_PUT4(cx,cy,dx,dy,col) do { \
    sdl_put_pixel((cx)+(dx), (cy)+(dy), (col)); \
    sdl_put_pixel((cx)-(dx), (cy)+(dy), (col)); \
    sdl_put_pixel((cx)+(dx), (cy)-(dy), (col)); \
    sdl_put_pixel((cx)-(dx), (cy)-(dy), (col)); \
} while(0)

static void draw_ellipse(int cx, int cy, int rx, int ry, int colour)
{
    if (rx <= 0 || ry <= 0) return;
    int x = 0, y = ry;
    long rx2 = (long)rx*rx, ry2 = (long)ry*ry;
    long p = ry2 - rx2*ry + rx2/4;

    /* Region 1 */
    while (2*ry2*x < 2*rx2*y) {
        ELLIPSE_PUT4(cx, cy, x, y, colour);
        if (p < 0) p += 2*ry2*(2*x+3);
        else { p += 2*ry2*(2*x+3) - 4*rx2*(y-1); y--; }
        x++;
    }
    /* Region 2 */
    p = (long)(ry2*(x+0.5)*(x+0.5)) + (long)(rx2*(y-1)*(y-1)) - (long)(rx2*ry2);
    while (y >= 0) {
        ELLIPSE_PUT4(cx, cy, x, y, colour);
        if (p > 0) p += -2*rx2*(2*y-3);
        else { p += 2*ry2*(2*x+2) - 2*rx2*(2*y-3); x++; }
        y--;
    }
}
#undef ELLIPSE_PUT4

void display_circle(int cx, int cy, int r, int colour, double aspect,
                    double start_angle, double end_angle, int fill)
{
    if (colour < 0) colour = g_pen_color;
    const ScreenMode *m = &screen_modes[g_screen];
    /* Correct for non-square pixels */
    double pixel_aspect = (double)m->gfx_h / m->gfx_w * ((double)m->text_cols / m->text_rows);
    if (aspect <= 0) aspect = pixel_aspect;

    int rx = r;
    int ry = (int)(r * aspect + 0.5);

    if (start_angle < 0 && end_angle < 0) {
        /* Full ellipse */
        draw_ellipse(cx, cy, rx, ry, colour);
    } else {
        /* Arc / pie slice — draw by sampling angle */
        if (start_angle < 0) start_angle = 0;
        if (end_angle   < 0) end_angle   = 2*M_PI;
        double step = 0.01;
        double a = start_angle;
        while (a <= end_angle + step) {
            double ang = (a > end_angle) ? end_angle : a;
            int x = cx + (int)(rx * cos(ang));
            int y = cy - (int)(ry * sin(ang));
            sdl_put_pixel(x, y, colour);
            a += step;
        }
        /* Draw radii for pie slice */
        if (start_angle != end_angle) {
            int xs = cx + (int)(rx * cos(start_angle));
            int ys = cy - (int)(ry * sin(start_angle));
            int xe = cx + (int)(rx * cos(end_angle));
            int ye = cy - (int)(ry * sin(end_angle));
            display_line(cx, cy, xs, ys, colour, 0);
            display_line(cx, cy, xe, ye, colour, 0);
        }
    }

    if (fill) {
        /* Simple scanline flood fill from centre */
        display_paint(cx, cy, colour, colour);
    }
    sdl_flush();
}

/* Flood fill (4-connected) */
void display_paint(int x, int y, int paint_colour, int border_colour)
{
    const ScreenMode *m = &screen_modes[g_screen];
    int target = sdl_get_pixel(x, y);
    if (target == paint_colour) return;

    /* Stack-based flood fill to avoid recursion overflow */
    typedef struct { short x; short y; } Pt;
    int cap = m->gfx_w * m->gfx_h;
    Pt *stack = malloc(cap * sizeof(Pt));
    if (!stack) return;
    int top = 0;
    stack[top++] = (Pt){(short)x, (short)y};

    while (top > 0) {
        Pt p = stack[--top];
        int cx = p.x, cy = p.y;
        if (cx < 0 || cx >= m->gfx_w || cy < 0 || cy >= m->gfx_h) continue;
        int cur = sdl_get_pixel(cx, cy);
        if (cur == paint_colour || cur == border_colour) continue;
        sdl_put_pixel(cx, cy, paint_colour);
        if (top + 4 < cap) {
            stack[top++] = (Pt){(short)(cx+1),(short)cy};
            stack[top++] = (Pt){(short)(cx-1),(short)cy};
            stack[top++] = (Pt){(short)cx,(short)(cy+1)};
            stack[top++] = (Pt){(short)cx,(short)(cy-1)};
        }
    }
    free(stack);
    sdl_flush();
}

/* ================================================================
 * DRAW command (GW-BASIC DRAW string)
 * ================================================================ */
void display_draw(const char *s)
{
    const ScreenMode *m = &screen_modes[g_screen];
    int x = g_pen_x, y = g_pen_y;
    int colour = g_pen_color;
    int scale = 1;

    const char *p = s;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char cmd = (char)toupper((unsigned char)*p++);

        /* optional angle/number argument */
        int n = 0;
        int has_n = 0;
        if (isdigit((unsigned char)*p) || *p == '-') {
            has_n = 1;
            n = (int)strtol(p, (char**)&p, 10);
        }
        int d = (has_n ? n : 1) * scale;

        int nx = x, ny = y;
        int plot = 1;

        switch (cmd) {
        case 'U': ny = y - d; break;
        case 'D': ny = y + d; break;
        case 'L': nx = x - d; break;
        case 'R': nx = x + d; break;
        case 'E': nx = x + d; ny = y - d; break;
        case 'F': nx = x + d; ny = y + d; break;
        case 'G': nx = x - d; ny = y + d; break;
        case 'H': nx = x - d; ny = y - d; break;
        case 'M': {
            /* M [±]x,y */
            int rel = 0;
            if (*p == '+' || *p == '-') { rel = (*p == '-') ? -1 : 1; p++; }
            int mx = (int)strtol(p, (char**)&p, 10);
            if (*p == ',') p++;
            int my = (int)strtol(p, (char**)&p, 10);
            if (rel) { nx = x + mx * rel; ny = y + my * rel; }
            else     { nx = mx; ny = my; }
            break;
        }
        case 'B': plot = 0; /* move without drawing, next command */
            /* re-parse next move */
            while (isspace((unsigned char)*p)) p++;
            if (*p) {
                cmd = (char)toupper((unsigned char)*p++);
                n = isdigit((unsigned char)*p) ? (int)strtol(p,(char**)&p,10) : 1;
                d = n * scale;
                switch (cmd) {
                case 'U': ny=y-d; break; case 'D': ny=y+d; break;
                case 'L': nx=x-d; break; case 'R': nx=x+d; break;
                case 'E': nx=x+d; ny=y-d; break;
                case 'F': nx=x+d; ny=y+d; break;
                case 'G': nx=x-d; ny=y+d; break;
                case 'H': nx=x-d; ny=y-d; break;
                default: break;
                }
            }
            break;
        case 'N': /* draw and return */ {
            while (isspace((unsigned char)*p)) p++;
            if (*p) {
                char c2 = (char)toupper((unsigned char)*p++);
                int n2 = isdigit((unsigned char)*p) ? (int)strtol(p,(char**)&p,10) : 1;
                int d2 = n2 * scale;
                int rx2=x, ry2=y;
                switch(c2){
                case 'U':ry2=y-d2;break;case 'D':ry2=y+d2;break;
                case 'L':rx2=x-d2;break;case 'R':rx2=x+d2;break;
                case 'E':rx2=x+d2;ry2=y-d2;break;
                case 'F':rx2=x+d2;ry2=y+d2;break;
                case 'G':rx2=x-d2;ry2=y+d2;break;
                case 'H':rx2=x-d2;ry2=y-d2;break;
                default:break;
                }
                /* draw and return to x,y */
                display_line(x, y, rx2, ry2, colour, 0);
            }
            continue; /* don't update x,y */
        }
        case 'C': colour = n & 15; g_pen_color = colour; continue;
        case 'S': scale  = (n < 1) ? 1 : n; continue;
        case 'P': { /* PAINT interior,border */
            int paint_c = n & 15;
            int border_c = paint_c;
            if (*p == ',') { p++; border_c = (int)strtol(p,(char**)&p,10) & 15; }
            display_paint(x, y, paint_c, border_c);
            continue;
        }
        case 'A': { /* Angle: 0=right,1=up,2=left,3=down */
            /* Stub: angle support requires rotation matrix, skip for now */
            continue;
        }
        case 'T': { /* Turn: TA angle in degrees */
            if (*p == 'A') p++;
            continue;
        }
        case 'X': { /* Execute substring */
            continue;
        }
        default: continue;
        }

        if (plot)
            display_line(x, y, nx, ny, colour, 0);
        x = nx; y = ny;
        if (x < 0) x = 0; if (x >= m->gfx_w) x = m->gfx_w - 1;
        if (y < 0) y = 0; if (y >= m->gfx_h) y = m->gfx_h - 1;
    }

    g_pen_x = x; g_pen_y = y;
    sdl_flush();
}

/* Set palette entry */
void display_palette(int index, int r, int g, int b)
{
    if (index < 0 || index > 15) return;
    g_palette[index].r = (uint8_t)r;
    g_palette[index].g = (uint8_t)g;
    g_palette[index].b = (uint8_t)b;
    g_palette[index].a = 255;
    sdl_flush();
}

/* Get/set pen position (for DRAW, LINE with step) */
void display_get_pen(int *x, int *y) { *x = g_pen_x; *y = g_pen_y; }
void display_set_pen(int x, int y, int colour) {
    g_pen_x = x; g_pen_y = y;
    if (colour >= 0) g_pen_color = colour;
}

#endif /* HAVE_SDL */
