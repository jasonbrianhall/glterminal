#include "wopr.h"
#include "../font_manager.h"   // ft_draw_text / ft_text_width etc. from your project
#include <SDL2/SDL.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <cctype>

// ============================================================================
// GLOBALS
// ============================================================================

WoprState g_wopr;

// ============================================================================
// CONSTANTS
// ============================================================================

static const char *WOPR_BANNER[] = {
    "                                                    ",
    "  ██╗    ██╗ ██████╗ ██████╗ ██████╗               ",
    "  ██║    ██║██╔═══██╗██╔══██╗██╔══██╗              ",
    "  ██║ █╗ ██║██║   ██║██████╔╝██████╔╝              ",
    "  ██║███╗██║██║   ██║██╔═══╝ ██╔══██╗              ",
    "  ╚███╔███╔╝╚██████╔╝██║     ██║  ██║              ",
    "   ╚══╝╚══╝  ╚═════╝ ╚═╝     ╚═╝  ╚═╝              ",
    "                                                    ",
    "  WAR OPERATION PLAN RESPONSE  --  DEFCON INTERFACE ",
    "  NORTHWESTERN UNIVERSITY, NORAD SECTOR 7           ",
    nullptr
};

static const char *GAME_LABELS[] = {
    "CHESS",
    "FALKEN'S MAZE",
    "GLOBAL THERMONUCLEAR WAR",
    "TIC-TAC-TOE",
    "MINESWEEPER",
};
static const WoprGame GAME_IDS[] = {
    WoprGame::CHESS,
    WoprGame::FALKEN_MAZE,
    WoprGame::GLOBAL_WAR,
    WoprGame::TIC_TAC_TOE,
    WoprGame::MINESWEEPER,
};
static const int GAME_COUNT = 5;

// Credentials (case-insensitive)
static const char *VALID_USER = "FALKEN";
static const char *VALID_PASS = "JOSHUA";

// ============================================================================
// HELPERS
// ============================================================================

static std::string to_upper(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = (char)toupper((unsigned char)c);
    return r;
}

// Append a line to the terminal buffer
static void push_line(WoprState *w, const std::string &line) {
    w->lines.push_back(line);
}

// Start a new crawl sequence (text appears at 300 baud = 30 cps ≈ 33ms/char)
static void begin_crawl(WoprState *w, const std::string &text) {
    w->crawl_target = text;
    w->crawl_pos    = 0;
    w->crawl_acc    = 0.0;
}

static bool crawl_done(const WoprState *w) {
    return w->crawl_pos >= (int)w->crawl_target.size();
}

// Flush whatever the crawl has so far into lines[], start fresh
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
// OPEN / CLOSE
// ============================================================================

void wopr_open() {
    WoprState *w = &g_wopr;
    *w = WoprState{};
    w->visible = true;
    w->lines.reserve(64);
    SDL_Log("[WOPR] opened, visible=%d\n", w->visible);
    set_phase(w, WoprPhase::MODEM_SCREECH);
    wopr_audio_play_screech();
}

void wopr_close() {
    WoprState *w = &g_wopr;

    // Free any running sub-game
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:   wopr_ttt_free(w);   break;
        case WoprPhase::PLAYING_CHESS: wopr_chess_free(w); break;
        case WoprPhase::PLAYING_MINES: wopr_mines_free(w); break;
        case WoprPhase::PLAYING_MAZE:  wopr_maze_free(w);  break;
        case WoprPhase::PLAYING_WAR:   wopr_war_free(w);   break;
        default: break;
    }
    wopr_audio_stop();
    w->visible = false;
    w->phase   = WoprPhase::INACTIVE;
}

// ============================================================================
// UPDATE
// ============================================================================

// Called when the login sequence completes successfully
static void begin_connecting(WoprState *w) {
    push_line(w, "");
    set_phase(w, WoprPhase::CONNECTING);
    begin_crawl(w, "LOGON ACCEPTED -- CONNECTING TO WOPR MAINFRAME...");
}

