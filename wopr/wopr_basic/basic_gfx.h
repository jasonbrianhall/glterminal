#pragma once
/*
 * basic_gfx.h — Graphics backend API for the BASIC interpreter.
 *
 * commands.cpp calls these functions directly.  Two implementations exist:
 *
 *   basic_gfx_sdl.cpp  — SDL2 pixel-buffer renderer (USE_SDL_WINDOW)
 *   basic_gfx_osc.cpp  — Original OSC 666 escape-code emitter (default)
 *
 * All coordinates are in logical screen pixels (SCREEN-mode space).
 * Color indices are 0-15 CGA palette, overridable via gfx_palette().
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/* Set the active SCREEN mode (0 = text, 1-28 = graphics).
 * Allocates / resizes the pixel buffer and SDL texture as needed.
 * mode 0 destroys the graphics surface and returns to text mode. */
void gfx_screen(int mode);

/* ── Palette ───────────────────────────────────────────────────────────────── */

/* Override palette slot idx (0-15) with a 24-bit RGB colour.
 * Affects all subsequent draw calls and redraws existing pixels if possible. */
void gfx_palette(int idx, int r, int g, int b);

/* ── Draw primitives ───────────────────────────────────────────────────────── */

/* Fill entire graphics surface with color, or clear the text grid (mode 0). */
void gfx_cls(int color);

/* Set single pixel. */
void gfx_pset(int x, int y, int color);

/* Draw a line from (x1,y1) to (x2,y2). */
void gfx_line(int x1, int y1, int x2, int y2, int color);

/* Draw an unfilled rectangle. */
void gfx_box(int x1, int y1, int x2, int y2, int color);

/* Draw a filled rectangle. */
void gfx_boxfill(int x1, int y1, int x2, int y2, int color);

/* Draw a circle outline. */
void gfx_circle(int cx, int cy, int radius, int color);

/* Flood-fill from (x,y) with fill_color, stopping at border_color. */
void gfx_paint(int x, int y, int fill_color, int border_color);

/* ── Sprite store (GET / PUT) ──────────────────────────────────────────────── */

/* Capture rectangle to sprite slot id. */
void gfx_get(int id, int x1, int y1, int x2, int y2);

/* Blit sprite id at (x,y).  xor_mode=1 → XOR blit, 0 → PSET (opaque). */
void gfx_put(int id, int x, int y, int xor_mode);

/* ── Query ─────────────────────────────────────────────────────────────────── */

/* Returns 1 if a graphics mode is active (SCREEN > 0). */
int  gfx_active(void);

/* Returns current logical width / height of the graphics surface. */
int  gfx_width(void);
int  gfx_height(void);

#ifdef __cplusplus
}
#endif
