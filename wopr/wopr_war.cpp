// wopr_war.cpp — Global Thermonuclear War (fake simulation)
//
// Displays a fake NORAD strategic overview with scrolling launch telemetry,
// target readouts, countdown sequences, and the classic ending.
// No actual input needed beyond ESC. Purely cinematic.

#include "wopr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include <string>

#include "wopr_render.h"

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
    int eta_sec;
    float progress; // 0..1 flight progress
    bool detonated;
};

struct WarState {
    double        t;             // elapsed seconds
    double        phase_t;       // time in current narrative phase
    int           phase;         // 0=intro, 1=launches, 2=countdown, 3=ending, 4=done
    std::vector<WarTarget> targets;
    std::vector<std::string> log_lines;
    int           countdown;
    bool          final_shown;
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
    t.eta_sec  = 30 + rand() % 90;
    t.progress = 0.f;
    t.detonated = false;
    s->targets.push_back(t);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_war_enter(WoprState *w) {
    WarState *s = new WarState{};
    s->t = 0; s->phase = 0; s->phase_t = 0;
    s->countdown = 60;
    s->final_shown = false;
    srand(0xDEFC0001);

    war_log(s, "WOPR STRATEGIC SIMULATION ENGAGED");
    war_log(s, "OPERATOR: JOSHUA / CLEARANCE: DELTA-5");
    war_log(s, "SCENARIO: GLOBAL THERMONUCLEAR WAR");
    war_log(s, "");
    war_log(s, "INITIATING LAUNCH SEQUENCE...");

    w->sub_state = s;
}

void wopr_war_free(WoprState *w) {
    delete ws(w);
    w->sub_state = nullptr;
}

void wopr_war_update(WoprState *w, double dt) {
    WarState *s = ws(w);
    if (!s || s->phase >= 4) return;

    s->t       += dt;
    s->phase_t += dt;

    // Phase 0: Intro (3s)
    if (s->phase == 0 && s->phase_t >= 3.0) {
        s->phase = 1; s->phase_t = 0;
        // Spawn initial wave
        for (int i = 0; i < 6; i++) spawn_target(s);
        war_log(s, "");
        war_log(s, "LAUNCH DETECTION: 6 ICBMs INBOUND");
    }

    // Phase 1: Launches — add more targets periodically, update flight
    if (s->phase == 1) {
        for (auto &t : s->targets) {
            if (!t.detonated) {
                t.progress += (float)(dt / t.eta_sec);
                if (t.progress >= 1.f) {
                    t.progress  = 1.f;
                    t.detonated = true;
                    war_log(s, "IMPACT: %-22s  [DETONATED]", t.city.c_str());
                }
            }
        }

        // Occasional new launches
        static double next_launch = 5.0;
        if (s->phase_t >= next_launch && (int)s->targets.size() < 18) {
            spawn_target(s);
            int idx = (int)s->targets.size()-1;
            war_log(s, "LAUNCH: %-20s → %s",
                    s->targets[idx].site.c_str(),
                    s->targets[idx].city.c_str());
            next_launch += 2.0 + (rand()%30)/10.0;
        }

        // Periodic status
        static double next_status = 4.0;
        if (s->phase_t >= next_status) {
            war_log(s, "[NORAD] %s", STATUS_MSGS[rand() % STATUS_COUNT]);
            next_status += 3.0 + (rand()%20)/10.0;
        }

        if (s->phase_t >= 30.0) {
            s->phase = 2; s->phase_t = 0;
            s->countdown = 15;
            war_log(s, "");
            war_log(s, "*** RETALIATION SEQUENCE ARMED ***");
            war_log(s, "AUTHORIZATION CODE: ZULU-GOLF-NINER-NINER");
        }
    }

    // Phase 2: Countdown
    if (s->phase == 2) {
        int new_cd = 15 - (int)s->phase_t;
        if (new_cd != s->countdown && new_cd >= 0) {
            s->countdown = new_cd;
            if (s->countdown % 5 == 0 || s->countdown <= 5)
                war_log(s, "  LAUNCH IN T-%02d SECONDS", s->countdown);
        }
        if (s->phase_t >= 16.0) {
            s->phase = 3; s->phase_t = 0;
        }
    }

    // Phase 3: Ending crawl
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
    y0 += fch * 2;

    // Left column: active targets (flight status bars)
    float col_w = (float)(cols * cw) * 0.55f;
    gl_draw_text("ACTIVE BALLISTIC TRAJECTORIES", x0, y0, 0.f, 0.8f, 0.3f, 1.f, scale);
    y0 += fch * 1.3f;

    int shown = 0;
    for (auto &t : s->targets) {
        if (shown >= 12) break;
        float ty = y0 + shown * fch * 1.3f;

        // Progress bar
        float bar_w = col_w * 0.45f;
        gl_draw_rect(x0, ty + fch * 0.2f, bar_w, fch * 0.7f,
                     0.f, 0.2f, 0.05f, 0.7f);
        gl_draw_rect(x0, ty + fch * 0.2f, bar_w * t.progress, fch * 0.7f,
                     t.detonated ? 1.f : 0.f,
                     t.detonated ? 0.2f : 1.f,
                     0.f, 0.9f);

        char line[80];
        snprintf(line, sizeof(line), "%-22s  %s",
                 t.city.c_str(), t.detonated ? "[DETONATED]" : "INBOUND");
        float tr = t.detonated ? 1.f : 0.f;
        float tg = t.detonated ? 0.2f : 0.9f;
        gl_draw_text(line, x0 + bar_w + fcw, ty, tr, tg, 0.f, 1.f, scale);
        shown++;
    }

    // Right column: log
    float rx = x0 + col_w + fcw * 2;
    float ry = (float)oy + fch * 2;
    gl_draw_text("NORAD LOG", rx, ry, 0.f, 0.8f, 0.3f, 1.f, scale);
    ry += fch * 1.3f;

    int log_vis = 20;
    int log_start = std::max(0, (int)s->log_lines.size() - log_vis);
    for (int i = log_start; i < (int)s->log_lines.size(); i++) {
        // Highlight ending lines
        bool final = (s->log_lines[i].find("STRANGE GAME") != std::string::npos ||
                      s->log_lines[i].find("WINNING MOVE") != std::string::npos ||
                      s->log_lines[i].find("HOW ABOUT")    != std::string::npos);
        float lr = final ? 0.f : 0.f;
        float lg = final ? 1.f : 0.7f;
        float lb = final ? 0.5f : 0.2f;
        gl_draw_text(s->log_lines[i].c_str(), rx, ry, lr, lg, lb, 1.f, scale);
        ry += fch;
    }

    // Countdown
    if (s->phase == 2) {
        float cy_pos = (float)oy + fch * (2 + 12 * 1.3f + 2);
        char cd[64];
        snprintf(cd, sizeof(cd), "LAUNCH COUNTDOWN:  T - %02d", s->countdown);
        bool blink = (SDL_GetTicks() / 300) % 2 == 0;
        if (blink)
            gl_draw_text(cd, x0, cy_pos, 1.f, 0.1f, 0.1f, 1.f, scale);
    }

    // Footer
    float fy = (float)(oy) + fch * 28;
    if (s->phase >= 3) {
        gl_draw_text("ESC=RETURN TO MENU", x0, fy, 0.f, 0.6f, 0.2f, 1.f, scale);
    } else {
        gl_draw_text("ESC=ABORT SIMULATION", x0, fy, 0.f, 0.4f, 0.1f, 1.f, scale);
    }
}

bool wopr_war_keydown(WoprState *w, SDL_Keycode sym) {
    // All key handling (including ESC) is done by the dispatcher in wopr.cpp
    (void)w; (void)sym;
    return true;
}
