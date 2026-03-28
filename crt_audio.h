#pragma once
#include <SDL2/SDL.h>
#include "gl_renderer.h"   // RENDER_MODE_* constants

// ============================================================================
// TERMINAL AUDIO — per-render-mode sound effects + fight/bouncing-circle SFX
// ============================================================================

void term_audio_init(void);
void term_audio_shutdown(void);
void term_audio_set_enabled(bool en);
bool term_audio_get_enabled(void);
void term_audio_set_mode(uint32_t render_mode);
void term_audio_set_activity(float level);   // 0..1, drives CRT buzz
void term_audio_cursor_ping(void);           // CRT mode only

// ── Fight mode events ────────────────────────────────────────────────────────
// weight: 0=jab, 1=cross/hook, 2=uppercut/kick/sweep
void fight_audio_hit(float x, float y, int weight);
void fight_audio_block(void);
void fight_audio_hadouken_launch(void);
void fight_audio_ko(void);
void fight_audio_cheer(void);

// ── Bouncing circle events ───────────────────────────────────────────────────
// radius_frac: current_radius / max_radius (0..1), used to vary pitch
void bc_audio_bounce(float radius_frac);

// Returns the SDL audio device ID (0 if not open). Used by WOPR audio.
SDL_AudioDeviceID term_audio_get_device(void);

// Mix a WOPR modem screech buffer into the audio callback stream.
// buf must remain valid until playback finishes (wopr_audio.cpp owns it).
void term_audio_wopr_play(const float *buf, int num_samples);
void term_audio_wopr_stop(void);
inline void crt_audio_init(void)            { term_audio_init(); }
inline void crt_audio_shutdown(void)        { term_audio_shutdown(); }
inline void crt_audio_set_activity(float l) { term_audio_set_activity(l); }
inline void crt_audio_cursor_ping(void)     { term_audio_cursor_ping(); }
