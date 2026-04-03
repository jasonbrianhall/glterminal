#include "wopr.h"
#include "wopr_salute.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <cctype>
#include "wopr_render.h"
#include <ctime>

// ============================================================================
// GLOBALS
// ============================================================================

WoprState g_wopr;

// ============================================================================
// CONSTANTS
// ============================================================================

struct GameEntry {
    const char *label;
    const char *keyword;
    WoprGame    id;
};

static const GameEntry GAMES[] = {
    { "CHESS",                    "CHESS",       WoprGame::CHESS      },
    { "FALKEN'S MAZE",            "MAZE",        WoprGame::FALKEN_MAZE},
    { "GLOBAL THERMONUCLEAR WAR", "WAR",         WoprGame::GLOBAL_WAR },
    { "TIC-TAC-TOE",              "TIC",         WoprGame::TIC_TAC_TOE},
    { "MINESWEEPER",              "MINESWEEPER", WoprGame::MINESWEEPER},
    { "ZORK",                     "ZORK",        WoprGame::ZORK       },
};
static const int GAME_COUNT = 6;

static const char *VALID_USER = "FALKEN";
static const char *VALID_PASS = "JOSHUA";

static const int INTEL_COUNT = 6;
static int g_intel_idx = 0;

static const int WIRE_STORY_COUNT = 8;
static int  g_wire_story_idx  = 0;

// Drip queue: lines waiting to be released one at a time into w->lines
static std::vector<std::string> g_drip_queue;
static double                   g_drip_acc   = 0.0;
static double                   g_drip_delay = 0.18;

// ============================================================================
// HELPERS
// ============================================================================

static std::string to_upper(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = (char)toupper((unsigned char)c);
    return r;
}

static void push_line(WoprState *w, const std::string &line) {
    w->lines.push_back(line);
}

static void begin_crawl(WoprState *w, const std::string &text) {
    w->crawl_target = text;
    w->crawl_pos    = 0;
    w->crawl_acc    = 0.0;
}

static bool crawl_done(const WoprState *w) {
    return w->crawl_pos >= (int)w->crawl_target.size();
}

static void crawl_commit(WoprState *w) {
    if (!w->crawl_target.empty())
        push_line(w, w->crawl_target);
    w->crawl_target.clear();
    w->crawl_pos = 0;
    w->crawl_acc = 0.0;
}

static void set_phase(WoprState *w, WoprPhase p) {
    w->phase       = p;
    w->phase_timer = 0.0;
    w->crawl_pos   = 0;
    w->crawl_acc   = 0.0;
}

// ============================================================================
// COMMAND SHELL
// ============================================================================

static std::vector<std::string> g_cmd_history;
static int                      g_history_cursor = -1;

static void enter_shell(WoprState *w) {
    push_line(w, "");
    push_line(w, "TYPE  HELP  FOR AVAILABLE COMMANDS.");
    push_line(w, "");
    w->input_buf.clear();
    g_history_cursor = -1;
    set_phase(w, WoprPhase::GAME_MENU);
}