// Called when the game menu should be shown
static void show_menu(WoprState *w) {
    crawl_commit(w);
    push_line(w, "");
    push_line(w, "GREETINGS, PROFESSOR FALKEN.");
    push_line(w, "");
    push_line(w, "SHALL WE PLAY A GAME?");
    push_line(w, "");
    for (int i = 0; i < GAME_COUNT; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "  %d. %s", i + 1, GAME_LABELS[i]);
        push_line(w, buf);
    }
    push_line(w, "");
    push_line(w, "  ESC  EXIT");
    push_line(w, "");
    w->menu_sel = 0;
    set_phase(w, WoprPhase::GAME_MENU);
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
            set_phase(w, WoprPhase::PLAYING_WAR);
            wopr_war_enter(w);
            break;
        default: break;
    }
}

void wopr_update(double dt) {
    WoprState *w = &g_wopr;
    if (!w->visible) return;

    w->phase_timer += dt;

    // Advance crawl
    if (!w->crawl_target.empty() && w->crawl_pos < (int)w->crawl_target.size()) {
        w->crawl_acc += dt * w->crawl_baud / 10.0; // 300 baud ≈ 30 chars/sec
        int advance = (int)w->crawl_acc;
        if (advance > 0) {
            w->crawl_acc -= advance;
            w->crawl_pos = std::min(w->crawl_pos + advance,
                                    (int)w->crawl_target.size());
        }
    }

    switch (w->phase) {

    case WoprPhase::MODEM_SCREECH:
        w->screech_t += dt;
        if (w->screech_t >= w->screech_dur) {
            wopr_audio_stop();
            // Begin login prompt
            push_line(w, "");
            push_line(w, "MILNET NODE 347 -- WOPR AUTHENTICATION REQUIRED");
            push_line(w, "");
            set_phase(w, WoprPhase::LOGIN_PROMPT);
            begin_crawl(w, "USERNAME: ");
        }
        break;

    case WoprPhase::LOGIN_PROMPT:
        if (crawl_done(w)) {
            // Crawl done — wait for input
            set_phase(w, WoprPhase::LOGIN_INPUT);
            w->input_buf.clear();
        }
        break;

    case WoprPhase::LOGIN_INPUT:
        // Input driven by keydown — nothing to tick
        break;

    case WoprPhase::LOGIN_REJECTED:
        if (w->phase_timer > 2.0) {
            push_line(w, "");
            if (w->reject_count >= 3) {
                begin_crawl(w, "ACCESS DENIED. CONNECTION TERMINATED.");
                // After short delay, close
                // handled in next tick
            }
            set_phase(w, WoprPhase::LOGIN_PROMPT);
            w->username_done = false;
            w->input_buf.clear();
            if (w->reject_count >= 3) {
                // Brief extra pause then farewell
                w->phase_timer = -1.5;
                begin_crawl(w, "ACCESS DENIED. CONNECTION TERMINATED.");
            } else {
                begin_crawl(w, "USERNAME: ");
            }
        }
        break;

    case WoprPhase::CONNECTING:
        if (crawl_done(w) && w->phase_timer > 1.5) {
            crawl_commit(w);
            // Fake connecting lines at fast baud
            // We push them all at once for effect
            push_line(w, "");
            push_line(w, "  NORAD CHEYENNE MOUNTAIN COMPLEX");
            push_line(w, "  WARGAMES SYSTEMS DIVISION");
            push_line(w, "  CLASSIFIED -- LEVEL 5 CLEARANCE");
            push_line(w, "");
            push_line(w, "  HELLO, PROFESSOR FALKEN.");
            push_line(w, "  LAST LOGIN: 1983-06-03  03:14:07 MDT");
            push_line(w, "");
            // Delay then show menu
            set_phase(w, WoprPhase::GAME_MENU);
            w->phase_timer = -0.8; // hold a moment before menu renders
        }
        break;

    case WoprPhase::GAME_MENU:
        if (w->phase_timer < 0) break; // still waiting
        // Menu is rendered; input is handled by keydown
        break;

    case WoprPhase::PLAYING_TTT:
        wopr_ttt_update(w, dt);
        break;
    case WoprPhase::PLAYING_CHESS:
        wopr_chess_update(w, dt);
        break;
    case WoprPhase::PLAYING_MINES:
        wopr_mines_update(w, dt);
        break;
    case WoprPhase::PLAYING_MAZE:
        wopr_maze_update(w, dt);
        break;
    case WoprPhase::PLAYING_WAR:
        wopr_war_update(w, dt);
        break;

    case WoprPhase::FAREWELL:
        if (crawl_done(w) && w->phase_timer > 2.5)
            wopr_close();
        break;

    default: break;
    }
}

