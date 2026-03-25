// wopr_audio.cpp — FSK modem screech mixed into the project's audio callback
// via term_audio_wopr_play().  F32 mono, 44100 Hz — matches crt_audio.cpp.

#include "wopr.h"
#include "../crt_audio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const int    SAMPLE_RATE = 44100;
static const float  PI2         = 6.28318530718f;
static const double DURATION    = 3.5;

static float *s_pcm     = nullptr;
static int    s_samples = 0;

static void build_screech() {
    s_samples = (int)(SAMPLE_RATE * DURATION);
    s_pcm = (float *)malloc(s_samples * sizeof(float));
    if (!s_pcm) return;

    double ph_orig = 0, ph_ans = 0, ph_2100 = 0;
    int bit_period = SAMPLE_RATE / 300;
    int bit_clock  = 0, bit_orig = 0, bit_ans = 0;
    srand(0x19830603);

    for (int i = 0; i < s_samples; i++) {
        double t = (double)i / SAMPLE_RATE;
        float  s = 0.f;

        if (++bit_clock >= bit_period) {
            bit_clock = 0;
            bit_orig  = rand() % 2;
            bit_ans   = rand() % 2;
        }

        // 2100 Hz answer tone (0–0.4s)
        if (t < 0.40) s += sinf((float)ph_2100) * 0.50f;
        ph_2100 += PI2 * 2100.0 / SAMPLE_RATE;

        // Originate FSK 1070/1270 Hz (0.35–1.25s)
        if (t >= 0.35 && t < 1.25) {
            double env = (t < 0.45) ? (t - 0.35) / 0.10 : 1.0;
            s += sinf((float)ph_orig) * (float)(0.33 * env);
        }
        ph_orig += PI2 * (bit_orig ? 1270.0 : 1070.0) / SAMPLE_RATE;

        // Answer FSK 2025/2225 Hz (1.1–2.3s)
        if (t >= 1.10 && t < 2.30) {
            double env = (t < 1.20) ? (t - 1.10) / 0.10 : 1.0;
            s += sinf((float)ph_ans) * (float)(0.33 * env);
        }
        ph_ans += PI2 * (bit_ans ? 2225.0 : 2025.0) / SAMPLE_RATE;

        // Full duplex + noise burst (2.2–3.5s)
        if (t >= 2.20) {
            double env = (t < 2.30) ? (t - 2.20) / 0.10 : 1.0;
            s += sinf((float)ph_orig) * (float)(0.26 * env);
            s += sinf((float)ph_ans)  * (float)(0.26 * env);
            if (t >= 2.80 && t < 3.20)
                s += ((float)(rand() % 1000) / 1000.f - 0.5f) * 0.10f;
        }

        // Fade out
        if (t > DURATION - 0.30)
            s *= (float)((DURATION - t) / 0.30);

        if (s >  1.f) s =  1.f;
        if (s < -1.f) s = -1.f;
        s_pcm[i] = s;
    }
}

bool wopr_audio_init() {
    // Defer build until play time so the device is guaranteed open
    return true;
}

void wopr_audio_shutdown() {
    wopr_audio_stop();
    free(s_pcm);
    s_pcm = nullptr;
}

void wopr_audio_play_screech() {
    if (!s_pcm) build_screech();
    if (!s_pcm) return;
    term_audio_wopr_play(s_pcm, s_samples);
}

void wopr_audio_stop() {
    term_audio_wopr_stop();
}
