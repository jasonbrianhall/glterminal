/*
 * chess_sound.cpp - Sound effects implementation
 * Generates procedural tones for chess moves since we can't embed audio files
 */

#include "chess_sound.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 44100
#define MAX_SOUND_LENGTH (SAMPLE_RATE * 2)  // 2 seconds max

static Mix_Chunk *sounds[SFX_COUNT] = {0};
static bool sound_enabled = false;

// Generate a simple sine wave tone at frequency for duration ms
static Mix_Chunk *generate_tone(float frequency, int duration_ms, float amplitude) {
    int num_samples = (SAMPLE_RATE * duration_ms) / 1000;
    Uint8 *data = (Uint8 *)malloc(num_samples * 2);
    if (!data) return NULL;

    Sint16 *samples = (Sint16 *)data;
    float period = SAMPLE_RATE / frequency;
    
    for (int i = 0; i < num_samples; i++) {
        float phase = (i / period) * 2.0f * 3.14159f;
        float sample = sin(phase) * amplitude * 32767.0f;
        samples[i] = (Sint16)sample;
    }

    Mix_Chunk *chunk = (Mix_Chunk *)malloc(sizeof(Mix_Chunk));
    if (!chunk) { free(data); return NULL; }
    
    chunk->allocated = 1;
    chunk->abuf = data;
    chunk->alen = num_samples * 2;
    chunk->volume = MIX_MAX_VOLUME;
    return chunk;
}

// Generate a sequence of tones (melody)
static Mix_Chunk *generate_melody(const float *frequencies, const int *durations, int count, float amplitude) {
    int total_samples = 0;
    for (int i = 0; i < count; i++) {
        total_samples += (SAMPLE_RATE * durations[i]) / 1000;
    }
    
    if (total_samples > MAX_SOUND_LENGTH) total_samples = MAX_SOUND_LENGTH;
    
    Uint8 *data = (Uint8 *)malloc(total_samples * 2);
    if (!data) return NULL;

    Sint16 *samples = (Sint16 *)data;
    int sample_idx = 0;

    for (int note = 0; note < count && sample_idx < total_samples; note++) {
        float frequency = frequencies[note];
        int duration = durations[note];
        int num_samples = (SAMPLE_RATE * duration) / 1000;
        
        if (sample_idx + num_samples > total_samples)
            num_samples = total_samples - sample_idx;

        float period = SAMPLE_RATE / frequency;
        for (int i = 0; i < num_samples; i++) {
            float phase = ((sample_idx + i) / period) * 2.0f * 3.14159f;
            float sample_val = sin(phase) * amplitude * 32767.0f;
            samples[sample_idx + i] = (Sint16)sample_val;
        }
        sample_idx += num_samples;
    }

    Mix_Chunk *chunk = (Mix_Chunk *)malloc(sizeof(Mix_Chunk));
    if (!chunk) { free(data); return NULL; }
    
    chunk->allocated = 1;
    chunk->abuf = data;
    chunk->alen = total_samples * 2;
    chunk->volume = MIX_MAX_VOLUME;
    return chunk;
}

void chess_sound_init(void) {
    if (Mix_OpenAudio(SAMPLE_RATE, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        fprintf(stderr, "Failed to init audio: %s\n", SDL_GetError());
        return;
    }

    // MOVE: Simple beep (C4 for 150ms)
    sounds[SFX_MOVE] = generate_tone(261.63f, 150, 0.7f);

    // CAPTURE: Two beeps (E4 then G4, 100ms each)
    float cap_freqs[] = {329.63f, 392.0f};
    int cap_durs[] = {100, 100};
    sounds[SFX_CAPTURE] = generate_melody(cap_freqs, cap_durs, 2, 0.75f);

    // CHECK: Rising arpeggio (C4, E4, G4, 80ms each)
    float check_freqs[] = {261.63f, 329.63f, 392.0f};
    int check_durs[] = {80, 80, 80};
    sounds[SFX_CHECK] = generate_melody(check_freqs, check_durs, 3, 0.8f);

    // CHECKMATE: Dramatic descending pattern (G5, D5, G4, 150ms each) then low C
    float mate_freqs[] = {783.99f, 587.33f, 392.0f, 261.63f};
    int mate_durs[] = {150, 150, 150, 300};
    sounds[SFX_CHECKMATE] = generate_melody(mate_freqs, mate_durs, 4, 0.85f);

    // RESIGN/TAUNT: Mocking "wah-wah-wah" (G4 down to C4 with dips)
    float resign_freqs[] = {392.0f, 349.23f, 329.63f, 293.66f, 261.63f};
    int resign_durs[] = {120, 120, 120, 120, 200};
    sounds[SFX_RESIGN] = generate_melody(resign_freqs, resign_durs, 5, 0.8f);

    sound_enabled = true;
}

void chess_sound_shutdown(void) {
    for (int i = 0; i < SFX_COUNT; i++) {
        if (sounds[i]) {
            free(sounds[i]->abuf);
            free(sounds[i]);
            sounds[i] = NULL;
        }
    }
    Mix_CloseAudio();
    sound_enabled = false;
}

void chess_sound_play(ChessSfxType type) {
    if (!sound_enabled || type < 0 || type >= SFX_COUNT || !sounds[type]) {
        return;
    }
    Mix_PlayChannel(-1, sounds[type], 0);
}
