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
#define VHS_HISS_VOL    0.028f
#define VHS_LP_COEFF    0.18f
#define VHS_ATTACK      0.6f
#define VHS_RELEASE     0.8f

// --- C64: SID power-on arp ---------------------------------------------------
#define C64_ARP_VOL     0.18f
#define C64_NOTE_DUR    0.055f
#define C64_ARP_NOTES   4
static const float C64_FREQS[C64_ARP_NOTES] = { 130.8f, 155.6f, 196.0f, 261.6f };

// --- Composite: 60Hz ground hum ----------------------------------------------
#define HUM_FREQ        60.0f
#define HUM_HARMONICS   4
#define HUM_VOL         0.022f
#define HUM_ATTACK      0.4f
#define HUM_RELEASE     0.6f

// --- Fight: Impact (punch/kick thud) ----------------------------------------
// Three weight classes, each a short noise burst shaped differently.
#define HIT_MAX_STACK   6
#define HIT_DURATION    0.06f    // light
#define HIT_DURATION_M  0.09f    // medium
#define HIT_DURATION_H  0.13f    // heavy
#define HIT_VOL_L       0.22f
#define HIT_VOL_M       0.30f
#define HIT_VOL_H       0.42f
#define HIT_LP_L        0.35f    // brighter smack
#define HIT_LP_M        0.22f
#define HIT_LP_H        0.12f    // muffled thud

// --- Fight: Block clank ------------------------------------------------------
#define BLOCK_FREQ      680.0f
#define BLOCK_DURATION  0.055f
#define BLOCK_VOL       0.18f

// --- Fight: Hadouken whoosh --------------------------------------------------
#define HDKN_DURATION   0.22f
#define HDKN_VOL        0.28f
#define HDKN_SWEEP_LO   180.0f
#define HDKN_SWEEP_HI   900.0f

// --- Fight: KO thud ----------------------------------------------------------
#define KO_FREQ_LO      55.0f
#define KO_FREQ_HI      120.0f
#define KO_DURATION     0.30f
#define KO_VOL          0.50f

// --- Fight: Crowd cheer (noise burst, modulated) -----------------------------
#define CHEER_DURATION  1.20f
#define CHEER_VOL       0.18f
#define CHEER_MOD_FREQ  7.0f     // crowd "wave" modulation

// --- Bouncing circle: Bounce bonk -------------------------------------------
#define BONK_MAX_STACK  4
#define BONK_BASE_FREQ  220.0f
#define BONK_FREQ_RANGE 180.0f   // pitch drops as ball grows
#define BONK_DURATION   0.08f
#define BONK_VOL        0.20f

// ============================================================================
// VOICE STRUCTS
// ============================================================================

struct ThunkVoice  { float phase; int samples_left, total_samples; float volume; bool active; };
struct BuzzVoice   { float phase[BUZZ_HARMONICS]; float current_vol, target_vol; };
struct PingVoice   { float phase; int samples_left, total_samples; float volume; };
struct VhsVoice    { float lp_state; float current_vol, target_vol; };
struct C64Voice    { float phase; int note_idx, samples_left, samples_per_note; float volume; bool active; };
struct HumVoice    { float phase[HUM_HARMONICS]; float current_vol, target_vol; };

// Generic noise-burst voice (fight hits, cheer)
struct NoiseBurst {
    float lp_state;
    float lp_coeff;
    int   samples_left, total_samples;
    float volume;
    bool  active;
};

// Metallic ping voice (block)
struct MetalPing { float phase; int samples_left, total_samples; float volume; bool active; };

// Frequency-sweep voice (hadouken, KO)
struct SweepVoice {
    float phase;
    float freq_lo, freq_hi;
    int   samples_left, total_samples;
    float volume;
    bool  active;
};

// Crowd cheer
struct CheerVoice {
    float lp_state;
    float mod_phase;
    int   samples_left, total_samples;
    float volume;
    bool  active;
};

// Bounce bonk
struct BonkVoice { float phase; float freq; int samples_left, total_samples; float volume; };