static std::string normalise(const std::string &s) {
    std::string r;
    bool sp = true;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!sp) { r += ' '; sp = true; }
        } else {
            r += (char)toupper((unsigned char)c);
            sp = false;
        }
    }
    if (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

static void split_verb(const std::string &cmd, std::string &verb, std::string &rest) {
    auto pos = cmd.find(' ');
    if (pos == std::string::npos) {
        verb = cmd; rest = "";
    } else {
        verb = cmd.substr(0, pos);
        rest = cmd.substr(pos + 1);
    }
}

static WoprGame find_game(const std::string &token) {
    for (int i = 0; i < GAME_COUNT; i++) {
        std::string lbl = GAMES[i].label;
        std::string kw  = GAMES[i].keyword;
        if (token == kw || token == lbl ||
            (token.size() >= 3 && lbl.find(token) != std::string::npos))
            return GAMES[i].id;
    }
    return WoprGame::NONE;
}

static void launch_game(WoprState *w, WoprGame game) {
    push_line(w, "");
    switch (game) {
        case WoprGame::TIC_TAC_TOE:
            push_line(w, "INITIATING: TIC-TAC-TOE");
            set_phase(w, WoprPhase::PLAYING_TTT);
            wopr_ttt_enter(w);
            break;
        case WoprGame::CHESS:
            push_line(w, "INITIATING: CHESS");
            set_phase(w, WoprPhase::PLAYING_CHESS);
            wopr_chess_enter(w);
            break;
        case WoprGame::MINESWEEPER:
            push_line(w, "INITIATING: MINESWEEPER");
            set_phase(w, WoprPhase::PLAYING_MINES);
            wopr_mines_enter(w);
            break;
        case WoprGame::FALKEN_MAZE:
            push_line(w, "INITIATING: FALKEN'S MAZE");
            set_phase(w, WoprPhase::PLAYING_MAZE);
            wopr_maze_enter(w);
            break;
        case WoprGame::GLOBAL_WAR:
            push_line(w, "INITIATING: GLOBAL THERMONUCLEAR WAR");
            push_line(w, "");
            push_line(w, "  WARNING: THIS SIMULATION USES LIVE TARGETING DATA.");
            push_line(w, "  ESTIMATED CASUALTIES: 4.2 BILLION.");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_WAR);
            wopr_war_enter(w);
            break;
        case WoprGame::ZORK:
            push_line(w, "INITIATING: ZORK");
            push_line(w, "");
            push_line(w, "  LOADING GREAT UNDERGROUND EMPIRE...");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_ZORK);
            wopr_zork_enter(w);
            break;
        case WoprGame::BASIC:
            push_line(w, "INITIATING: BASIC");
            push_line(w, "");
            push_line(w, "  LOADING PROGRAMMING LANGUAGE...");
            push_line(w, "");
            set_phase(w, WoprPhase::PLAYING_ZORK);
            wopr_basic_enter(w);
            break;

        default: break;
    }
}

