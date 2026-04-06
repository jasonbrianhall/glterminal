#ifndef DISPLAY_H
#define DISPLAY_H

/*
 * display.h — Terminal/graphics display abstraction for the BASIC interpreter.
 *
 * Screen 0   : text mode (80×25 or 40×25) — ANSI terminal or SDL window
 * Screens 1-9: SDL2 pixel-graphics modes matching GW-BASIC / QBASIC
 *
 * Replace display_ansi.c with display_sdl.c (compiled when HAVE_SDL is set)
 * to get both text and full graphics support.
 */

/* ================================================================
 * Init / shutdown
 * ================================================================ */
void display_init(void);
void display_shutdown(void);

/* ================================================================
 * SCREEN mode — 0=text, 1-9=graphics (SDL only; ANSI stubs no-op)
 * ================================================================ */
void display_set_screen(int mode);
int  display_get_screen(void);

/* ================================================================
 * Text-mode operations (work in all screen modes)
 * ================================================================ */

/* CLS — clear screen / framebuffer */
void display_cls(void);

/* LOCATE row, col  (1-based) */
void display_locate(int row, int col);

/* COLOR fg, bg  (CGA colour indices 0-15) */
void display_color(int fg, int bg);

/* WIDTH cols */
void display_width(int cols);

/* Print a raw string (interprets \n, \r, \b) */
void display_print(const char *s);

/* Print a single character */
void display_putchar(int c);

/* Newline */
void display_newline(void);

/* Hide / show cursor */
void display_cursor(int visible);

/* SPC(n) — print n spaces */
void display_spc(int n);

/* Get current terminal / mode width in text columns */
int display_get_width(void);

/* ================================================================
 * Keyboard input
 * ================================================================ */

/* INKEY$ — non-blocking: returns 0 if no key waiting */
int display_inkey(void);

/* Blocking single-character read */
int display_getchar(void);

/* Blocking line read — fills buf (without newline), returns length */
int display_getline(char *buf, int bufsz);

/* ================================================================
 * Graphics primitives (SCREEN 1-9, SDL only; ANSI stubs no-op)
 * ================================================================ */

/* PSET (x,y), colour  — colour<0 → use current pen colour */
void display_pset(int x, int y, int colour);

/* POINT(x,y) — return palette index of pixel at (x,y) */
int  display_point(int x, int y);

/* LINE (x1,y1)-(x2,y2), colour [,B|BF]
 *   style: 0=line, 1=box outline (B), 2=filled box (BF) */
void display_line(int x1, int y1, int x2, int y2, int colour, int style);

/* CIRCLE (cx,cy), r [,colour [,start [,end [,aspect]]]]
 *   start/end: arc angles in radians; <0 = full ellipse
 *   fill: non-zero = flood-fill interior (used for filled circle) */
void display_circle(int cx, int cy, int r, int colour,
                    double aspect, double start_angle, double end_angle,
                    int fill);

/* PAINT (x,y), paint_colour, border_colour — flood fill */
void display_paint(int x, int y, int paint_colour, int border_colour);

/* DRAW "string" — GW-BASIC DRAW macro language */
void display_draw(const char *s);

/* PALETTE index, r, g, b — replace a palette entry (0-15) */
void display_palette(int index, int r, int g, int b);

/* Pen position (for LINE STEP, DRAW, etc.) */
void display_get_pen(int *x, int *y);
void display_set_pen(int x, int y, int colour);   /* colour<0 = keep current */

/* GET (x1,y1)-(x2,y2), buf — capture screen rect; returns pixel count.
 * buf must hold at least (x2-x1+1)*(y2-y1+1) ints (palette indices). */
int  display_get_rect(int x1, int y1, int x2, int y2, int *buf);

/* PUT (x,y), buf, w, h, mode — blit pixel buffer captured by display_get_rect.
 * mode: 0=PSET, 1=XOR, 2=OR, 3=AND, 4=PRESET */
void display_put_rect(int x, int y, int w, int h, const int *buf, int mode);

#endif /* DISPLAY_H */
