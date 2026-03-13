#pragma once
#include "gl_renderer.h"   // RENDER_MODE_* constants

// ============================================================================
// TERMINAL AUDIO — per-render-mode sound effects
//
// Call sites:
//   main(), after SDL_Init:
//     term_audio_init();
//
//   main(), on shutdown:
//     term_audio_shutdown();
//
//   main(), whenever g_render_mode changes (pass new value):
//     term_audio_set_mode(g_render_mode);
//
//   term_render(), once per frame (pass glyph fill ratio 0..1):
//     term_audio_set_activity(dirty_cells / (float)(cols * rows));
//
//   handle_key(), on arrow / Home / End:
//     term_audio_cursor_ping();
// ============================================================================

void term_audio_init(void);
void term_audio_shutdown(void);
void term_audio_set_mode(int render_mode);
void term_audio_set_activity(float level);   // 0..1, drives CRT buzz
void term_audio_cursor_ping(void);           // CRT mode only

// Legacy aliases so existing call sites still compile without changes
inline void crt_audio_init(void)            { term_audio_init(); }
inline void crt_audio_shutdown(void)        { term_audio_shutdown(); }
inline void crt_audio_set_activity(float l) { term_audio_set_activity(l); }
inline void crt_audio_cursor_ping(void)     { term_audio_cursor_ping(); }
