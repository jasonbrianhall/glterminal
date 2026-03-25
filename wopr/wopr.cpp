#include "wopr.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <cctype>
#include "wopr_render.h"

// ============================================================================
// GLOBALS
// ============================================================================

WoprState g_wopr;

// ============================================================================
// CONSTANTS
// ============================================================================

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
// MENU HELPERS
// First line index of the game list (for highlight rendering)
// ============================================================================

static int g_menu_line_start = -1; // index into w->lines where "  1. CHESS" is

static void show_menu(WoprState *w) {
    crawl_commit(w);
    push_line(w, "");
    push_line(w, "GREETINGS, PROFESSOR FALKEN.");
    push_line(w, "");
    push_line(w, "SHALL WE PLAY A GAME?");
    push_line(w, "");
    g_menu_line_start = (int)w->lines.size(); // record where entries start
    for (int i = 0; i < GAME_COUNT; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "  %d. %s", i + 1, GAME_LABELS[i]);
        push_line(w, buf);
    }
    push_line(w, "");
    push_line(w, "  ESC  EXIT");
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

// ============================================================================
// OPEN / CLOSE
// ============================================================================

void wopr_open() {
    WoprState *w = &g_wopr;
    *w = WoprState{};
    w->visible = true;
    w->lines.reserve(64);
    g_menu_line_start = -1;

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
            show_menu(w);  // sets phase to GAME_MENU — won't fire again
        }
        break;

    case WoprPhase::GAME_MENU:
        break;

    case WoprPhase::PLAYING_TTT:   wopr_ttt_update(w, dt);   break;
    case WoprPhase::PLAYING_CHESS: wopr_chess_update(w, dt); break;
    case WoprPhase::PLAYING_MINES: wopr_mines_update(w, dt); break;
    case WoprPhase::PLAYING_MAZE:  wopr_maze_update(w, dt);  break;
    case WoprPhase::PLAYING_WAR:   wopr_war_update(w, dt);   break;

    case WoprPhase::FAREWELL:
        if (crawl_done(w) && w->phase_timer > 2.0)
            wopr_close();
        break;

    default: break;
    }
}

// ============================================================================
// RENDERING
// ============================================================================

void wopr_render(int win_w, int win_h) {
    WoprState *w = &g_wopr;
    if (!w->visible) return;

    const float PAD   = 24.f;
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

    // Sub-game rendering
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
    int total = (int)w->lines.size();
    int crawl_extra = w->crawl_target.empty() ? 0 : 1;
    int input_extra = (w->phase == WoprPhase::LOGIN_INPUT) ? 1 : 0;
    int used = total + crawl_extra + input_extra;
    int start_line = std::max(0, used - vis_rows);

    float y = y0;
    for (int li = start_line; li < total; li++) {
        // Highlight selected menu item
        if (w->phase == WoprPhase::GAME_MENU &&
            g_menu_line_start >= 0 &&
            li >= g_menu_line_start &&
            li < g_menu_line_start + GAME_COUNT) {
            int idx = li - g_menu_line_start;
            if (idx == w->menu_sel) {
                gl_draw_rect(x0 - 2, y, area_w, ch,
                             0.f, 0.45f, 0.12f, 0.4f);
                gl_draw_text(w->lines[li].c_str(), x0, y,
                             0.f, 1.f, 0.5f, 1.f, SCALE);
                y += ch;
                continue;
            }
        }
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
        if (!w->username_done)
            prompt = "USERNAME: " + w->input_buf;
        else
            prompt = "PASSWORD: " + std::string(w->input_buf.size(), '*');
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) prompt += '_';
        gl_draw_text(prompt.c_str(), x0, y, 0.f, 1.f, 0.27f, 1.f, SCALE);
    }

    // Controls hint at bottom
    if (w->phase == WoprPhase::GAME_MENU) {
        const char *hint = "ARROWS/ENTER SELECT    ESC QUIT";
        gl_draw_text(hint,
                     x0 + (area_w - gl_text_width(hint, SCALE)) * 0.5f,
                     win_h - PAD - ch * 1.5f,
                     0.f, 0.5f, 0.15f, 1.f, SCALE);
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
        default: return false;
    }
    // Don't call show_menu — just go back to GAME_MENU phase
    // The lines are already in the buffer from before
    set_phase(w, WoprPhase::GAME_MENU);
    w->menu_sel = 0;
    return true;
}

bool wopr_keydown(SDL_Keycode sym, const char *text) {
    WoprState *w = &g_wopr;
    if (!w->visible) return false;

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
        default: break;
    }

    // ESC
    if (sym == SDLK_ESCAPE) {
        if (w->phase == WoprPhase::GAME_MENU) {
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

    // Speed up crawl on any key during login prompts
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

    // Game menu
    if (w->phase == WoprPhase::GAME_MENU) {
        if (sym == SDLK_UP || sym == SDLK_w) {
            w->menu_sel = (w->menu_sel - 1 + GAME_COUNT) % GAME_COUNT;
            return true;
        }
        if (sym == SDLK_DOWN || sym == SDLK_s) {
            w->menu_sel = (w->menu_sel + 1) % GAME_COUNT;
            return true;
        }
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_SPACE) {
            launch_game(w, GAME_IDS[w->menu_sel]);
            return true;
        }
        if (sym >= SDLK_1 && sym <= SDLK_1 + GAME_COUNT - 1) {
            int idx = sym - SDLK_1;
            w->menu_sel = idx;
            launch_game(w, GAME_IDS[idx]);
            return true;
        }
        return true;
    }

    return true;
}
