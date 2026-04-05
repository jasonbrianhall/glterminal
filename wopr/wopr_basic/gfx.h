#ifndef GFX_H
#define GFX_H

/*
 * gfx.h — SDL2 pixel-graphics window interface.
 *
 * This is an INTERNAL header used only by display_ansi.c and gfx_sdl.c.
 * BASIC commands and the rest of the interpreter never include this directly;
 * they go through display.h.
 *
 * When HAVE_SDL is not defined, gfx_null.c provides silent stubs so the
 * linker is always satisfied.
 *
 * Lifecycle:
 *   gfx_open(mode)   — open an SDL window for screen mode 1-9
 *   gfx_close()      — close the SDL window, return to text-only mode
 *   gfx_is_open()    — non-zero if a graphics window is currently active
 *
 * All coordinates are in logical (mode-native) pixels.
 * Colour arguments are CGA palette indices 0-15 (or -1 = current pen).
 */

/* Open/close */
void gfx_open(int mode);   /* mode 1-9 */
void gfx_close(void);
int  gfx_is_open(void);
int  gfx_get_mode(void);
int  gfx_get_cols(void);   /* text columns for current mode */

/* Palette */
void gfx_palette(int index, int r, int g, int b);

/* Primitives */
void gfx_cls(int bg_colour);
void gfx_pset(int x, int y, int colour);
int  gfx_point(int x, int y);
void gfx_line(int x1, int y1, int x2, int y2, int colour, int style);
void gfx_circle(int cx, int cy, int r, int colour,
                double aspect, double start_angle, double end_angle);
void gfx_paint(int x, int y, int paint_colour, int border_colour);
void gfx_draw(const char *s);

/* Pen position (for LINE STEP, DRAW) */
void gfx_get_pen(int *x, int *y);
void gfx_set_pen(int x, int y, int colour);  /* colour < 0 = keep current */

/* Text rendering into the graphics window */
void gfx_print_char(unsigned char ch, int fg, int bg);
void gfx_locate(int row, int col);
void gfx_color(int fg, int bg);
void gfx_cursor(int visible);
void gfx_flush(void);

/* Non-blocking key poll (mirrors display_inkey for the SDL window) */
int gfx_inkey(void);

/* Blocking reads — used when a graphics screen is active */
int  gfx_getchar(void);
int  gfx_getline(char *buf, int bufsz);

#endif /* GFX_H */