static void do_command(WoprState *w, const std::string &raw) {
    std::string cmd = normalise(raw);
    if (cmd.empty()) { push_line(w, ""); return; }

    // Echo with prompt
    push_line(w, std::string("WOPR> ") + cmd);
    push_line(w, "");

    // History
    g_cmd_history.push_back(cmd);
    if (g_cmd_history.size() > 32) g_cmd_history.erase(g_cmd_history.begin());
    g_history_cursor = -1;

    std::string verb, rest;
    split_verb(cmd, verb, rest);

    // ── HELP ──────────────────────────────────────────────────────────────
    if (verb == "HELP" || verb == "?") {
        push_line(w, "  AVAILABLE COMMANDS:");
        push_line(w, "");
        push_line(w, "  HELP                  DISPLAY THIS MESSAGE");
        push_line(w, "  LIST GAMES            SHOW AVAILABLE SIMULATIONS");
        push_line(w, "  PLAY <game>           LAUNCH A SIMULATION");
        push_line(w, "  INTELLIGENCE          DISPLAY CURRENT THREAT ASSESSMENT");
        push_line(w, "  WIRE                  INCOMING WIRE SERVICE FEED");
        push_line(w, "  SITREP                SITUATION REPORT -- ALL THEATERS");
        push_line(w, "  STATUS                SYSTEM AND DEFCON STATUS");
        push_line(w, "  WHOAMI                DISPLAY CURRENT USER RECORD");
        push_line(w, "  HISTORY               COMMAND HISTORY");
        push_line(w, "  CLEAR                 CLEAR TERMINAL BUFFER");
        push_line(w, "  LOGOUT / QUIT / EXIT  TERMINATE SESSION");
        push_line(w, "");
        push_line(w, "  EXAMPLE:  PLAY CHESS");
        push_line(w, "            PLAY GLOBAL THERMONUCLEAR WAR");
        push_line(w, "            PLAY ZORK");
        return;
    }

    // ── LIST (GAMES) ──────────────────────────────────────────────────────
    if (verb == "LIST" || cmd == "GAMES" || cmd == "LIST GAMES" || cmd == "SHOW GAMES") {
        push_line(w, "  AVAILABLE SIMULATIONS:");
        push_line(w, "");
        for (int i = 0; i < GAME_COUNT; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "    %-32s  (PLAY %s)", GAMES[i].label, GAMES[i].keyword);
            push_line(w, buf);
        }
        push_line(w, "");
        return;
    }

    // ── PLAY ──────────────────────────────────────────────────────────────
    if (verb == "PLAY" || verb == "RUN" || verb == "START") {
        if (rest.empty()) {
            push_line(w, "  USAGE:  PLAY <game name>");
            push_line(w, "  TYPE  LIST GAMES  FOR AVAILABLE SIMULATIONS.");
            return;
        }
        WoprGame g = find_game(rest);
        if (g == WoprGame::NONE) {
            push_line(w, "  SIMULATION NOT FOUND: " + rest);
            push_line(w, "  TYPE  LIST GAMES  TO SEE AVAILABLE SIMULATIONS.");
            return;
        }
        launch_game(w, g);
        return; // phase has changed; don't print trailing blank
    }

    // ── INTELLIGENCE ──────────────────────────────────────────────────────
    if (verb == "INTELLIGENCE" || verb == "INTEL" || verb == "THREAT") {
        push_line(w, "  *** CLASSIFIED -- EYES ONLY -- LEVEL 5 ***");
        push_line(w, "");
        const char *report = INTEL_REPORTS[g_intel_idx % INTEL_COUNT];
        g_intel_idx++;
        std::string line;
        for (const char *p = report; ; p++) {
            if (*p == '\n' || *p == '\0') {
                push_line(w, "  " + line);
                line.clear();
                if (*p == '\0') break;
            } else {
                line += *p;
            }
        }
        push_line(w, "");
        push_line(w, "  END OF REPORT.  DESTROY AFTER READING.");
        push_line(w, "");
        return;
    }

    // ── STATUS ────────────────────────────────────────────────────────────
    if (verb == "STATUS") {
        push_line(w, "  WOPR STRATEGIC DEFENSE SYSTEM  REV 4.1.7");
        push_line(w, "  UPTIME     : 847 DAYS  14 HRS  03 MIN");
        push_line(w, "  CPU LOAD   : 12.4%");
        push_line(w, "  SIMULATIONS: 19,432 COMPLETED THIS CYCLE");
        push_line(w, "");
        push_line(w, "  DEFCON STATUS    : 3");
        push_line(w, "  TRIAD READINESS  : GREEN");
        push_line(w, "  ABM NETWORK      : NOMINAL");
        push_line(w, "  SATELLITE UPLINK : ACTIVE  (3 OF 4 BIRDS ONLINE)");
        push_line(w, "");
        return;
    }

    // ── WHOAMI ────────────────────────────────────────────────────────────
    if (verb == "WHOAMI" || verb == "WHO" || cmd == "WHO AM I") {
        push_line(w, "  USER     : FALKEN, STEPHEN W.  DR.");
        push_line(w, "  CLEARANCE: TS/SCI  (CODEWORD: CRYSTAL WIND)");
        push_line(w, "  LAST LOGIN: 1983-06-03  03:14:07 MDT");
        push_line(w, "  SESSIONS : 1,847");
        push_line(w, "  NOTE     : ACCOUNT FLAGGED BY NSA -- SEE CASE FILE 7734");
        push_line(w, "");
        return;
    }

    // ── HISTORY ───────────────────────────────────────────────────────────
    if (verb == "HISTORY") {
        if (g_cmd_history.size() <= 1) {
            push_line(w, "  NO HISTORY.");
        } else {
            int start = std::max(0, (int)g_cmd_history.size() - 21); // -1 because we just added this cmd
            for (int i = start; i < (int)g_cmd_history.size() - 1; i++) {
                char buf[128];
                snprintf(buf, sizeof(buf), "  %3d  %s", i + 1, g_cmd_history[i].c_str());
                push_line(w, buf);
            }
        }
        push_line(w, "");
        return;
    }

    // ── CLEAR ─────────────────────────────────────────────────────────────
    if (verb == "CLEAR" || verb == "CLS") {
        w->lines.clear();
        push_line(w, "");
        return;
    }

    // ── LOGOUT / QUIT / EXIT ──────────────────────────────────────────────
    if (verb == "LOGOUT" || verb == "QUIT" || verb == "EXIT" || verb == "BYE") {
        begin_crawl(w, "SESSION TERMINATED.  GOODBYE, PROFESSOR FALKEN.");
        SDL_Delay(100);
        wopr_close();
        return;
    }

    // ── WIRE ──────────────────────────────────────────────────────────────
    if (verb == "WIRE" || cmd == "WIRE FEED" || cmd == "NEWS") {
        push_line(w, "  ** MILNET WIRE SERVICE -- UNCLASSIFIED FEED **");
        push_line(w, "  RECEIVING...");
        push_line(w, "");
        const char **story = WIRE_STORIES[g_wire_story_idx % WIRE_STORY_COUNT];
        g_wire_story_idx++;
        for (int i = 0; story[i] != nullptr; i++) {
            g_drip_queue.push_back(std::string("  ") + story[i]);
        }
        g_drip_queue.push_back("");
        g_drip_queue.push_back("  -- END TRANSMISSION --");
        g_drip_queue.push_back("");
        g_drip_acc = 0.0;
        return;
    }

    // ── SITREP ────────────────────────────────────────────────────────────
    if (verb == "SITREP" || cmd == "SIT REP" || cmd == "SITUATION REPORT") {

        srand((unsigned)time(nullptr));
        SaluteReport current_salute = SALUTE_POOL[rand()%SALUTE_POOL_SIZE];
        push_line(w, "Source: ");
        push_line(w, current_salute.source);
        push_line(w, "");

        push_line(w, "Activity:");
        push_line(w, current_salute.activity);
        if (!current_salute.activity2==NULL) push_line(w, current_salute.activity2);
        push_line(w, "");

        push_line(w, "Location:");
        push_line(w, current_salute.location);
        if (!current_salute.activity2==NULL) push_line(w, current_salute.location2);
        push_line(w, "");

        push_line(w, "Uniform:");
        push_line(w, current_salute.uniform);
        if (!current_salute.activity2==NULL) push_line(w, current_salute.uniform2);
        push_line(w, "");

        push_line(w, "Time:");
        push_line(w, current_salute.time_dtg);
        push_line(w, "");

        push_line(w, "Equipment:");
        push_line(w, current_salute.equipment);
        if (!current_salute.activity2==NULL) push_line(w, current_salute.equipment2);
        push_line(w, "");
        return;
    }

    // ── UNKNOWN ───────────────────────────────────────────────────────────
    push_line(w, "  COMMAND NOT RECOGNIZED: " + verb);
    push_line(w, "  TYPE  HELP  FOR AVAILABLE COMMANDS.");
    push_line(w, "");
}

