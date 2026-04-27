/*
 * chess_sound.h - Sound effects for chess game
 * Uses SDL_mixer for audio playback
 */

#ifndef CHESS_SOUND_H
#define CHESS_SOUND_H

#include <SDL2/SDL_mixer.h>

typedef enum {
    SFX_MOVE,       // Normal piece move
    SFX_CAPTURE,    // Piece captured
    SFX_CHECK,      // King in check
    SFX_CHECKMATE,  // Checkmate
    SFX_RESIGN,     // Player resigns / WOPR taunts
    SFX_COUNT
} ChessSfxType;

// Initialize sound system. Call once at startup.
void chess_sound_init(void);

// Cleanup sound system. Call on shutdown.
void chess_sound_shutdown(void);

// Play a sound effect
void chess_sound_play(ChessSfxType type);

#endif /* CHESS_SOUND_H */