// ============================================================================
// STATE
// ============================================================================

static SDL_AudioDeviceID s_dev      = 0;
static int               s_cur_mode = RENDER_MODE_NORMAL;
static bool              s_audio_enabled = false;  // off by default

// CRT
static ThunkVoice  s_thunk = {};
static BuzzVoice   s_buzz  = {};
static PingVoice   s_pings[PING_MAX_STACK] = {};
static int         s_ping_write = 0;
// VHS
static VhsVoice    s_vhs = {};
// C64
static C64Voice    s_c64 = {};
// Composite
static HumVoice    s_hum = {};
// Fight
static NoiseBurst  s_hits[HIT_MAX_STACK] = {};
static int         s_hit_write = 0;
static MetalPing   s_block = {};
static SweepVoice  s_hadouken = {};
static SweepVoice  s_ko = {};
static CheerVoice  s_cheer = {};
// Bouncing circle
static BonkVoice   s_bonks[BONK_MAX_STACK] = {};
static int         s_bonk_write = 0;

// ============================================================================
// AUDIO CALLBACK
// ============================================================================

static inline float smooth(float &cur, float target, float attack, float release) {
    float diff = target - cur;
    float rate = (diff > 0.f) ? (1.f / (attack  * SAMPLE_RATE))
                               : (1.f / (release * SAMPLE_RATE));
    if (fabsf(diff) < rate) cur = target;
    else cur += (diff > 0.f ? rate : -rate);
    return cur;
}

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
            sample   += sinf(s_thunk.phase) * expf(-t * 14.0f) * s_thunk.volume;
            s_thunk.phase = fmodf(s_thunk.phase + twopi * THUNK_FREQ * dt, twopi);
            if (--s_thunk.samples_left <= 0) s_thunk.active = false;
        }

        // ── CRT scanline buzz ─────────────────────────────────────────────
        {
            smooth(s_buzz.current_vol, s_buzz.target_vol, BUZZ_ATTACK, BUZZ_RELEASE);
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
            smooth(s_vhs.current_vol, s_vhs.target_vol, VHS_ATTACK, VHS_RELEASE);
            if (s_vhs.current_vol > 0.0001f) {
                float noise = ((float)rand() / (float)RAND_MAX) * 2.f - 1.f;
                s_vhs.lp_state += VHS_LP_COEFF * (noise - s_vhs.lp_state);
                sample += s_vhs.lp_state * s_vhs.current_vol;
            }
        }

        // ── C64 SID arp ───────────────────────────────────────────────────
        if (s_c64.active) {
            float t   = 1.0f - (float)s_c64.samples_left / (float)s_c64.samples_per_note;
            float sq  = sinf(s_c64.phase) >= 0.f ? 1.0f : -1.0f;
            sample   += sq * expf(-t * 3.5f) * s_c64.volume;
            s_c64.phase = fmodf(s_c64.phase + twopi * C64_FREQS[s_c64.note_idx] * dt, twopi);
            if (--s_c64.samples_left <= 0) {
                if (++s_c64.note_idx >= C64_ARP_NOTES) s_c64.active = false;
                else { s_c64.samples_left = s_c64.samples_per_note; s_c64.phase = 0.f; }
            }
        }

        // ── Composite 60Hz hum ────────────────────────────────────────────
        {
            smooth(s_hum.current_vol, s_hum.target_vol, HUM_ATTACK, HUM_RELEASE);
            if (s_hum.current_vol > 0.0001f) {
                float hsample = 0.0f;
                for (int h = 0; h < HUM_HARMONICS; h++) {
                    int harm = h * 2 + 1;
                    hsample += sinf(s_hum.phase[h]) / (float)harm;
                    s_hum.phase[h] = fmodf(s_hum.phase[h] + twopi * HUM_FREQ * harm * dt, twopi);
                }
                sample += hsample * s_hum.current_vol;
            }
        }

        // ── Fight: hits (noise bursts) ─────────────────────────────────────
        for (int h = 0; h < HIT_MAX_STACK; h++) {
            NoiseBurst &nb = s_hits[h];
            if (!nb.active) continue;
            float t   = 1.0f - (float)nb.samples_left / (float)nb.total_samples;
            // Percussive envelope: very fast attack, exponential decay
            float env = expf(-t * 22.0f);
            float noise = ((float)rand() / (float)RAND_MAX) * 2.f - 1.f;
            nb.lp_state += nb.lp_coeff * (noise - nb.lp_state);
            sample += nb.lp_state * env * nb.volume;
            if (--nb.samples_left <= 0) nb.active = false;
        }

        // ── Fight: block clank ────────────────────────────────────────────
        if (s_block.active) {
            float t   = 1.0f - (float)s_block.samples_left / (float)s_block.total_samples;
            float env = expf(-t * 35.0f);
            sample   += sinf(s_block.phase) * env * s_block.volume;
            // Add a bit of inharmonic ring for metallic quality
            sample   += sinf(s_block.phase * 2.73f) * env * s_block.volume * 0.3f;
            s_block.phase = fmodf(s_block.phase + twopi * BLOCK_FREQ * dt, twopi);
            if (--s_block.samples_left <= 0) s_block.active = false;
        }

        // ── Fight: hadouken sweep ─────────────────────────────────────────
        if (s_hadouken.active) {
            float t    = 1.0f - (float)s_hadouken.samples_left / (float)s_hadouken.total_samples;
            float freq = s_hadouken.freq_lo + (s_hadouken.freq_hi - s_hadouken.freq_lo) * t;
            float env  = sinf(t * 3.14159f); // bell shape
            sample    += sinf(s_hadouken.phase) * env * s_hadouken.volume;
            // Second harmonic for a richer whoosh
            sample    += sinf(s_hadouken.phase * 1.5f) * env * s_hadouken.volume * 0.4f;
            s_hadouken.phase = fmodf(s_hadouken.phase + twopi * freq * dt, twopi);
            if (--s_hadouken.samples_left <= 0) s_hadouken.active = false;
        }

        // ── Fight: KO thud ───────────────────────────────────────────────
        if (s_ko.active) {
            float t    = 1.0f - (float)s_ko.samples_left / (float)s_ko.total_samples;
            float freq = s_ko.freq_hi + (s_ko.freq_lo - s_ko.freq_hi) * t; // pitch drops
            float env  = expf(-t * 8.0f);
            sample    += sinf(s_ko.phase) * env * s_ko.volume;
            s_ko.phase = fmodf(s_ko.phase + twopi * freq * dt, twopi);
            if (--s_ko.samples_left <= 0) s_ko.active = false;
        }

        // ── Fight: crowd cheer ────────────────────────────────────────────
        if (s_cheer.active) {
            float t    = 1.0f - (float)s_cheer.samples_left / (float)s_cheer.total_samples;
            // Envelope: fast rise, slow decay
            float env  = (1.0f - expf(-t * 8.0f)) * expf(-t * 3.5f);
            // AM modulation at cheer frequency simulates crowd waves
            float mod  = 0.5f + 0.5f * sinf(s_cheer.mod_phase);
            s_cheer.mod_phase = fmodf(s_cheer.mod_phase + twopi * CHEER_MOD_FREQ * dt, twopi);
            float noise = ((float)rand() / (float)RAND_MAX) * 2.f - 1.f;
            s_cheer.lp_state += 0.08f * (noise - s_cheer.lp_state);
            sample += s_cheer.lp_state * mod * env * s_cheer.volume;
            if (--s_cheer.samples_left <= 0) s_cheer.active = false;
        }

        // ── Bouncing circle: bonks ────────────────────────────────────────
        for (int b = 0; b < BONK_MAX_STACK; b++) {
            BonkVoice &bv = s_bonks[b];
            if (bv.samples_left <= 0) continue;
            float t   = 1.0f - (float)bv.samples_left / (float)bv.total_samples;
            float env = expf(-t * 30.0f);
            sample   += sinf(bv.phase) * env * bv.volume;
            // Add one overtone for a "boing" quality
            sample   += sinf(bv.phase * 2.1f) * env * bv.volume * 0.35f;
            bv.phase  = fmodf(bv.phase + twopi * bv.freq * dt, twopi);
            bv.samples_left--;
        }

        // Soft clip
        if      (sample >  1.0f) sample =  1.0f;
        else if (sample < -1.0f) sample = -1.0f;
        out[i] = sample;
    }
}

