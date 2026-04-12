#pragma once
// ============================================================================
// sdl_renderer.h
//
// Declares the runtime flag and SDL entry points called by the dispatch stubs
// in gl_renderer.cpp.  Include this only from gl_renderer.cpp and
// gl_terminal_main.cpp.
// ============================================================================

#include <SDL2/SDL.h>

// Set to true by gl_terminal_main when GL 3.3 context creation fails,
// BEFORE calling gl_init_renderer().
extern bool g_use_sdl_renderer;

// SDL renderer handle — populated by gl_terminal_main in the SDL path,
// used by sdl_renderer.cpp internally.
extern SDL_Renderer *g_sdl_renderer;
extern SDL_Texture  *s_basic_tex;       // persistent BASIC graphics layer (SDL path)
extern bool          s_basic_has_content;

// ── SDL-path implementations (defined in sdl_renderer.cpp) ──────────────────
void sdl_init_renderer   (int w, int h);
void sdl_resize_fbo      (int w, int h);
void sdl_flush_verts     (void);
void sdl_draw_verts      (Vertex *v, int n);
void sdl_flush_glyphs    (void);
void sdl_draw_glyph_verts(GlyphVertex *v, int n);
void sdl_begin_term_frame(int win_w, int win_h, float bg_r, float bg_g, float bg_b);
void sdl_clear_term_frame(int win_w, int win_h, float bg_r, float bg_g, float bg_b);
void sdl_end_term_frame  (void);
void sdl_begin_frame     (void);
void sdl_end_frame       (float time, int win_w, int win_h);
void sdl_update_ghost    (int win_w, int win_h);
