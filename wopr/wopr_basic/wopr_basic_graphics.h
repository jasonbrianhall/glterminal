// wopr_basic_graphics.h
// ============================================================================
// WOPR Graphics Backend for BASIC Interpreter
//
// When compiled with -DWOPR, this module provides the graphics primitives
// (SCREEN, CLS, CIRCLE, LINE, PSET, etc.) through the WOPR overlay's
// gfx_* API functions.
//
// The WOPR system uses an SDL2 pixel buffer backend (basic_gfx_sdl.cpp)
// which is exposed via basic_gfx.h. This header wraps those functions
// to provide a clean interface matching what commands.cpp expects.
// ============================================================================

#pragma once

#ifdef WOPR

#include "basic_gfx.h"
#include <cmath>

// ============================================================================
// Graphics State Management
// ============================================================================

/* Initialized by wopr_graphics_init() */
extern int g_screen_mode;
extern int g_screen_width;
extern int g_screen_height;
extern double g_gfx_x, g_gfx_y;

// ============================================================================
// WOPR Graphics API — wraps basic_gfx.h for BASIC commands
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize graphics for WOPR (called from wopr_open) */
void wopr_graphics_init(void);

/* Shutdown graphics (called from wopr_close) */
void wopr_graphics_shutdown(void);

/* Mark graphics dirty for next render pass */
void wopr_graphics_mark_dirty(void);

#ifdef __cplusplus
}
#endif

/* ========================================================================== */
/* Inline wrappers matching basic_gfx.h API                                 */
/* ========================================================================== */

static inline void wopr_gfx_screen(int mode) {
    gfx_screen(mode);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_screen_tc(int w, int h) {
    gfx_screen_tc(w, h);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_cls(int color) {
    gfx_cls(color);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_pset(int x, int y, int color) {
    gfx_pset(x, y, color);
    wopr_graphics_mark_dirty();
}

static inline int wopr_gfx_point(int x, int y) {
    return gfx_point(x, y);
}

static inline void wopr_gfx_line(int x1, int y1, int x2, int y2, int color) {
    gfx_line(x1, y1, x2, y2, color);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_box(int x1, int y1, int x2, int y2, int color) {
    gfx_box(x1, y1, x2, y2, color);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_boxfill(int x1, int y1, int x2, int y2, int color) {
    gfx_boxfill(x1, y1, x2, y2, color);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_circle(int cx, int cy, int radius, int color) {
    gfx_circle(cx, cy, radius, color);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_arc(int cx, int cy, int radius, 
                                 double start_angle, double end_angle, int color) {
    gfx_arc(cx, cy, radius, start_angle, end_angle, color);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_paint(int x, int y, int fill_color, int border_color) {
    gfx_paint(x, y, fill_color, border_color);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_palette(int idx, int r, int g, int b) {
    gfx_palette(idx, r, g, b);
    wopr_graphics_mark_dirty();
}

static inline int wopr_gfx_active(void) {
    return gfx_active();
}

static inline int wopr_gfx_width(void) {
    return gfx_width();
}

static inline int wopr_gfx_height(void) {
    return gfx_height();
}

static inline void wopr_gfx_get(int id, int x1, int y1, int x2, int y2) {
    gfx_get(id, x1, y1, x2, y2);
}

static inline void wopr_gfx_put(int id, int x, int y, int xor_mode) {
    gfx_put(id, x, y, xor_mode);
    wopr_graphics_mark_dirty();
}

static inline void wopr_gfx_put_array(const int *raw_longs, int count, 
                                       int x, int y, int xor_mode) {
    gfx_put_array(raw_longs, count, x, y, xor_mode);
    wopr_graphics_mark_dirty();
}

#endif /* WOPR */