// ============================================================================
// OPEN / CLOSE
// ============================================================================

void wopr_open() {
    WoprState *w = &g_wopr;
    *w = WoprState{};
    w->visible = true;
    w->lines.reserve(128);
    g_cmd_history.clear();
    g_history_cursor = -1;
    g_intel_idx      = 0;
    g_wire_story_idx = 0;
    g_drip_queue.clear();
    g_drip_acc       = 0.0;

    wopr_audio_play_screech();

    push_line(w, "");
    push_line(w, "MILNET NODE 347 -- WOPR AUTHENTICATION REQUIRED");
    push_line(w, "");
    set_phase(w, WoprPhase::LOGIN_PROMPT);
    begin_crawl(w, "USERNAME: ");
}

void wopr_close() {
    WoprState *w = &g_wopr;
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:   wopr_ttt_free(w);   break;
        case WoprPhase::PLAYING_CHESS: wopr_chess_free(w); break;
        case WoprPhase::PLAYING_MINES: wopr_mines_free(w); break;
        case WoprPhase::PLAYING_MAZE:  wopr_maze_free(w);  break;
        case WoprPhase::PLAYING_WAR:   wopr_war_free(w);   break;
        case WoprPhase::PLAYING_ZORK:  wopr_zork_free(w);  break;
        default: break;
    }
    wopr_audio_stop();
    w->visible = false;
    w->phase   = WoprPhase::INACTIVE;
}

// ============================================================================
// UPDATE
// ============================================================================