// ============================================================================
// RENDERING
// ============================================================================

// Colours (phosphor green on black CRT palette)
static const SDL_Color COL_BG     = {  0,   0,   0, 230};
static const SDL_Color COL_GREEN  = {  0, 255,  70, 255};
static const SDL_Color COL_DIM    = {  0, 160,  40, 255};
static const SDL_Color COL_SEL    = {  0, 255,  70, 255};
static const SDL_Color COL_BORDER = {  0, 180,  50, 255};

// Render a filled rect using SDL renderer (caller must have renderer available)
// We use the same approach as sftp_overlay: draw quads via OpenGL.
// We expose a helper that matches your project's gl_draw_rect convention.

#include "wopr_render.h"

void wopr_render(int win_w, int win_h) {
    WoprState *w = &g_wopr;
    if (!w->visible) return;

    const float PAD   = 24.f;
    const float SCALE = 1.0f;
    float ch = gl_text_height(SCALE);
    float cw = gl_text_width("M", SCALE);

    //SDL_Log("[WOPR] render win=%dx%d phase=%d ch=%.1f cw=%.1f\n", win_w, win_h, (int)w->phase, ch, cw);

    // Full-screen dark background
    gl_draw_rect(0, 0, (float)win_w, (float)win_h, 0.f, 0.f, 0.f, 0.90f);

    // CRT border
    gl_draw_rect(PAD - 4, PAD - 4,
                 win_w - 2*(PAD-4), win_h - 2*(PAD-4),
                 0.f, 0.45f, 0.12f, 1.f);
    gl_draw_rect(PAD, PAD,
                 win_w - 2*PAD, win_h - 2*PAD,
                 0.f, 0.f, 0.f, 1.f);

    float x0 = PAD + 8;
    float y0 = PAD + 8;
    float area_w = win_w - 2*(PAD + 8);
    int   vis_rows = (int)((win_h - 2*(PAD + 8)) / ch) - 2;

    // WOPR phase-specific rendering
    if (w->phase == WoprPhase::MODEM_SCREECH) {
        // Modem screech visual — scrolling hex noise + progress bar
        float prog = (float)(w->screech_t / w->screech_dur);
        prog = std::min(prog, 1.f);

        const char *title = "WOPR MODEM HANDSHAKE  300 BAUD";
        float tx = x0 + (area_w - gl_text_width(title, SCALE)) * 0.5f;
        gl_draw_text(title, tx, y0 + ch*2, 0.f, 1.f, 0.27f, 1.f, SCALE);

        // Noise lines
        srand((unsigned)(w->screech_t * 60));
        for (int row = 0; row < 10; row++) {
            char line[128] = {};
            for (int i = 0; i < 60; i++) {
                static const char *hex = "0123456789ABCDEF";
                line[i] = hex[rand() % 16];
            }
            gl_draw_text(line, x0 + cw*2, y0 + ch*(5+row),
                         0.f, 0.4f, 0.1f, 1.f, SCALE);
        }

        // Progress bar
        float bar_y = y0 + ch * 17;
        float bar_w = area_w * 0.7f;
        float bar_x = x0 + (area_w - bar_w) * 0.5f;
        gl_draw_rect(bar_x, bar_y, bar_w, ch * 1.4f, 0.f, 0.25f, 0.07f, 1.f);
        gl_draw_rect(bar_x, bar_y, bar_w * prog, ch * 1.4f, 0.f, 1.f, 0.27f, 1.f);
        const char *connecting = "ESTABLISHING CARRIER...";
        gl_draw_text(connecting,
                     bar_x + (bar_w - gl_text_width(connecting, SCALE))*0.5f,
                     bar_y + ch*0.2f,
                     0.f, 0.f, 0.f, 1.f, SCALE);
        return;
    }

    // Sub-game rendering (own full-screen layouts)
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
    if (in_subgame) return;

    // ── Terminal log scroll ──────────────────────────────────────────────
    // Decide how many committed lines fit
    int total_committed = (int)w->lines.size();
    int crawl_extra = w->crawl_target.empty() ? 0 : 1;
    int input_extra = (w->phase == WoprPhase::LOGIN_INPUT) ? 1 : 0;
    int used_rows = total_committed + crawl_extra + input_extra;
    int start_line = std::max(0, used_rows - vis_rows);

    float y = y0;
    for (int li = start_line; li < total_committed; li++) {
        gl_draw_text(w->lines[li].c_str(), x0, y, 0.f, 1.f, 0.27f, 1.f, SCALE);
        y += ch;
    }

    // Active crawl line
    if (!w->crawl_target.empty()) {
        std::string partial = w->crawl_target.substr(0, w->crawl_pos);
        gl_draw_text(partial.c_str(), x0, y, 0.f, 1.f, 0.27f, 1.f, SCALE);
        y += ch;
    }

    // Input line (login)
    if (w->phase == WoprPhase::LOGIN_INPUT) {
        std::string prompt;
        if (!w->username_done) {
            prompt = "USERNAME: " + w->input_buf;
        } else {
            // Password — show asterisks
            prompt = "PASSWORD: " + std::string(w->input_buf.size(), '*');
        }
        // Blinking cursor
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, 0.f, 1.f, 0.27f, 1.f, SCALE);
        y += ch;
    }

    // Game menu highlight
    if (w->phase == WoprPhase::GAME_MENU && w->phase_timer >= 0) {
        // Re-draw menu selection highlight over the line we care about
        // Find position of first game entry in lines[]
        // We search backwards for "  1. CHESS"
        int menu_base = -1;
        for (int li = (int)w->lines.size()-1; li >= 0; li--) {
            if (w->lines[li].find("  1. ") != std::string::npos) {
                menu_base = li; break;
            }
        }
        if (menu_base >= 0) {
            int sel_line = menu_base + w->menu_sel;
            if (sel_line >= start_line && sel_line < (int)w->lines.size()) {
                float sel_y = y0 + (sel_line - start_line) * ch;
                gl_draw_rect(x0 - 2, sel_y, area_w, ch,
                             0.f, 0.55f, 0.15f, 0.35f);
                // Re-draw the text brighter
                gl_draw_text(w->lines[sel_line].c_str(), x0, sel_y,
                             0.f, 1.f, 0.6f, 1.f, SCALE);
            }
        }
    }

    // ESC hint at bottom
    if (w->phase == WoprPhase::GAME_MENU) {
        const char *hint = "ARROWS/ENTER SELECT    ESC QUIT";
        gl_draw_text(hint,
                     x0 + (area_w - gl_text_width(hint, SCALE)) * 0.5f,
                     win_h - PAD - ch * 1.5f,
                     0.f, 0.6f, 0.2f, 1.f, SCALE);
    }

    gl_flush_verts();
}

