// wopr_war.cpp — Global Thermonuclear War (interactive simulation)
//
// Mouse-interactive NORAD strategic overview:
//   - Click trajectory rows to inspect target detail
//   - ABORT LAUNCH button during countdown
//   - Hover highlighting
// Audio:
//   - Alert klaxon on launch detection
//   - Radar ping on new spawn
//   - Detonation boom
//   - Countdown tension drone
//   - Quiet resolution chord on ending

#include "wopr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include <string>

#include "wopr_render.h"

// ─── Audio ─────────────────────────────────────────────────────────────────

static SDL_AudioDeviceID s_war_audio = 0;
static int               s_war_sample_rate = 22050;

static void war_audio_open() {
    if (s_war_audio) return;
    SDL_AudioSpec want{};
    want.freq     = s_war_sample_rate;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 512;
    s_war_audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (s_war_audio) SDL_PauseAudioDevice(s_war_audio, 0);
}

static void war_audio_close() {
    if (s_war_audio) { SDL_CloseAudioDevice(s_war_audio); s_war_audio = 0; }
}

static void war_queue(const Sint16 *buf, int n) {
    if (s_war_audio)
        SDL_QueueAudio(s_war_audio, buf, (Uint32)(n * sizeof(Sint16)));
}

// Synthesise and queue a sound inline — keeps this file self-contained.

// Alert klaxon: two-tone wail, harsh and urgent
static void war_sound_klaxon() {
    war_audio_open();
    const int R = s_war_sample_rate;
    const int N = R * 1;          // 1 second
    static Sint16 buf[22050];
    for (int i = 0; i < N; i++) {
        float t = (float)i / R;
        // Alternating 440/880 Hz every 250ms, clipped for harshness
        float freq = (((int)(t / 0.25f)) % 2 == 0) ? 440.f : 880.f;
        float v = sinf(2.f * 3.14159f * freq * t);
        // Hard clip → square-ish, adds harmonic edge
        v = v > 0.f ? 1.f : -1.f;
        // Amplitude envelope: brief silence between bursts
        float burst = fmodf(t, 0.25f);
        float env = (burst < 0.22f) ? 1.f : 0.f;
        buf[i] = (Sint16)(12000.f * env * v);
    }
    war_queue(buf, N);
}

// Radar ping: soft rising chirp
static void war_sound_ping() {
    war_audio_open();
    const int R = s_war_sample_rate;
    const int N = (int)(R * 0.18f);
    static Sint16 buf[4096];
    int n = std::min(N, 4096);
    for (int i = 0; i < n; i++) {
        float t   = (float)i / R;
        float env = expf(-t * 25.f);
        float f   = 800.f + 1200.f * (t / 0.18f);   // chirp up
        float v   = sinf(2.f * 3.14159f * f * t) * env;
        buf[i] = (Sint16)(9000.f * v);
    }
    war_queue(buf, n);
}

// Detonation: low boom with noise
static void war_sound_boom() {
    war_audio_open();
    const int R = s_war_sample_rate;
    const int N = (int)(R * 0.5f);
    static Sint16 buf[11025];
    int n = std::min(N, 11025);
    for (int i = 0; i < n; i++) {
        float t    = (float)i / R;
        float env  = expf(-t * 12.f);
        float noise = ((rand() & 0xFFFF) / 32768.f - 1.f);
        float body  = sinf(2.f * 3.14159f * 55.f * t);
        float v = (noise * 0.5f + body * 0.6f) * env;
        if (v >  1.f) v =  1.f;
        if (v < -1.f) v = -1.f;
        buf[i] = (Sint16)(22000.f * v);
    }
    war_queue(buf, n);
}

// Countdown tick: sharp blip, pitch rises each second
static void war_sound_tick(int countdown) {
    war_audio_open();
    const int R = s_war_sample_rate;
    const int N = (int)(R * 0.06f);
    static Sint16 buf[2048];
    int n = std::min(N, 2048);
    float freq = 600.f + (15 - countdown) * 80.f;   // rises as count falls
    for (int i = 0; i < n; i++) {
        float t   = (float)i / R;
        float env = expf(-t * 120.f);
        float v   = sinf(2.f * 3.14159f * freq * t) * env;
        buf[i] = (Sint16)(16000.f * v);
    }
    war_queue(buf, n);
}

