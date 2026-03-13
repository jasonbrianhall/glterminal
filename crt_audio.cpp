#include "crt_audio.h"
#include "gl_renderer.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// TUNING
// ============================================================================

#define SAMPLE_RATE     44100
#define CHANNELS        1
#define BUFFER_SAMPLES  1024

// --- CRT: Power-on thunk -----------------------------------------------------
#define THUNK_FREQ      72.0f
#define THUNK_DURATION  0.18f
#define THUNK_VOLUME    0.22f

// --- CRT: Scanline buzz ------------------------------------------------------
#define BUZZ_FREQ       3000.0f
#define BUZZ_HARMONICS  3
#define BUZZ_MAX_VOL    0.015f
#define BUZZ_ATTACK     0.30f
#define BUZZ_RELEASE    0.50f

// --- CRT: Cursor ping --------------------------------------------------------
#define PING_FREQ       1800.0f
#define PING_DURATION   0.018f
#define PING_VOLUME     0.07f
#define PING_MAX_STACK  4

// --- VHS: Tape hiss ----------------------------------------------------------
// White noise low-passed to ~4kHz via a simple one-pole IIR.
#define VHS_HISS_VOL    0.028f
#define VHS_LP_COEFF    0.18f    // lower = more muffled, higher = brighter hiss
#define VHS_ATTACK      0.6f
#define VHS_RELEASE     0.8f

// --- C64: SID power-on arp ---------------------------------------------------
// Three-note minor arp on a buzzy square wave, fires once on mode entry.
#define C64_ARP_VOL     0.18f
#define C64_NOTE_DUR    0.055f   // seconds per note
#define C64_ARP_NOTES   4
static const float C64_FREQS[C64_ARP_NOTES] = { 130.8f, 155.6f, 196.0f, 261.6f }; // C3 Eb3 G3 C4

// --- Composite: 60Hz ground hum ----------------------------------------------
#define HUM_FREQ        60.0f
#define HUM_HARMONICS   4        // odd harmonics for transformer character
#define HUM_VOL         0.022f
#define HUM_ATTACK      0.4f
#define HUM_RELEASE     0.6f

// ============================================================================
// VOICE STRUCTS
// ============================================================================

struct ThunkVoice {
    float phase;
    int   samples_left, total_samples;
    float volume;
    bool  active;
};

struct BuzzVoice {
    float phase[BUZZ_HARMONICS];
    float current_vol, target_vol;
};

struct PingVoice {
    float phase;
    int   samples_left, total_samples;
    float volume;
};

struct VhsVoice {
    float lp_state;          // one-pole filter state
    float current_vol, target_vol;
};

struct C64Voice {
    float phase;
    int   note_idx;
    int   samples_left, samples_per_note;
    float volume;
    bool  active;
};

struct HumVoice {
    float phase[HUM_HARMONICS];
    float current_vol, target_vol;
};

// ============================================================================
// STATE
// ============================================================================

static SDL_AudioDeviceID s_dev        = 0;
static int               s_cur_mode   = RENDER_MODE_NORMAL;

static ThunkVoice  s_thunk  = {};
static BuzzVoice   s_buzz   = {};
static PingVoice   s_pings[PING_MAX_STACK] = {};
static int         s_ping_write = 0;
static VhsVoice    s_vhs    = {};
static C64Voice    s_c64    = {};
static HumVoice    s_hum    = {};

// ============================================================================
// AUDIO CALLBACK
// ============================================================================

