#include "crt_audio.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <string.h>
#include <atomic>

// ============================================================================
// TUNING
// ============================================================================

#define SAMPLE_RATE     44100
#define CHANNELS        1
#define BUFFER_SAMPLES  1024

// --- Power-on thunk ----------------------------------------------------------
#define THUNK_FREQ      72.0f
#define THUNK_DURATION  0.18f
#define THUNK_VOLUME    0.22f

// --- Scanline buzz -----------------------------------------------------------
// True 15.7kHz is above most adults' hearing. Use a 3kHz cluster that
// still reads as "electrical buzz" and is actually audible.
#define BUZZ_FREQ       3000.0f
#define BUZZ_HARMONICS  3
#define BUZZ_MAX_VOL    0.015f
#define BUZZ_ATTACK     0.30f
#define BUZZ_RELEASE    0.50f

// --- Cursor ping -------------------------------------------------------------
#define PING_FREQ       1800.0f
#define PING_DURATION   0.018f
#define PING_VOLUME     0.07f
#define PING_MAX_STACK  4

// ============================================================================
// VOICE STRUCTS
// ============================================================================

struct ThunkVoice {
    float phase;
    int   samples_left;
    int   total_samples;
    float volume;
    bool  active;
};

struct BuzzVoice {
    float phase[BUZZ_HARMONICS];
    float current_vol;   // smoothed
    float target_vol;    // set from main thread
};

struct PingVoice {
    float phase;
    int   samples_left;
    int   total_samples;
    float volume;
};

// ============================================================================
// STATE  (written from main thread, read from audio callback)
// ============================================================================

static SDL_AudioDeviceID s_dev = 0;
static bool              s_crt_was_on = false;

// Thunk: written under audio lock
static ThunkVoice s_thunk;

// Buzz: target_vol written atomically, rest touched only in callback
static BuzzVoice  s_buzz;

// Pings: ring buffer, written under audio lock
static PingVoice  s_pings[PING_MAX_STACK];
static int        s_ping_write = 0;   // next slot to write (wraps)

// ============================================================================
// AUDIO CALLBACK
// ============================================================================