void wopr_update(double dt) {
    WoprState *w = &g_wopr;
    if (!w->visible) return;

    w->phase_timer += dt;

    // Advance crawl
    if (!w->crawl_target.empty() && w->crawl_pos < (int)w->crawl_target.size()) {
        w->crawl_acc += dt * w->crawl_baud / 10.0;
        int advance = (int)w->crawl_acc;
        if (advance > 0) {
            w->crawl_acc -= advance;
            w->crawl_pos = std::min(w->crawl_pos + advance,
                                    (int)w->crawl_target.size());
        }
    }

    switch (w->phase) {

    case WoprPhase::LOGIN_PROMPT:
        if (crawl_done(w)) {
            set_phase(w, WoprPhase::LOGIN_INPUT);
            w->input_buf.clear();
        }
        break;

    case WoprPhase::LOGIN_INPUT:
        break;

    case WoprPhase::LOGIN_REJECTED:
        if (w->phase_timer > 2.0) {
            push_line(w, "");
            w->username_done = false;
            w->input_buf.clear();
            if (w->reject_count >= 3) {
                push_line(w, "ACCESS DENIED. CONNECTION TERMINATED.");
                set_phase(w, WoprPhase::FAREWELL);
                begin_crawl(w, "");
            } else {
                set_phase(w, WoprPhase::LOGIN_PROMPT);
                begin_crawl(w, "USERNAME: ");
            }
        }
        break;

    case WoprPhase::CONNECTING:
        if (crawl_done(w) && w->phase_timer > 1.5) {
            crawl_commit(w);
            push_line(w, "");
            push_line(w, "  NORAD CHEYENNE MOUNTAIN COMPLEX");
            push_line(w, "  WARGAMES SYSTEMS DIVISION");
            push_line(w, "  CLASSIFIED -- LEVEL 5 CLEARANCE");
            push_line(w, "");
            push_line(w, "  HELLO, PROFESSOR FALKEN.");
            push_line(w, "  LAST LOGIN: 1983-06-03  03:14:07 MDT");
            enter_shell(w);
        }
        break;

    // CMD_PROMPT is stored in GAME_MENU slot
    case WoprPhase::GAME_MENU:
        // Drain wire drip queue
        if (!g_drip_queue.empty()) {
            g_drip_acc += dt;
            while (g_drip_acc >= g_drip_delay && !g_drip_queue.empty()) {
                g_drip_acc -= g_drip_delay;
                push_line(w, g_drip_queue.front());
                g_drip_queue.erase(g_drip_queue.begin());
            }
        }
        break;

    case WoprPhase::PLAYING_TTT:   wopr_ttt_update(w, dt);   break;
    case WoprPhase::PLAYING_CHESS: wopr_chess_update(w, dt); break;
    case WoprPhase::PLAYING_MINES: wopr_mines_update(w, dt); break;
    case WoprPhase::PLAYING_MAZE:  wopr_maze_update(w, dt);  break;
    case WoprPhase::PLAYING_WAR:   wopr_war_update(w, dt);   break;
    case WoprPhase::PLAYING_ZORK:  wopr_zork_update(w, dt);  break;

    case WoprPhase::FAREWELL:
        if (crawl_done(w) && w->phase_timer > 2.5)
            wopr_close();
        break;

    default: break;
    }
}

// ============================================================================
// RENDER
// ============================================================================