static void audio_callback(void * /*userdata*/, Uint8 *stream, int len) {
    int    n     = len / (int)sizeof(float);
    float *out   = (float *)stream;
    memset(out, 0, len);

    const float dt    = 1.0f / (float)SAMPLE_RATE;
    const float twopi = 6.28318530f;

    for (int i = 0; i < n; i++) {
        float sample = 0.0f;

        // ── CRT thunk ─────────────────────────────────────────────────────
        if (s_thunk.active) {
            float t   = 1.0f - (float)s_thunk.samples_left / (float)s_thunk.total_samples;
            float env = expf(-t * 14.0f);
            sample += sinf(s_thunk.phase) * env * s_thunk.volume;
            s_thunk.phase = fmodf(s_thunk.phase + twopi * THUNK_FREQ * dt, twopi);
            if (--s_thunk.samples_left <= 0) s_thunk.active = false;
        }

        // ── CRT scanline buzz ─────────────────────────────────────────────
        {
            float diff = s_buzz.target_vol - s_buzz.current_vol;
            float rate = (diff > 0.f) ? (1.f / (BUZZ_ATTACK  * SAMPLE_RATE))
                                      : (1.f / (BUZZ_RELEASE * SAMPLE_RATE));
            if (fabsf(diff) < rate) s_buzz.current_vol = s_buzz.target_vol;
            else                    s_buzz.current_vol += (diff > 0.f ? rate : -rate);

            if (s_buzz.current_vol > 0.0001f) {
                float bsample = 0.0f;
                for (int h = 0; h < BUZZ_HARMONICS; h++) {
                    bsample += sinf(s_buzz.phase[h]) / (float)(h + 1);
                    s_buzz.phase[h] = fmodf(s_buzz.phase[h] + twopi * BUZZ_FREQ * (h+1) * dt, twopi);
                }
                sample += bsample * s_buzz.current_vol;
            }
        }

        // ── CRT cursor pings ──────────────────────────────────────────────
        for (int p = 0; p < PING_MAX_STACK; p++) {
            PingVoice &pv = s_pings[p];
            if (pv.samples_left <= 0) continue;
            float t   = 1.0f - (float)pv.samples_left / (float)pv.total_samples;
            float env = (1.0f - expf(-t * 200.0f)) * expf(-t * 28.0f);
            sample += sinf(pv.phase) * env * pv.volume;
            pv.phase = fmodf(pv.phase + twopi * PING_FREQ * dt, twopi);
            pv.samples_left--;
        }

        // ── VHS tape hiss ─────────────────────────────────────────────────
        {
            float diff = s_vhs.target_vol - s_vhs.current_vol;
            float rate = (diff > 0.f) ? (1.f / (VHS_ATTACK  * SAMPLE_RATE))
                                      : (1.f / (VHS_RELEASE * SAMPLE_RATE));
            if (fabsf(diff) < rate) s_vhs.current_vol = s_vhs.target_vol;
            else                    s_vhs.current_vol += (diff > 0.f ? rate : -rate);

            if (s_vhs.current_vol > 0.0001f) {
                // White noise → one-pole low-pass
                float noise = ((float)rand() / (float)RAND_MAX) * 2.f - 1.f;
                s_vhs.lp_state += VHS_LP_COEFF * (noise - s_vhs.lp_state);
                sample += s_vhs.lp_state * s_vhs.current_vol;
            }
        }

        // ── C64 SID arp ───────────────────────────────────────────────────
        if (s_c64.active) {
            // Square wave (sign of sine)
            float sq = sinf(s_c64.phase) >= 0.f ? 1.0f : -1.0f;
            // Envelope: short decay per note
            float t   = 1.0f - (float)s_c64.samples_left / (float)s_c64.samples_per_note;
            float env = expf(-t * 3.5f);
            sample += sq * env * s_c64.volume;
            s_c64.phase = fmodf(s_c64.phase + twopi * C64_FREQS[s_c64.note_idx] * dt, twopi);
            if (--s_c64.samples_left <= 0) {
                s_c64.note_idx++;
                if (s_c64.note_idx >= C64_ARP_NOTES) {
                    s_c64.active = false;
                } else {
                    s_c64.samples_left = s_c64.samples_per_note;
                    s_c64.phase = 0.f;
                }
            }
        }

        // ── Composite 60Hz hum ────────────────────────────────────────────
        {
            float diff = s_hum.target_vol - s_hum.current_vol;
            float rate = (diff > 0.f) ? (1.f / (HUM_ATTACK  * SAMPLE_RATE))
                                      : (1.f / (HUM_RELEASE * SAMPLE_RATE));
            if (fabsf(diff) < rate) s_hum.current_vol = s_hum.target_vol;
            else                    s_hum.current_vol += (diff > 0.f ? rate : -rate);

            if (s_hum.current_vol > 0.0001f) {
                float hsample = 0.0f;
                for (int h = 0; h < HUM_HARMONICS; h++) {
                    // Odd harmonics only (1,3,5,7) for transformer character
                    int harm = h * 2 + 1;
                    hsample += sinf(s_hum.phase[h]) / (float)harm;
                    s_hum.phase[h] = fmodf(s_hum.phase[h] + twopi * HUM_FREQ * harm * dt, twopi);
                }
                sample += hsample * s_hum.current_vol;
            }
        }

        // Soft clip to avoid any surprises
        if      (sample >  1.0f) sample =  1.0f;
        else if (sample < -1.0f) sample = -1.0f;

        out[i] = sample;
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void term_audio_init(void) {
    if (s_dev) return;

    SDL_AudioSpec want{}, got{};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_F32LSB;
    want.channels = CHANNELS;
    want.samples  = BUFFER_SAMPLES;
    want.callback = audio_callback;
    want.userdata = nullptr;

    s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got,
                                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!s_dev) {
        SDL_Log("[Audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    if (got.format != AUDIO_F32LSB && got.format != AUDIO_F32MSB)
        SDL_Log("[Audio] WARNING: got format 0x%x, expected F32\n", got.format);

    memset(&s_thunk, 0, sizeof(s_thunk));
    memset(&s_buzz,  0, sizeof(s_buzz));
    memset(s_pings,  0, sizeof(s_pings));
    memset(&s_vhs,   0, sizeof(s_vhs));
    memset(&s_c64,   0, sizeof(s_c64));
    memset(&s_hum,   0, sizeof(s_hum));

    SDL_PauseAudioDevice(s_dev, 0);
    SDL_Log("[Audio] init OK — freq=%d fmt=0x%x ch=%d buf=%d\n",
            got.freq, got.format, got.channels, got.samples);
}

void term_audio_shutdown(void) {
    if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; }
}

void term_audio_set_mode(int render_mode) {
    if (!s_dev) return;

    int prev = s_cur_mode;
    s_cur_mode = render_mode;

    // --- silence everything first, then set what's needed ---
    s_buzz.target_vol = 0.f;
    s_vhs.target_vol  = 0.f;
    s_hum.target_vol  = 0.f;

    switch (render_mode) {

    case RENDER_MODE_CRT:
        if (prev != RENDER_MODE_CRT) {
            // Power-on thunk
            SDL_LockAudioDevice(s_dev);
            s_thunk.total_samples = (int)(THUNK_DURATION * SAMPLE_RATE);
            s_thunk.samples_left  = s_thunk.total_samples;
            s_thunk.phase         = 0.f;
            s_thunk.volume        = THUNK_VOLUME;
            s_thunk.active        = true;
            SDL_UnlockAudioDevice(s_dev);
        }
        // buzz volume set each frame via term_audio_set_activity()
        break;

    case RENDER_MODE_VHS:
        s_vhs.target_vol = VHS_HISS_VOL;
        break;

    case RENDER_MODE_C64:
        if (prev != RENDER_MODE_C64) {
            SDL_LockAudioDevice(s_dev);
            s_c64.note_idx        = 0;
            s_c64.samples_per_note = (int)(C64_NOTE_DUR * SAMPLE_RATE);
            s_c64.samples_left    = s_c64.samples_per_note;
            s_c64.phase           = 0.f;
            s_c64.volume          = C64_ARP_VOL;
            s_c64.active          = true;
            SDL_UnlockAudioDevice(s_dev);
        }
        break;

    case RENDER_MODE_COMPOSITE:
        s_hum.target_vol = HUM_VOL;
        break;

    default:
        break;
    }
}

void term_audio_set_activity(float level) {
    if (!s_dev || s_cur_mode != RENDER_MODE_CRT) { s_buzz.target_vol = 0.f; return; }
    if (level < 0.f) level = 0.f;
    if (level > 1.f) level = 1.f;
    s_buzz.target_vol = level * BUZZ_MAX_VOL;
}

void term_audio_cursor_ping(void) {
    if (!s_dev || s_cur_mode != RENDER_MODE_CRT) return;
    SDL_LockAudioDevice(s_dev);
    PingVoice &pv = s_pings[s_ping_write % PING_MAX_STACK];
    s_ping_write++;
    pv.total_samples = (int)(PING_DURATION * SAMPLE_RATE);
    pv.samples_left  = pv.total_samples;
    pv.phase         = 0.f;
    pv.volume        = PING_VOLUME;
    SDL_UnlockAudioDevice(s_dev);
}