// ============================================================================
// INPUT
// ============================================================================

// Returns true and closes the sub-game, returning to menu
static bool sub_back(WoprState *w) {
    switch (w->phase) {
        case WoprPhase::PLAYING_TTT:   wopr_ttt_free(w);   break;
        case WoprPhase::PLAYING_CHESS: wopr_chess_free(w); break;
        case WoprPhase::PLAYING_MINES: wopr_mines_free(w); break;
        case WoprPhase::PLAYING_MAZE:  wopr_maze_free(w);  break;
        case WoprPhase::PLAYING_WAR:   wopr_war_free(w);   break;
        default: return false;
    }
    show_menu(w);
    return true;
}

bool wopr_keydown(SDL_Keycode sym, const char *text) {
    WoprState *w = &g_wopr;
    if (!w->visible) return false;

    // Sub-games get first crack; they return false to bubble up (e.g. ESC)
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
        default: break;
    }

    // Global ESC always closes
    if (sym == SDLK_ESCAPE) {
        if (w->phase == WoprPhase::GAME_MENU) {
            // Farewell
            crawl_commit(w);
            push_line(w, "");
            push_line(w, "A STRANGE GAME.");
            push_line(w, "THE ONLY WINNING MOVE IS NOT TO PLAY.");
            push_line(w, "");
            push_line(w, "HOW ABOUT A NICE GAME OF CHESS?");
            set_phase(w, WoprPhase::FAREWELL);
            begin_crawl(w, "CONNECTION CLOSED.  GOODBYE, PROFESSOR FALKEN.");
        } else {
            wopr_close();
        }
        return true;
    }

    // Modem screech — any key skips it
    if (w->phase == WoprPhase::MODEM_SCREECH) {
        w->screech_t = w->screech_dur;
        return true;
    }

    // Skip crawl on ENTER/SPACE
    if ((sym == SDLK_RETURN || sym == SDLK_SPACE) &&
        (w->phase == WoprPhase::LOGIN_PROMPT || w->phase == WoprPhase::CONNECTING)) {
        w->crawl_pos = (int)w->crawl_target.size();
        return true;
    }

    // Login input
    if (w->phase == WoprPhase::LOGIN_INPUT) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (!w->username_done) {
                // Submitted username
                w->username_entered = to_upper(w->input_buf);
                push_line(w, "USERNAME: " + w->username_entered);
                w->username_done = true;
                w->input_buf.clear();
                begin_crawl(w, "PASSWORD: ");
                set_phase(w, WoprPhase::LOGIN_PROMPT);
            } else {
                // Submitted password
                w->password_entered = to_upper(w->input_buf);
                push_line(w, "PASSWORD: " + std::string(w->input_buf.size(), '*'));
                w->input_buf.clear();
                if (w->username_entered == VALID_USER &&
                    w->password_entered == VALID_PASS) {
                    begin_connecting(w);
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
        // Printable ASCII
        if (text && text[0] >= 32 && text[0] < 127 && w->input_buf.size() < 32) {
            w->input_buf += (char)toupper((unsigned char)text[0]);
            return true;
        }
        return true;
    }

    // Game menu navigation
    if (w->phase == WoprPhase::GAME_MENU && w->phase_timer >= 0) {
        if (sym == SDLK_UP || sym == SDLK_k) {
            w->menu_sel = (w->menu_sel - 1 + GAME_COUNT) % GAME_COUNT;
            return true;
        }
        if (sym == SDLK_DOWN || sym == SDLK_j) {
            w->menu_sel = (w->menu_sel + 1) % GAME_COUNT;
            return true;
        }
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_SPACE) {
            launch_game(w, GAME_IDS[w->menu_sel]);
            return true;
        }
        // Number shortcuts
        if (sym >= SDLK_1 && sym <= SDLK_1 + GAME_COUNT - 1) {
            int idx = sym - SDLK_1;
            w->menu_sel = idx;
            launch_game(w, GAME_IDS[idx]);
            return true;
        }
        return true;
    }

    return true; // consume all keys while overlay is up
}