// ============================================================================
// PUBLIC API — lifecycle
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

    memset(&s_thunk,    0, sizeof(s_thunk));
    memset(&s_buzz,     0, sizeof(s_buzz));
    memset(s_pings,     0, sizeof(s_pings));
    memset(&s_vhs,      0, sizeof(s_vhs));
    memset(&s_c64,      0, sizeof(s_c64));
    memset(&s_hum,      0, sizeof(s_hum));
    memset(s_hits,      0, sizeof(s_hits));
    memset(&s_block,    0, sizeof(s_block));
    memset(&s_hadouken, 0, sizeof(s_hadouken));
    memset(&s_ko,       0, sizeof(s_ko));
    memset(&s_cheer,    0, sizeof(s_cheer));
    memset(s_bonks,     0, sizeof(s_bonks));

    SDL_PauseAudioDevice(s_dev, 1);  // start paused — enabled via settings
    SDL_Log("[Audio] init OK — freq=%d fmt=0x%x ch=%d buf=%d\n",
            got.freq, got.format, got.channels, got.samples);
}

void term_audio_shutdown(void) {
    if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; }
}

void term_audio_set_enabled(bool en) {
    s_audio_enabled = en;
    if (s_dev) SDL_PauseAudioDevice(s_dev, en ? 0 : 1);
}