static void audio_callback(void * /*userdata*/, Uint8 *stream, int len) {
    int n = len / (int)sizeof(float);
    float *out = (float *)stream;
    memset(out, 0, len);

    const float dt = 1.0f / (float)SAMPLE_RATE;
    const float twopi = 6.28318530f;

    for (int i = 0; i < n; i++) {
        float sample = 0.0f;

        // ── Thunk ──────────────────────────────────────────────────────────
        if (s_thunk.active) {
            float t = 1.0f - (float)s_thunk.samples_left / (float)s_thunk.total_samples;
            // Pure exponential decay — hits hard immediately, fades over ~220ms
            float env = expf(-t * 14.0f);
            sample += sinf(s_thunk.phase) * env * s_thunk.volume;
            s_thunk.phase = fmodf(s_thunk.phase + twopi * THUNK_FREQ * dt, twopi);
            if (--s_thunk.samples_left <= 0) s_thunk.active = false;
        }

        // ── Scanline buzz ──────────────────────────────────────────────────
        {
            // Smooth target → current
            float diff = s_buzz.target_vol - s_buzz.current_vol;
            float rate = (diff > 0.f) ? (1.f / (BUZZ_ATTACK  * SAMPLE_RATE))
                                      : (1.f / (BUZZ_RELEASE * SAMPLE_RATE));
            if (fabsf(diff) < rate) s_buzz.current_vol = s_buzz.target_vol;
            else                    s_buzz.current_vol += (diff > 0.f ? rate : -rate);

            if (s_buzz.current_vol > 0.0001f) {
                float bsample = 0.0f;
                for (int h = 0; h < BUZZ_HARMONICS; h++) {
                    float hfreq = BUZZ_FREQ * (float)(h + 1);
                    // Attenuate higher harmonics to stay below Nyquist gracefully
                    float hamp = 1.0f / (float)(h + 1);
                    bsample += sinf(s_buzz.phase[h]) * hamp;
                    s_buzz.phase[h] = fmodf(s_buzz.phase[h] + twopi * hfreq * dt, twopi);
                }
                sample += bsample * s_buzz.current_vol;
            }
        }

        // ── Pings ──────────────────────────────────────────────────────────
        for (int p = 0; p < PING_MAX_STACK; p++) {
            PingVoice &pv = s_pings[p];
            if (pv.samples_left <= 0) continue;
            float t = 1.0f - (float)pv.samples_left / (float)pv.total_samples;
            // Fast attack, exponential tail
            float env = (1.0f - expf(-t * 200.0f)) * expf(-t * 28.0f);
            sample += sinf(pv.phase) * env * pv.volume;
            pv.phase = fmodf(pv.phase + twopi * PING_FREQ * dt, twopi);
            pv.samples_left--;
        }

        out[i] = sample;
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void crt_audio_init(void) {
    if (s_dev) return;

    SDL_AudioSpec want{}, got{};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_F32LSB;   // explicit LE float — AUDIO_F32SYS can mismatch
    want.channels = CHANNELS;
    want.samples  = BUFFER_SAMPLES;
    want.callback = audio_callback;
    want.userdata = nullptr;

    // SDL_AUDIO_ALLOW_FORMAT_CHANGES: if the device can't do F32, SDL will
    // convert for us rather than failing silently with garbage output.
    s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got,
                                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!s_dev) {
        SDL_Log("[CRT Audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    if (got.format != AUDIO_F32LSB && got.format != AUDIO_F32MSB) {
        SDL_Log("[CRT Audio] WARNING: got format 0x%x, expected F32 — audio may be garbled\n", got.format);
    }

    memset(&s_thunk, 0, sizeof(s_thunk));
    memset(&s_buzz,  0, sizeof(s_buzz));
    memset(s_pings,  0, sizeof(s_pings));

    SDL_PauseAudioDevice(s_dev, 0);
    SDL_Log("[CRT Audio] init OK — freq=%d fmt=0x%x ch=%d buf=%d\n",
            got.freq, got.format, got.channels, got.samples);
}

void crt_audio_shutdown(void) {
    if (s_dev) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
}

void crt_audio_set_mode(bool crt_on) {
    if (!s_dev) return;
    if (crt_on && !s_crt_was_on) {
        // Rising edge → fire thunk
        SDL_LockAudioDevice(s_dev);
        s_thunk.total_samples = (int)(THUNK_DURATION * SAMPLE_RATE);
        s_thunk.samples_left  = s_thunk.total_samples;
        s_thunk.phase         = 0.f;
        s_thunk.volume        = THUNK_VOLUME;
        s_thunk.active        = true;
        SDL_UnlockAudioDevice(s_dev);
    }
    if (!crt_on) {
        // Mode off — silence buzz immediately
        s_buzz.target_vol = 0.f;
    }
    s_crt_was_on = crt_on;
}

void crt_audio_set_activity(float level) {
    if (!s_dev || !s_crt_was_on) { s_buzz.target_vol = 0.f; return; }
    // level 0..1 → buzz target volume, clamped
    if (level < 0.f) level = 0.f;
    if (level > 1.f) level = 1.f;
    // Even at level=1 this is BUZZ_MAX_VOL — never loud
    s_buzz.target_vol = level * BUZZ_MAX_VOL;
}

void crt_audio_cursor_ping(void) {
    if (!s_dev || !s_crt_was_on) return;

    SDL_LockAudioDevice(s_dev);
    // Write into next slot (overwrites oldest if all busy — acceptable)
    PingVoice &pv = s_pings[s_ping_write % PING_MAX_STACK];
    s_ping_write++;
    pv.total_samples = (int)(PING_DURATION * SAMPLE_RATE);
    pv.samples_left  = pv.total_samples;
    pv.phase         = 0.f;
    pv.volume        = PING_VOLUME;
    SDL_UnlockAudioDevice(s_dev);
}
