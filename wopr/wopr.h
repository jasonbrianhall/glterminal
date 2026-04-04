#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>

// ============================================================================
// WOPR OVERLAY  (F7)
//
// A War Games-inspired retro terminal overlay with:
//   - 300-baud simulated login sequence (FALKEN / JOSHUA)
//   - WOPR game menu
//   - Playable: Tic-Tac-Toe, Chess, Minesweeper, Falken's Maze,
//               Global Thermonuclear War, Zork
// ============================================================================

enum class WoprPhase {
    INACTIVE,
    MODEM_SCREECH,    // FSK audio plays, progress bar fills
    LOGIN_PROMPT,     // Typing USERNAME: / PASSWORD: at 300 baud
    LOGIN_INPUT,      // Waiting for user keystrokes
    LOGIN_REJECTED,   // Wrong creds → "IDENTIFICATION NOT RECOGNIZED"
    CONNECTING,       // "CONNECTING TO WOPR..." crawl
    GAME_MENU,        // Main WOPR game list / command shell
    PLAYING_TTT,
    PLAYING_CHESS,
    PLAYING_MINES,
    PLAYING_MAZE,
    PLAYING_WAR,
    PLAYING_ZORK,
    PLAYING_BASIC,
    FAREWELL,         // "A STRANGE GAME..." on exit
};

enum class WoprGame {
    NONE = 0,
    CHESS,
    FALKEN_MAZE,
    GLOBAL_WAR,
    TIC_TAC_TOE,
    MINESWEEPER,
    ZORK,
    BASIC,
    // Sentinel
    COUNT
};

// ─── Shared overlay state ──────────────────────────────────────────────────

struct WoprState {
    WoprPhase phase       = WoprPhase::INACTIVE;
    bool      visible     = false;

    // Baud-rate text crawler
    std::string  crawl_target;   // full text to reveal
    int          crawl_pos  = 0; // chars revealed so far
    double       crawl_acc  = 0; // accumulated fractional chars
    double       crawl_baud = 300.0;

    // Login state
    std::string  input_buf;      // what user has typed this field
    bool         username_done = false;
    std::string  username_entered;
    std::string  password_entered;
    int          reject_count  = 0;

    // Full terminal buffer (lines already committed)
    std::vector<std::string> lines;

    // Game menu
    int          menu_sel  = 0;

    // Modem screech
    double       screech_t = 0.0;   // elapsed seconds
    double       screech_dur = 3.5; // total screech duration

    // Phase timer (used for delays/transitions)
    double       phase_timer = 0.0;

    // Sub-game state — opaque blobs owned by sub-modules
    void        *sub_state = nullptr;
};

extern WoprState g_wopr;

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_open();
void wopr_close();

// ─── Per-frame update (call from main loop, dt in seconds) ────────────────
void wopr_update(double dt);

// ─── Render (call after terminal render, before menu) ─────────────────────
void wopr_render(int win_w, int win_h);

// ─── Input — returns true if event consumed ───────────────────────────────
bool wopr_keydown(SDL_Keycode sym, const char *text);

// ─── Audio ────────────────────────────────────────────────────────────────
bool  wopr_audio_init();
void  wopr_audio_shutdown();
void  wopr_audio_play_screech();
void  wopr_audio_stop();

// ─── Sub-game interfaces (implemented in wopr_*.cpp) ──────────────────────

// Tic-Tac-Toe
void wopr_ttt_enter(WoprState *w);
void wopr_ttt_update(WoprState *w, double dt);
void wopr_ttt_render(WoprState *w, int x, int y, int cw, int ch, int cols);
bool wopr_ttt_keydown(WoprState *w, SDL_Keycode sym);
void wopr_ttt_free(WoprState *w);

// Chess
void wopr_chess_enter(WoprState *w);
void wopr_chess_update(WoprState *w, double dt);
void wopr_chess_render(WoprState *w, int x, int y, int cw, int ch, int cols);
bool wopr_chess_keydown(WoprState *w, SDL_Keycode sym);
void wopr_chess_free(WoprState *w);

// Minesweeper
void wopr_mines_enter(WoprState *w);
void wopr_mines_update(WoprState *w, double dt);
void wopr_mines_render(WoprState *w, int x, int y, int cw, int ch, int cols);
bool wopr_mines_keydown(WoprState *w, SDL_Keycode sym);
void wopr_mines_free(WoprState *w);

// Falken's Maze
void wopr_maze_enter(WoprState *w);
void wopr_maze_update(WoprState *w, double dt);
void wopr_maze_render(WoprState *w, int x, int y, int cw, int ch, int cols);
bool wopr_maze_keydown(WoprState *w, SDL_Keycode sym);
void wopr_maze_free(WoprState *w);

// Global Thermonuclear War
void wopr_war_enter(WoprState *w);
void wopr_war_update(WoprState *w, double dt);
void wopr_war_render(WoprState *w, int x, int y, int cw, int ch, int cols);
bool wopr_war_keydown(WoprState *w, SDL_Keycode sym);
void wopr_war_free(WoprState *w);

// Zork / Dungeon
void wopr_zork_enter(WoprState *w);
void wopr_zork_update(WoprState *w, double dt);
void wopr_zork_render(WoprState *w, int x, int y, int cw, int ch, int cols);
bool wopr_zork_keydown(WoprState *w, SDL_Keycode sym);
void wopr_zork_text(WoprState *w, const char *text);
void wopr_zork_free(WoprState *w);

// BASIC
void wopr_basic_enter(WoprState *w);
void wopr_basic_update(WoprState *w, double dt);
void wopr_basic_render(WoprState *w, int x, int y, int cw, int ch, int cols);
bool wopr_basic_keydown(WoprState *w, SDL_Keycode sym);
bool wopr_basic_is_waiting_input(WoprState *w);
void wopr_basic_text(WoprState *w, const char *text);
void wopr_basic_free(WoprState *w);