void wopr_render(int win_w, int win_h) {
    WoprState *w = &g_wopr;
    if (!w->visible) return;

    const int   PAD   = 24;
    const float SCALE = 1.0f;
    float ch = gl_text_height(SCALE);
    float cw = gl_text_width("M", SCALE);
    if (ch < 1.f) ch = 16.f;
    if (cw < 1.f) cw = 10.f;

    // Full-screen dark background
    gl_draw_rect(0, 0, (float)win_w, (float)win_h, 0.f, 0.f, 0.f, 0.92f);

    // Green border
    gl_draw_rect(PAD - 4, PAD - 4,
                 win_w - 2*(PAD-4), win_h - 2*(PAD-4),
                 0.f, 0.55f, 0.15f, 1.f);
    gl_draw_rect(PAD, PAD,
                 win_w - 2*PAD, win_h - 2*PAD,
                 0.f, 0.f, 0.f, 1.f);

    float x0     = PAD + 8;
    float y0     = PAD + 8;
    float area_w = win_w - 2*(PAD + 8);
    int   vis_rows = (int)((win_h - 2*(PAD + 8)) / ch) - 2;

    // Sub-game rendering (games with their own full UI)
    // Zork is intentionally NOT here — it uses the terminal scroll path below
    bool in_subgame = false;
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:
            wopr_ttt_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_CHESS:
            wopr_chess_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_MINES:
            wopr_mines_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_MAZE:
            wopr_maze_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        case WoprPhase::PLAYING_WAR:
            wopr_war_render(w, (int)x0, (int)y0, (int)cw, (int)ch, (int)(area_w/cw));
            in_subgame = true; break;
        default: break;
    }
    if (in_subgame) { gl_flush_verts(); return; }

    // ── Terminal log scroll ──────────────────────────────────────────────────
    int total       = (int)w->lines.size();
    int crawl_extra = w->crawl_target.empty() ? 0 : 1;
    bool in_shell   = (w->phase == WoprPhase::GAME_MENU);
    bool in_zork    = (w->phase == WoprPhase::PLAYING_ZORK);
    int input_extra = (w->phase == WoprPhase::LOGIN_INPUT || in_shell || in_zork) ? 1 : 0;
    int used        = total + crawl_extra + input_extra;
    int start_line  = std::max(0, used - vis_rows);

    float y = y0;
    for (int li = start_line; li < total; li++) {
        gl_draw_text(w->lines[li].c_str(), x0, y, 0.f, 1.f, 0.27f, 1.f, SCALE);
        y += ch;
    }

    // Active crawl line
    if (!w->crawl_target.empty()) {
        std::string partial = w->crawl_target.substr(0, w->crawl_pos);
        gl_draw_text(partial.c_str(), x0, y, 0.f, 1.f, 0.27f, 1.f, SCALE);
        y += ch;
    }

    // Input line
    if (w->phase == WoprPhase::LOGIN_INPUT) {
        std::string prompt;
        if (!w->username_done)
            prompt = "USERNAME: " + w->input_buf;
        else
            prompt = "PASSWORD: " + std::string(w->input_buf.size(), '*');
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, 0.f, 1.f, 0.27f, 1.f, SCALE);
    } else if (in_shell) {
        std::string prompt = "WOPR> " + w->input_buf;
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, 0.f, 1.f, 0.6f, 1.f, SCALE);
    } else if (in_zork) {
        std::string prompt = "> " + w->input_buf;
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, 0.f, 1.f, 0.6f, 1.f, SCALE);
    }

    gl_flush_verts();
}

// ============================================================================
// INPUT
// ============================================================================

static bool sub_back(WoprState *w) {
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:   wopr_ttt_free(w);   break;
        case WoprPhase::PLAYING_CHESS: wopr_chess_free(w); break;
        case WoprPhase::PLAYING_MINES: wopr_mines_free(w); break;
        case WoprPhase::PLAYING_MAZE:  wopr_maze_free(w);  break;
        case WoprPhase::PLAYING_WAR:   wopr_war_free(w);   break;
        case WoprPhase::PLAYING_ZORK:  wopr_zork_free(w);  break;
        default: return false;
    }
    push_line(w, "");
    push_line(w, "SIMULATION TERMINATED.");
    push_line(w, "");
    w->input_buf.clear();
    g_history_cursor = -1;
    set_phase(w, WoprPhase::GAME_MENU);
    return true;
}