bool term_audio_get_enabled(void) {
    return s_audio_enabled;
}

// ============================================================================
// PUBLIC API — render mode
// ============================================================================

void term_audio_set_mode(int render_mode) {
    if (!s_dev) return;
    int prev   = s_cur_mode;
    s_cur_mode = render_mode;

    s_buzz.target_vol = 0.f;
    s_vhs.target_vol  = 0.f;
    s_hum.target_vol  = 0.f;

    switch (render_mode) {
    case RENDER_MODE_CRT:
        if (prev != RENDER_MODE_CRT) {
            SDL_LockAudioDevice(s_dev);
            s_thunk.total_samples = (int)(THUNK_DURATION * SAMPLE_RATE);
            s_thunk.samples_left  = s_thunk.total_samples;
            s_thunk.phase  = 0.f;
            s_thunk.volume = THUNK_VOLUME;
            s_thunk.active = true;
            SDL_UnlockAudioDevice(s_dev);
        }
        break;
    case RENDER_MODE_VHS:
        s_vhs.target_vol = VHS_HISS_VOL;
        break;
    case RENDER_MODE_C64:
        if (prev != RENDER_MODE_C64) {
            SDL_LockAudioDevice(s_dev);
            s_c64.note_idx         = 0;
            s_c64.samples_per_note = (int)(C64_NOTE_DUR * SAMPLE_RATE);
            s_c64.samples_left     = s_c64.samples_per_note;
            s_c64.phase            = 0.f;
            s_c64.volume           = C64_ARP_VOL;
            s_c64.active           = true;
            SDL_UnlockAudioDevice(s_dev);
        }
        break;
    case RENDER_MODE_COMPOSITE:
        s_hum.target_vol = HUM_VOL;
        break;
    default: break;
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
    pv.phase  = 0.f;
    pv.volume = PING_VOLUME;
    SDL_UnlockAudioDevice(s_dev);
}

// ============================================================================
// PUBLIC API — fight mode events
// ============================================================================

void fight_audio_hit(float /*x*/, float /*y*/, int weight) {
    if (!s_dev) return;
    float dur = (weight == 0) ? HIT_DURATION : (weight == 1) ? HIT_DURATION_M : HIT_DURATION_H;
    float vol = (weight == 0) ? HIT_VOL_L    : (weight == 1) ? HIT_VOL_M    : HIT_VOL_H;
    float lp  = (weight == 0) ? HIT_LP_L     : (weight == 1) ? HIT_LP_M     : HIT_LP_H;

    SDL_LockAudioDevice(s_dev);
    NoiseBurst &nb = s_hits[s_hit_write % HIT_MAX_STACK];
    s_hit_write++;
    nb.total_samples = (int)(dur * SAMPLE_RATE);
    nb.samples_left  = nb.total_samples;
    nb.lp_coeff      = lp;
    nb.lp_state      = 0.f;
    nb.volume        = vol;
    nb.active        = true;
    SDL_UnlockAudioDevice(s_dev);
}

void fight_audio_block(void) {
    if (!s_dev) return;
    SDL_LockAudioDevice(s_dev);
    s_block.total_samples = (int)(BLOCK_DURATION * SAMPLE_RATE);
    s_block.samples_left  = s_block.total_samples;
    s_block.phase  = 0.f;
    s_block.volume = BLOCK_VOL;
    s_block.active = true;
    SDL_UnlockAudioDevice(s_dev);
}

void fight_audio_hadouken_launch(void) {
    if (!s_dev) return;
    SDL_LockAudioDevice(s_dev);
    s_hadouken.total_samples = (int)(HDKN_DURATION * SAMPLE_RATE);
    s_hadouken.samples_left  = s_hadouken.total_samples;
    s_hadouken.freq_lo = HDKN_SWEEP_LO;
    s_hadouken.freq_hi = HDKN_SWEEP_HI;
    s_hadouken.phase   = 0.f;
    s_hadouken.volume  = HDKN_VOL;
    s_hadouken.active  = true;
    SDL_UnlockAudioDevice(s_dev);
}

void fight_audio_ko(void) {
    if (!s_dev) return;
    SDL_LockAudioDevice(s_dev);
    s_ko.total_samples = (int)(KO_DURATION * SAMPLE_RATE);
    s_ko.samples_left  = s_ko.total_samples;
    s_ko.freq_lo = KO_FREQ_LO;
    s_ko.freq_hi = KO_FREQ_HI;
    s_ko.phase   = 0.f;
    s_ko.volume  = KO_VOL;
    s_ko.active  = true;
    SDL_UnlockAudioDevice(s_dev);
}

void fight_audio_cheer(void) {
    if (!s_dev) return;
    SDL_LockAudioDevice(s_dev);
    s_cheer.total_samples = (int)(CHEER_DURATION * SAMPLE_RATE);
    s_cheer.samples_left  = s_cheer.total_samples;
    s_cheer.lp_state      = 0.f;
    s_cheer.mod_phase     = 0.f;
    s_cheer.volume        = CHEER_VOL;
    s_cheer.active        = true;
    SDL_UnlockAudioDevice(s_dev);
}

// ============================================================================
// PUBLIC API — bouncing circle
// ============================================================================

void bc_audio_bounce(float radius_frac) {
    if (!s_dev) return;
    if (radius_frac < 0.f) radius_frac = 0.f;
    if (radius_frac > 1.f) radius_frac = 1.f;
    // Pitch drops as ball grows — bigger ball = lower thud
    float freq = BONK_BASE_FREQ + BONK_FREQ_RANGE * (1.0f - radius_frac);

    SDL_LockAudioDevice(s_dev);
    BonkVoice &bv = s_bonks[s_bonk_write % BONK_MAX_STACK];
    s_bonk_write++;
    bv.total_samples = (int)(BONK_DURATION * SAMPLE_RATE);
    bv.samples_left  = bv.total_samples;
    bv.freq          = freq;
    bv.phase         = 0.f;
    bv.volume        = BONK_VOL;
    SDL_UnlockAudioDevice(s_dev);
}
