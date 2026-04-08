#pragma once
// basic_graphics.h — BASIC graphics protocol extension for felix terminal
//
// Wire up in terminal.cpp: when an OSC sequence with code 666 is complete,
// call basic_handle_osc() with the payload after the "666;" prefix.
//
// Example in term_feed() OSC dispatch:
//   if (osc_code == 666)
//       basic_handle_osc(t, osc_payload, osc_len, win_w, win_h);

#include "terminal.h"

// Call once after gl_init_renderer() / sdl_init_renderer().
void basic_graphics_init(int win_w, int win_h);

// Call from term_render() inside gl_begin_term_frame()/gl_end_term_frame()
// to flush queued draw commands into the current GL frame.
// Returns true if anything was drawn (caller should set needs_render).
void basic_render(int win_w, int win_h);

// True when at least one command is queued and waiting to be rendered.
extern bool s_dl_dirty;

// True while a SCREEN mode > 0 is active.
// When set, term_render should skip drawing opaque cell background quads
// so BASIC graphics (composited underneath by gl_end_frame) show through.
extern bool g_basic_graphics_active;

// Handle a complete OSC 666 payload. payload points to everything after
// the "666;" prefix (i.e. the command string). win_w/win_h are the current
// terminal window dimensions in pixels.
void basic_handle_osc(Terminal *t, const char *payload, int len,
                      int win_w, int win_h);

// Free all sprites and GL/SDL resources. Call before SDL_Quit().
void basic_graphics_shutdown(void);

// ============================================================================
// BASIC-SIDE HELPER MACROS (copy into your BASIC runtime's C helper header)
// ============================================================================
//
// #define BG_ESC     "\033]666;"
// #define BG_ST      "\033\\"
//
// static inline void bg_circle(int x,int y,int r,int c)
//   { printf(BG_ESC "circle;%d;%d;%d;%d" BG_ST, x,y,r,c); fflush(stdout); }
//
// static inline void bg_line(int x1,int y1,int x2,int y2,int c)
//   { printf(BG_ESC "line;%d;%d;%d;%d;%d" BG_ST, x1,y1,x2,y2,c); fflush(stdout); }
//
// static inline void bg_line_box(int x1,int y1,int x2,int y2,int c,int filled)
//   { printf(BG_ESC "line;%d;%d;%d;%d;%d;%s" BG_ST, x1,y1,x2,y2,c,filled?"BF":"B"); fflush(stdout); }
//
// static inline void bg_pset(int x,int y,int c)
//   { printf(BG_ESC "pset;%d;%d;%d" BG_ST, x,y,c); fflush(stdout); }
//
// static inline void bg_paint(int x,int y,int c,int bc)
//   { printf(BG_ESC "paint;%d;%d;%d;%d" BG_ST, x,y,c,bc); fflush(stdout); }
//
// static inline void bg_cls(int c)
//   { printf(BG_ESC "cls;%d" BG_ST, c); fflush(stdout); }
//
// static inline void bg_screen(int mode)
//   { printf(BG_ESC "screen;%d" BG_ST, mode); fflush(stdout); }
//
// static inline void bg_palette(int idx,int r,int g,int b)
//   { printf(BG_ESC "palette;%d;%d;%d;%d" BG_ST, idx,r,g,b); fflush(stdout); }
//
// static inline void bg_play(const char *mml)
//   { printf(BG_ESC "play;%s" BG_ST, mml); fflush(stdout); }
//
// static inline void bg_get(int id,int x1,int y1,int x2,int y2)
//   { printf(BG_ESC "get;%d;%d;%d;%d;%d" BG_ST, id,x1,y1,x2,y2); fflush(stdout); }
//
// static inline void bg_put(int id,int x,int y,int xormode)
//   { printf(BG_ESC "put;%d;%d;%d;%s" BG_ST, id,x,y,xormode?"xor":"pset"); fflush(stdout); }