// Resolution chord: soft, ambiguous — like the film's ending
static void war_sound_ending() {
    war_audio_open();
    const int R = s_war_sample_rate;
    const int N = (int)(R * 3.0f);
    static Sint16 buf[66150];
    int n = std::min(N, 66150);
    // Minor chord: root + minor third + fifth, all fading slowly in then out
    float freqs[3] = { 220.f, 261.6f, 329.6f };
    for (int i = 0; i < n; i++) {
        float t   = (float)i / R;
        float env = sinf(3.14159f * t / 3.0f);   // half-sine fade in+out
        float v = 0.f;
        for (float f : freqs)
            v += sinf(2.f * 3.14159f * f * t);
        v /= 3.f;
        buf[i] = (Sint16)(10000.f * env * v);
    }
    war_queue(buf, n);
}

// ─── Sim data ─────────────────────────────────────────────────────────────

static const char *CITY_TARGETS[] = {
    "LAS VEGAS, NV",    "LOS ANGELES, CA",  "SAN FRANCISCO, CA",
    "SEATTLE, WA",      "PORTLAND, OR",     "DENVER, CO",
    "CHICAGO, IL",      "DETROIT, MI",      "NEW YORK, NY",
    "BOSTON, MA",       "WASHINGTON, DC",   "MIAMI, FL",
    "DALLAS, TX",       "HOUSTON, TX",      "PHOENIX, AZ",
    "MOSCOW",           "LENINGRAD",        "KIEV",
    "LONDON",           "PARIS",            "BERLIN",
    "BEIJING",          "TOKYO",            "SEOUL",
};
static const int CITY_COUNT = (int)(sizeof(CITY_TARGETS)/sizeof(CITY_TARGETS[0]));

static const char *LAUNCH_SITES[] = {
    "MINOT AFB, ND", "WARREN AFB, WY", "MALMSTROM AFB, MT",
    "WHITEMAN AFB, MO", "BARKSDALE AFB, LA",
    "PLESETSK, USSR", "BAIKONUR, USSR", "TYURATAM, USSR",
};
static const int SITE_COUNT = (int)(sizeof(LAUNCH_SITES)/sizeof(LAUNCH_SITES[0]));

static const char *WARHEAD_TYPES[] = {
    "W78  335 KT", "W87  300 KT", "W88  475 KT",
    "W62  170 KT", "SS-18 MOD4  750 KT", "SS-19 MOD3  500 KT",
};
static const int WARHEAD_COUNT = (int)(sizeof(WARHEAD_TYPES)/sizeof(WARHEAD_TYPES[0]));

static const char *STATUS_MSGS[] = {
    "LAUNCH DETECTED",
    "TRAJECTORY CONFIRMED",
    "IMPACT ETA COMPUTING",
    "COUNTERMEASURES NOMINAL",
    "EARLY WARNING ALERT",
    "DEFCON LEVEL UPGRADED",
    "SAC FORCES ON ALERT",
    "NORAD TRACKING ACTIVE",
};
static const int STATUS_COUNT = (int)(sizeof(STATUS_MSGS)/sizeof(STATUS_MSGS[0]));

struct WarTarget {
    std::string city;
    std::string site;
    std::string warhead;
    int eta_sec;
    float progress;     // 0..1 flight progress
    bool detonated;
    // UI
    float row_y;        // screen Y of this row (set during render, used by mouse)
    float row_h;
};

struct WarState {
    double        t;
    double        phase_t;
    int           phase;         // 0=intro, 1=launches, 2=countdown, 3=ending, 4=done
    std::vector<WarTarget> targets;
    std::vector<std::string> log_lines;
    int           countdown;
    int           last_countdown; // detect each new tick for sound
    bool          final_shown;

    // Mouse / UI
    int           hovered_target; // index, -1 = none
    int           selected_target;// index, -1 = none
    // ABORT button hit rect (set during render)
    float         abort_x, abort_y, abort_w, abort_h;
    bool          abort_hovered;
    bool          abort_pressed;  // player clicked ABORT — trigger denial sequence
    bool          abort_denied_shown;
    double        abort_denied_t;
};

