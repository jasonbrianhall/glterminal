#pragma once

// ============================================================================
// CRT AUDIO  — synthesized sound effects for CRT render mode
//
// Call sites:
//   gl_terminal_main.cpp (or wherever render mode toggles):
//     crt_audio_set_mode(g_render_mode == RENDER_MODE_CRT);
//
//   term_render() bottom, each frame:
//     crt_audio_set_activity(dirty_cells / (float)(t->cols * t->rows));
//
//   handle_key(), cursor-movement keys:
//     crt_audio_cursor_ping();
// ============================================================================

void crt_audio_init(void);
void crt_audio_shutdown(void);

// Call whenever CRT mode is toggled. Fires the power-on thunk on rising edge.
void crt_audio_set_mode(bool crt_on);

// 0..1 glyph activity level — drives scanline buzz volume. Call each frame.
void crt_audio_set_activity(float level);

// Fire a short beam-sweep ping (cursor movement / keypress).
void crt_audio_cursor_ping(void);