bool wopr_keydown(SDL_Keycode sym, const char *text) {
    WoprState *w = &g_wopr;
    if (!w->visible) return false;

    // Global hotkeys always pass through to the main loop
    if (sym == SDLK_F11) return false;

    // Sub-games first
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_ttt_keydown(w, sym);
        case WoprPhase::PLAYING_CHESS:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_chess_keydown(w, sym);
        case WoprPhase::PLAYING_MINES:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_mines_keydown(w, sym);
        case WoprPhase::PLAYING_MAZE:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_maze_keydown(w, sym);
        case WoprPhase::PLAYING_WAR:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            return wopr_war_keydown(w, sym);
        case WoprPhase::PLAYING_ZORK:
            if (sym == SDLK_ESCAPE) { sub_back(w); return true; }
            if (text && *text)
                wopr_zork_text(w, text);
            return wopr_zork_keydown(w, sym);
        default: break;
    }

    // ESC always closes overlay (outside sub-games)
    if (sym == SDLK_ESCAPE) {
        wopr_close();
        return true;
    }

    // Speed up crawl on any key during login prompts / connecting
    if (w->phase == WoprPhase::LOGIN_PROMPT || w->phase == WoprPhase::CONNECTING) {
        w->crawl_pos = (int)w->crawl_target.size();
        return true;
    }

    // Login input
    if (w->phase == WoprPhase::LOGIN_INPUT) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (!w->username_done) {
                w->username_entered = to_upper(w->input_buf);
                push_line(w, "USERNAME: " + w->username_entered);
                w->username_done = true;
                w->input_buf.clear();
                begin_crawl(w, "PASSWORD: ");
                set_phase(w, WoprPhase::LOGIN_PROMPT);
            } else {
                w->password_entered = to_upper(w->input_buf);
                push_line(w, "PASSWORD: " + std::string(w->input_buf.size(), '*'));
                w->input_buf.clear();
                if (w->username_entered == VALID_USER &&
                    w->password_entered == VALID_PASS) {
                    push_line(w, "");
                    set_phase(w, WoprPhase::CONNECTING);
                    begin_crawl(w, "LOGON ACCEPTED -- CONNECTING TO WOPR MAINFRAME...");
                } else {
                    w->reject_count++;
                    push_line(w, "");
                    push_line(w, "IDENTIFICATION NOT RECOGNIZED.");
                    push_line(w, "PLEASE VERIFY AND RETRY.");
                    set_phase(w, WoprPhase::LOGIN_REJECTED);
                    w->username_done = false;
                }
            }
            return true;
        }
        if (sym == SDLK_BACKSPACE) {
            if (!w->input_buf.empty()) w->input_buf.pop_back();
            return true;
        }
        if (text && text[0] >= 32 && text[0] < 127 && w->input_buf.size() < 32) {
            w->input_buf += (char)toupper((unsigned char)text[0]);
            return true;
        }
        return true;
    }

    // CMD_PROMPT (stored as GAME_MENU)
    if (w->phase == WoprPhase::GAME_MENU) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            do_command(w, w->input_buf);
            w->input_buf.clear();
            g_history_cursor = -1;
            return true;
        }
        if (sym == SDLK_BACKSPACE) {
            if (!w->input_buf.empty()) w->input_buf.pop_back();
            return true;
        }
        // Command history navigation
        if (sym == SDLK_UP) {
            int hist_size = (int)g_cmd_history.size();
            if (hist_size > 0) {
                if (g_history_cursor < 0)
                    g_history_cursor = hist_size - 1;
                else if (g_history_cursor > 0)
                    g_history_cursor--;
                w->input_buf = g_cmd_history[g_history_cursor];
            }
            return true;
        }
        if (sym == SDLK_DOWN) {
            if (g_history_cursor >= 0) {
                g_history_cursor++;
                if (g_history_cursor >= (int)g_cmd_history.size()) {
                    g_history_cursor = -1;
                    w->input_buf.clear();
                } else {
                    w->input_buf = g_cmd_history[g_history_cursor];
                }
            }
            return true;
        }
        // Tab completion (verb only)
        if (sym == SDLK_TAB) {
            static const char *VERBS[] = {
                "HELP", "LIST GAMES", "PLAY ", "INTELLIGENCE",
                "WIRE", "SITREP", "STATUS", "WHOAMI", "HISTORY", "CLEAR", "LOGOUT"
            };
            std::string up = to_upper(w->input_buf);
            for (auto *v : VERBS) {
                if (std::string(v).find(up) == 0 && up.size() < strlen(v)) {
                    w->input_buf = v;
                    break;
                }
            }
            return true;
        }
        // Printable character
        if (text && text[0] >= 32 && text[0] < 127 && w->input_buf.size() < 80) {
            w->input_buf += (char)toupper((unsigned char)text[0]);
            return true;
        }
        return true;
    }

    return true;
}