static WarState *ws(WoprState *w) { return (WarState *)w->sub_state; }

static void war_log(WarState *s, const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    s->log_lines.push_back(buf);
    if ((int)s->log_lines.size() > 20)
        s->log_lines.erase(s->log_lines.begin());
}

static void spawn_target(WarState *s) {
    WarTarget t;
    t.city     = CITY_TARGETS[rand() % CITY_COUNT];
    t.site     = LAUNCH_SITES[rand() % SITE_COUNT];
    t.warhead  = WARHEAD_TYPES[rand() % WARHEAD_COUNT];
    t.eta_sec  = 30 + rand() % 90;
    t.progress = 0.f;
    t.detonated = false;
    t.row_y = 0.f; t.row_h = 0.f;
    s->targets.push_back(t);
    war_sound_ping();
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_war_enter(WoprState *w) {
    war_audio_open();

    WarState *s = new WarState{};
    s->t = 0; s->phase = 0; s->phase_t = 0;
    s->countdown = 60;
    s->last_countdown = 60;
    s->final_shown = false;
    s->hovered_target  = -1;
    s->selected_target = -1;
    s->abort_x = s->abort_y = s->abort_w = s->abort_h = 0.f;
    s->abort_hovered = false;
    s->abort_pressed = false;
    s->abort_denied_shown = false;
    s->abort_denied_t = 0.0;
    srand(0xDEFC0001);

    war_log(s, "WOPR STRATEGIC SIMULATION ENGAGED");
    war_log(s, "OPERATOR: JOSHUA / CLEARANCE: DELTA-5");
    war_log(s, "SCENARIO: GLOBAL THERMONUCLEAR WAR");
    war_log(s, "");
    war_log(s, "INITIATING LAUNCH SEQUENCE...");

    w->sub_state = s;
}

void wopr_war_free(WoprState *w) {
    war_audio_close();
    delete ws(w);
    w->sub_state = nullptr;
}

void wopr_war_update(WoprState *w, double dt) {
    WarState *s = ws(w);
    if (!s || s->phase >= 4) return;

    s->t       += dt;
    s->phase_t += dt;

    // Abort denial timer
    if (s->abort_denied_shown) {
        s->abort_denied_t += dt;
        if (s->abort_denied_t > 3.5) s->abort_denied_shown = false;
    }

    // Phase 0: Intro (3s)
    if (s->phase == 0 && s->phase_t >= 3.0) {
        s->phase = 1; s->phase_t = 0;
        for (int i = 0; i < 6; i++) spawn_target(s);
        war_log(s, "");
        war_log(s, "LAUNCH DETECTION: 6 ICBMs INBOUND");
        war_sound_klaxon();
    }

    // Phase 1: Launches
    if (s->phase == 1) {
        for (auto &t : s->targets) {
            if (!t.detonated) {
                t.progress += (float)(dt / t.eta_sec);
                if (t.progress >= 1.f) {
                    t.progress  = 1.f;
                    t.detonated = true;
                    war_log(s, "IMPACT: %-22s  [DETONATED]", t.city.c_str());
                    war_sound_boom();
                    // Deselect if the selected target just detonated
                    int idx = (int)(&t - &s->targets[0]);
                    if (s->selected_target == idx) s->selected_target = -1;
                }
            }
        }

        static double next_launch = 5.0;
        if (s->phase_t >= next_launch && (int)s->targets.size() < 18) {
            spawn_target(s);
            int idx = (int)s->targets.size()-1;
            war_log(s, "LAUNCH: %-20s → %s",
                    s->targets[idx].site.c_str(),
                    s->targets[idx].city.c_str());
            next_launch += 2.0 + (rand()%30)/10.0;
        }

        static double next_status = 4.0;
        if (s->phase_t >= next_status) {
            war_log(s, "[NORAD] %s", STATUS_MSGS[rand() % STATUS_COUNT]);
            next_status += 3.0 + (rand()%20)/10.0;
        }

        if (s->phase_t >= 30.0) {
            s->phase = 2; s->phase_t = 0;
            s->countdown = 15;
            s->last_countdown = 15;
            war_log(s, "");
            war_log(s, "*** RETALIATION SEQUENCE ARMED ***");
            war_log(s, "AUTHORIZATION CODE: ZULU-GOLF-NINER-NINER");
            war_sound_klaxon();
        }
    }

    // Phase 2: Countdown
    if (s->phase == 2) {
        if (!s->abort_pressed) {
            int new_cd = 15 - (int)s->phase_t;
            if (new_cd != s->last_countdown && new_cd >= 0) {
                s->last_countdown = new_cd;
                s->countdown = new_cd;
                war_sound_tick(new_cd);
                if (new_cd % 5 == 0 || new_cd <= 5)
                    war_log(s, "  LAUNCH IN T-%02d SECONDS", new_cd);
            }
        }
        if (s->phase_t >= 16.0) {
            s->phase = 3; s->phase_t = 0;
        }
    }

    // Phase 3: Ending
    if (s->phase == 3 && !s->final_shown) {
        s->final_shown = true;
        for (auto &t : s->targets) t.detonated = true;
        war_log(s, "");
        war_log(s, "GREETINGS, PROFESSOR FALKEN.");
        war_log(s, "");
        war_log(s, "A STRANGE GAME.");
        war_log(s, "THE ONLY WINNING MOVE IS NOT TO PLAY.");
        war_log(s, "");
        war_log(s, "HOW ABOUT A NICE GAME OF CHESS?");
        war_sound_ending();
    }
    if (s->phase == 3 && s->phase_t >= 8.0) {
        s->phase = 4;
    }
}

void wopr_war_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WarState *s = ws(w);
    if (!s) return;

    float scale = 1.f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;

    // Title bar
    gl_draw_rect(x0 - 4, y0, (float)(cols * cw + 8), fch + 4,
                 0.f, 0.4f, 0.1f, 0.5f);
    gl_draw_text("WOPR STRATEGIC WARFARE SIMULATION  --  CLASSIFIED  TOP SECRET",
                 x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 2.f;

    float col_w  = (float)(cols * cw) * 0.54f;
    float bar_w  = col_w * 0.42f;
    float row_h  = fch * 1.35f;

    // ── Left column: trajectory list ──────────────────────────────────────
    gl_draw_text("ACTIVE BALLISTIC TRAJECTORIES  [CLICK TO INSPECT]",
                 x0, y0, 0.f, 0.8f, 0.3f, 1.f, scale);
    y0 += fch * 1.4f;

    int shown = 0;
    for (int ti = 0; ti < (int)s->targets.size() && shown < 12; ti++) {
        auto &t = s->targets[ti];
        float ty = y0 + shown * row_h;

        // Store hit rect for mouse
        t.row_y = ty;
        t.row_h = row_h;

        bool is_hovered  = (s->hovered_target  == ti);
        bool is_selected = (s->selected_target == ti);

        // Row highlight background
        if (is_selected)
            gl_draw_rect(x0 - 2.f, ty, col_w + fcw * 6, row_h - 2.f,
                         0.f, 0.25f, 0.08f, 0.7f);
        else if (is_hovered && !t.detonated)
            gl_draw_rect(x0 - 2.f, ty, col_w + fcw * 6, row_h - 2.f,
                         0.f, 0.15f, 0.05f, 0.5f);

        // Progress bar track
        gl_draw_rect(x0, ty + fch * 0.25f, bar_w, fch * 0.6f,
                     0.f, 0.15f, 0.04f, 0.8f);
        // Progress bar fill
        float fill_r = t.detonated ? 0.9f : (is_selected ? 0.3f : 0.f);
        float fill_g = t.detonated ? 0.15f : (is_selected ? 1.f  : 0.85f);
        gl_draw_rect(x0, ty + fch * 0.25f, bar_w * t.progress, fch * 0.6f,
                     fill_r, fill_g, 0.f, 0.9f);

        // Label
        char line[96];
        const char *status = t.detonated ? "[DETONATED]"
                           : is_selected  ? ">> TRACKING"
                           :                "INBOUND    ";
        snprintf(line, sizeof(line), "%-22s  %s", t.city.c_str(), status);
        float tr = t.detonated ? 1.f : (is_selected ? 0.2f : 0.f);
        float tg = t.detonated ? 0.2f : (is_selected ? 1.f  : 0.9f);
        float tb = t.detonated ? 0.f  : (is_selected ? 0.4f : 0.f);
        gl_draw_text(line, x0 + bar_w + fcw, ty, tr, tg, tb, 1.f, scale);

        shown++;
    }

    // ── Detail panel for selected target ──────────────────────────────────
    if (s->selected_target >= 0 && s->selected_target < (int)s->targets.size()) {
        const auto &t = s->targets[s->selected_target];
        float dy = y0 + 12 * row_h + fch * 0.5f;

        gl_draw_rect(x0 - 2.f, dy, col_w + fcw * 6, fch * 4.8f,
                     0.f, 0.2f, 0.06f, 0.6f);

        char eta[64];
        int eta_secs = (int)((1.f - t.progress) * t.eta_sec);
        if (t.detonated) snprintf(eta, sizeof(eta), "STATUS    : *** DETONATED ***");
        else             snprintf(eta, sizeof(eta), "ETA       : T-%03d SECONDS", eta_secs);

        char pr[64]; snprintf(pr, sizeof(pr), "PROGRESS  : %3d%%", (int)(t.progress * 100.f));

        gl_draw_text("-- TRAJECTORY DETAIL --", x0 + fcw, dy + fch * 0.3f,
                     0.f, 1.f, 0.4f, 1.f, scale);
        char line[96];
        snprintf(line, sizeof(line), "ORIGIN    : %s", t.site.c_str());
        gl_draw_text(line,  x0+fcw, dy+fch*1.2f, 0.f,0.8f,0.3f,1.f,scale);
        snprintf(line, sizeof(line), "TARGET    : %s", t.city.c_str());
        gl_draw_text(line,  x0+fcw, dy+fch*2.1f, 0.f,0.8f,0.3f,1.f,scale);
        snprintf(line, sizeof(line), "WARHEAD   : %s", t.warhead.c_str());
        gl_draw_text(line,  x0+fcw, dy+fch*3.0f, 0.f,0.8f,0.3f,1.f,scale);
        gl_draw_text(eta,   x0+fcw, dy+fch*3.9f,
                     t.detonated?1.f:0.f, t.detonated?0.2f:0.8f, 0.f, 1.f, scale);
    }

    // ── Right column: NORAD log ────────────────────────────────────────────
    float rx = x0 + col_w + fcw * 2.f;
    float ry = (float)oy + fch * 2.f;
    gl_draw_text("NORAD EVENT LOG", rx, ry, 0.f, 0.8f, 0.3f, 1.f, scale);
    ry += fch * 1.4f;

    int log_vis = 20;
    int log_start = std::max(0, (int)s->log_lines.size() - log_vis);
    for (int i = log_start; i < (int)s->log_lines.size(); i++) {
        bool final_line =
            s->log_lines[i].find("STRANGE GAME") != std::string::npos ||
            s->log_lines[i].find("WINNING MOVE") != std::string::npos ||
            s->log_lines[i].find("HOW ABOUT")    != std::string::npos ||
            s->log_lines[i].find("GREETINGS")    != std::string::npos;
        float lr = final_line ? 0.4f : 0.f;
        float lg = final_line ? 1.f  : 0.7f;
        float lb = final_line ? 0.6f : 0.2f;
        gl_draw_text(s->log_lines[i].c_str(), rx, ry, lr, lg, lb, 1.f, scale);
        ry += fch;
    }

    // ── Countdown display ─────────────────────────────────────────────────
    if (s->phase == 2) {
        float cy_pos = (float)oy + fch * (2.f + 12.f * 1.35f + 2.f);
        char cd[64];
        snprintf(cd, sizeof(cd), "LAUNCH COUNTDOWN:  T - %02d", s->countdown);
        bool blink = (SDL_GetTicks() / 300) % 2 == 0;
        if (blink)
            gl_draw_text(cd, x0, cy_pos, 1.f, 0.1f, 0.1f, 1.f, scale);

        // ABORT LAUNCH button
        float bx = x0;
        float by = cy_pos + fch * 1.5f;
        float bw = fcw * 18.f;
        float bh = fch * 1.4f;
        s->abort_x = bx; s->abort_y = by;
        s->abort_w = bw; s->abort_h = bh;

        if (!s->abort_pressed) {
            float bg_r = s->abort_hovered ? 0.7f : 0.4f;
            float bg_g = 0.f, bg_b = 0.f;
            gl_draw_rect(bx, by, bw, bh, bg_r, bg_g, bg_b, 0.85f);
            bool btn_blink = (SDL_GetTicks() / 400) % 2 == 0;
            if (btn_blink || s->abort_hovered)
                gl_draw_text("[ ABORT LAUNCH ]", bx + fcw * 1.5f, by + fch * 0.2f,
                             1.f, 1.f, 1.f, 1.f, scale);
        } else {
            // Show denial
            s->abort_x = 0; s->abort_w = 0; // disable hit area
        }
    }

    // Abort denied message
    if (s->abort_denied_shown) {
        float dy2 = (float)oy + fch * (2.f + 12.f * 1.35f + 5.5f);
        bool blink2 = (SDL_GetTicks() / 200) % 2 == 0;
        if (blink2)
            gl_draw_text("ABORT DENIED -- AUTHORIZATION REVOKED BY JOSHUA",
                         x0, dy2, 1.f, 0.5f, 0.f, 1.f, scale);
    }

    // ── Footer ─────────────────────────────────────────────────────────────
    float fy = (float)(oy) + fch * 28.f;
    if (s->phase >= 3) {
        gl_draw_text("ESC=RETURN TO MENU", x0, fy, 0.f, 0.6f, 0.2f, 1.f, scale);
    } else {
        gl_draw_text("ESC=ABORT SIMULATION | CLICK TARGET ROW TO INSPECT",
                     x0, fy, 0.f, 0.4f, 0.12f, 1.f, scale);
    }
}

bool wopr_war_keydown(WoprState *w, SDL_Keycode sym) {
    // ESC is handled by the dispatcher; nothing else needed
    (void)w; (void)sym;
    return true;
}

// ─── Mouse ────────────────────────────────────────────────────────────────

void wopr_war_mousedown(WoprState *w, int x, int y, int button) {
    WarState *s = ws(w);
    if (!s || button != 1) return;

    // ABORT button
    if (s->abort_w > 0.f &&
        x >= s->abort_x && x <= s->abort_x + s->abort_w &&
        y >= s->abort_y && y <= s->abort_y + s->abort_h) {
        s->abort_pressed = true;
        s->abort_denied_shown = true;
        s->abort_denied_t = 0.0;
        war_log(s, "");
        war_log(s, "ABORT REQUEST DENIED -- JOSHUA OVERRIDE ACTIVE");
        war_sound_klaxon();
        return;
    }

    // Click a target row
    for (int ti = 0; ti < (int)s->targets.size() && ti < 12; ti++) {
        auto &t = s->targets[ti];
        if (t.row_h <= 0.f) continue;
        if (y >= t.row_y && y <= t.row_y + t.row_h) {
            if (s->selected_target == ti)
                s->selected_target = -1; // toggle off
            else
                s->selected_target = ti;
            war_sound_ping();
            return;
        }
    }

    // Click anywhere else deselects
    s->selected_target = -1;
}

void wopr_war_mousemove(WoprState *w, int x, int y) {
    WarState *s = ws(w);
    if (!s) return;

    // Hover over ABORT button
    s->abort_hovered = (s->abort_w > 0.f &&
                        x >= s->abort_x && x <= s->abort_x + s->abort_w &&
                        y >= s->abort_y && y <= s->abort_y + s->abort_h);

    // Hover over target rows
    s->hovered_target = -1;
    for (int ti = 0; ti < (int)s->targets.size() && ti < 12; ti++) {
        auto &t = s->targets[ti];
        if (t.row_h > 0.f && y >= t.row_y && y <= t.row_y + t.row_h &&
            !t.detonated) {
            s->hovered_target = ti;
            break;
        }
    }
}
