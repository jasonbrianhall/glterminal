// wopr_tictactoe.cpp — WOPR Tic-Tac-Toe sub-game
//
// Player is X, WOPR is O.  Minimax AI — unbeatable unless player finds the draw.
// Controls: arrow keys / WASD to move cursor, ENTER/SPACE to place.
// 0-player mode: WOPR X vs WOPR O, auto-plays with think delays.

#include "wopr.h"
#include <string.h>
#include <limits.h>
#include <time.h>
#include "wopr_render.h"

// ─── Screen / mode ────────────────────────────────────────────────────────

enum TttScreen { TTT_LOBBY, TTT_GAME };

// ─── State ────────────────────────────────────────────────────────────────

struct TttState {
    TttScreen screen;
    int       lobby_sel;   // 0 = 0 players, 1 = 1 player

    int  board[9];   // 0=empty, 1=X, -1=O
    int  cursor;     // 0-8
    int  result;     // 0=playing, 1=X wins, -1=O wins, 2=draw
    bool wopr_turn;  // true when AI (O in 1p; alternating in 0p) is "thinking"
    int  num_players;
    double ai_delay;
    double min_think_ms;  // decays each game in 0-player mode
    int    game_count;
    int    stop_at_game;
    char message[64];
};

static TttState *ttt(WoprState *w) { return (TttState *)w->sub_state; }

// ─── Minimax ──────────────────────────────────────────────────────────────

static int ttt_winner(const int board[9]) {
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},
        {0,3,6},{1,4,7},{2,5,8},
        {0,4,8},{2,4,6}
    };
    for (auto &l : lines) {
        int s = board[l[0]] + board[l[1]] + board[l[2]];
        if (s ==  3) return  1;
        if (s == -3) return -1;
    }
    return 0;
}

static bool ttt_full(const int board[9]) {
    for (int i = 0; i < 9; i++) if (!board[i]) return false;
    return true;
}

// Standard negamax: positive = good for the side whose turn it is.
// 'side' is the side to move: 1=X, -1=O.
static int minimax(int board[9], int side, int depth) {
    int w = ttt_winner(board);
    if (w ==  1) return  (10 - depth) * 1;   // X won
    if (w == -1) return -(10 - depth) * 1;   // O won — bad for current side if side==1
    // Restate: always return score from X's perspective, caller adjusts
    if (w) return w * (10 - depth);
    if (ttt_full(board)) return 0;

    int best = INT_MIN;
    for (int i = 0; i < 9; i++) {
        if (!board[i]) {
            board[i] = side;
            // Score from X's perspective
            int val = -minimax(board, -side, depth + 1) * side * side;
            // Actually let's just keep scores in X's frame:
            board[i] = 0;
            (void)val;
        }
    }
    // Redo cleanly — scores always in X's frame:
    (void)best;
    return 0; // placeholder; real impl below
}

// Clean implementation: scores always from X's perspective (X=+, O=-).
static int minimax_x(int board[9], int side, int depth) {
    int w = ttt_winner(board);
    if (w) return w * (10 - depth);
    if (ttt_full(board)) return 0;

    // side==1 means X to move (maximizer), side==-1 means O to move (minimizer)
    int best = (side == 1) ? INT_MIN : INT_MAX;
    for (int i = 0; i < 9; i++) {
        if (!board[i]) {
            board[i] = side;
            int val = minimax_x(board, -side, depth + 1);
            board[i] = 0;
            if (side == 1) best = val > best ? val : best;
            else           best = val < best ? val : best;
        }
    }
    return best;
}

// Returns best move for 'side' (1=X, -1=O). Ties broken via /dev/urandom.
static int ai_best_move(const int board[9], int side) {
    int tmp[9];
    memcpy(tmp, board, sizeof(tmp));

    int best_val = (side == 1) ? INT_MIN : INT_MAX;
    int candidates[9];
    int ncand = 0;

    for (int i = 0; i < 9; i++) {
        if (!tmp[i]) {
            tmp[i] = side;
            int val = minimax_x(tmp, -side, 1);
            tmp[i] = 0;

            bool better = (side == 1) ? (val > best_val) : (val < best_val);
            bool equal  = (val == best_val);

            if (better) {
                best_val  = val;
                ncand     = 0;
                candidates[ncand++] = i;
            } else if (equal) {
                candidates[ncand++] = i;
            }
        }
    }

    if (ncand == 0) return -1;
    if (ncand == 1) return candidates[0];
    return candidates[rand() % (unsigned)ncand];
}

// ─── Game helpers ─────────────────────────────────────────────────────────

static void reset_game(TttState *s) {
    memset(s->board, 0, sizeof(s->board));
    s->cursor    = 4;
    s->result    = 0;
    s->wopr_turn = false;
    s->ai_delay  = 0.0;

    if (s->num_players == 0) {
        s->game_count++;
        // Decay: 20% faster each game, floor at 50ms then snap to 0
        s->min_think_ms *= 0.80;
        if (s->min_think_ms < 50.0) s->min_think_ms = 0.0;

        if (s->game_count >= s->stop_at_game) {
            s->result    = 2;  // freeze board
            s->wopr_turn = false;
            strcpy(s->message, "A STRANGE GAME.  THE ONLY WINNING MOVE IS NOT TO PLAY.");
            return;
        }

        strcpy(s->message, "WOPR X VS WOPR O.  X CALCULATING...");
        s->wopr_turn = true;
        s->ai_delay  = s->min_think_ms / 1000.0 + (rand() % 40) / 1000.0;
    } else {
        strcpy(s->message, "YOUR MOVE, PROFESSOR.");
    }
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_ttt_enter(WoprState *w) {
    srand((unsigned)time(nullptr));
    TttState *s = new TttState{};
    s->screen       = TTT_LOBBY;
    s->lobby_sel    = 1;
    s->min_think_ms = 800.0;  // resets only when re-entering from menu
    s->game_count   = 0;
    s->stop_at_game = 50 + rand() % 51;  // 50–100
    w->sub_state = s;
}

void wopr_ttt_free(WoprState *w) {
    delete ttt(w);
    w->sub_state = nullptr;
}

void wopr_ttt_update(WoprState *w, double dt) {
    TttState *s = ttt(w);
    if (!s || s->screen != TTT_GAME) return;

    // 0-player: auto-restart after terminal state pause
    if (s->result && s->num_players == 0 && s->wopr_turn) {
        s->ai_delay -= dt;
        if (s->ai_delay <= 0) reset_game(s);
        return;
    }

    if (s->result) return;

    if (s->wopr_turn) {
        s->ai_delay -= dt;
        if (s->ai_delay <= 0) {
            // Determine which side is moving
            // Count pieces: X has 1s, O has -1s
            int nx = 0, no_ = 0;
            for (int i = 0; i < 9; i++) {
                if (s->board[i] ==  1) nx++;
                if (s->board[i] == -1) no_++;
            }
            // X always moves first; if nx == no_, it's X's turn, else O's
            int side = (nx == no_) ? 1 : -1;

            int mv = ai_best_move(s->board, side);
            if (mv >= 0) {
                s->board[mv] = side;
                int res = ttt_winner(s->board);
                if (res) {
                    s->result = res;
                    if (s->num_players == 0) {
                        // Brief pause then auto-restart
                        s->wopr_turn = true;
                        s->ai_delay  = (s->min_think_ms > 0.0)
                                       ? s->min_think_ms / 1000.0 * 2.0
                                       : 0.0;
                        const char *winner = (res == 1) ? "X" : "O";
                        char buf[64];
                        snprintf(buf, sizeof(buf), "WOPR %s WINS.  RESTARTING...", winner);
                        strcpy(s->message, buf);
                    } else {
                        strcpy(s->message, (res == -1)
                            ? "WOPR WINS.  INTERESTING GAME."
                            : "YOU WIN.  UNEXPECTED OUTCOME.");
                    }
                } else if (ttt_full(s->board)) {
                    s->result = 2;
                    if (s->num_players == 0) {
                        s->wopr_turn = true;
                        s->ai_delay  = (s->min_think_ms > 0.0)
                                       ? s->min_think_ms / 1000.0 * 2.0
                                       : 0.0;
                        strcpy(s->message, "DRAW.  RESTARTING...");
                    } else {
                        strcpy(s->message, "DRAW.  SHALL WE PLAY AGAIN?");
                    }
                } else {
                    if (s->num_players == 0) {
                        // Other AI's turn now
                        int next_side = (side == 1) ? -1 : 1;
                        const char *nm = (next_side == 1) ? "X" : "O";
                        char buf[64];
                        snprintf(buf, sizeof(buf), "WOPR %s CALCULATING...", nm);
                        strcpy(s->message, buf);
                        s->ai_delay = s->min_think_ms / 1000.0 + (rand() % 40) / 1000.0;
                        // stay wopr_turn = true
                    } else {
                        strcpy(s->message, "YOUR MOVE, PROFESSOR.");
                        s->wopr_turn = false;
                    }
                }
            } else {
                s->wopr_turn = false;
            }
        }
    }
}

void wopr_ttt_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    TttState *s = ttt(w);
    if (!s) return;

    float scale = 1.0f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;

    // ── Lobby ─────────────────────────────────────────────────────────────
    if (s->screen == TTT_LOBBY) {
        gl_draw_text("TIC-TAC-TOE  --  WOPR GAMES DIVISION",
                     x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
        y0 += fch * 3.f;

        gl_draw_text("HOW MANY PLAYERS?",
                     x0, y0, 0.f, 1.f, 0.5f, 1.f, scale);
        y0 += fch * 3.f;

        const char *opts[2] = { "0  --  WOPR VS WOPR", "1  --  PLAYER VS WOPR" };
        for (int i = 0; i < 2; i++) {
            bool sel = (s->lobby_sel == i);
            float g = sel ? 1.0f : 0.4f;
            float b = sel ? 0.4f : 0.1f;
            if (sel) gl_draw_text(">", x0, y0 + i * fch * 2.5f, 0.f, 1.f, 0.4f, 1.f, scale);
            gl_draw_text(opts[i], x0 + fcw * 3, y0 + i * fch * 2.5f, 0.f, g, b, 1.f, scale);
        }

        y0 += fch * 7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER=START  ESC=MENU",
                     x0, y0, 0.f, 0.3f, 0.1f, 1.f, scale);
        return;
    }

    // ── Game ──────────────────────────────────────────────────────────────
    const char *title = (s->num_players == 0)
        ? "TIC-TAC-TOE  --  WOPR VS WOPR"
        : "TIC-TAC-TOE  --  WOPR GAMES DIVISION";
    gl_draw_text(title, x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 2;

    float cell_w = fcw * 6;
    float cell_h = fch * 2.5f;
    float bx = x0 + fcw * 2;
    float by = y0;

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int idx = row*3 + col;
            int val = s->board[idx];
            bool sel = (s->num_players == 1 && s->cursor == idx
                        && !s->result && !s->wopr_turn);

            float cx = bx + col * (cell_w + fcw);
            float cy = by + row * cell_h;

            float bg = sel ? 0.5f : 0.08f;
            float bb = sel ? 0.15f : 0.0f;
            gl_draw_rect(cx, cy, cell_w - 1, cell_h - 1, 0.f, bg, bb, 0.9f);

            const char *glyph = (val == 0) ? "." : (val == 1) ? "X" : "O";
            float gr = (val ==  1) ? 0.3f : (val == -1) ? 1.f  : 0.f;
            float gg = (val ==  1) ? 1.f  : (val == -1) ? 0.3f : 0.5f;
            float gb = (val ==  1) ? 0.3f : (val == -1) ? 0.3f : 0.15f;
            if (sel) { gr = 0.f; gg = 1.f; gb = 0.5f; }

            float tx = cx;
            float ty = cy + cell_h * 0.6f;
            gl_draw_text(glyph, tx, ty, gr, gg, gb, 1.f, scale);
        }

        if (row < 2) {
            float dy = by + (row + 1) * cell_h - fch * 0.4f;
            gl_draw_text("------+-------+------", bx, dy, 0.f, 0.5f, 0.15f, 1.f, scale);
        }
    }

    float my = by + 3 * cell_h + fch;
    gl_draw_text(s->message, x0, my, 0.f, 1.f, 0.6f, 1.f, scale);
    my += fch * 1.5f;

    if (s->result) {
        gl_draw_text("R=REMATCH   ESC=MENU", x0, my, 0.f, 0.7f, 0.2f, 1.f, scale);
    } else if (s->num_players == 0) {
        // spectating — no controls to show
        if ((SDL_GetTicks() / 500) % 2 == 0)
            gl_draw_text("R=LOBBY   ESC=MENU", x0, my, 0.f, 0.3f, 0.1f, 1.f, scale);
    } else {
        gl_draw_text("ARROWS MOVE   ENTER PLACE   ESC MENU", x0, my,
                     0.f, 0.5f, 0.15f, 1.f, scale);
    }
}

bool wopr_ttt_keydown(WoprState *w, SDL_Keycode sym) {
    TttState *s = ttt(w);
    if (!s) return false;

    // ── Lobby ─────────────────────────────────────────────────────────────
    if (s->screen == TTT_LOBBY) {
        switch (sym) {
            case SDLK_UP:   case SDLK_w: s->lobby_sel = 0; break;
            case SDLK_DOWN: case SDLK_s: s->lobby_sel = 1; break;
            case SDLK_0: s->lobby_sel = 0; break;
            case SDLK_1: s->lobby_sel = 1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                s->num_players = s->lobby_sel;
                s->screen = TTT_GAME;
                reset_game(s);
                break;
            default: break;
        }
        return true;
    }

    // ── Game ──────────────────────────────────────────────────────────────
    if (sym == SDLK_r) {
        s->screen = TTT_LOBBY;
        return true;
    }

    if (s->result) {
        if (sym == SDLK_r || sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_SPACE) {
            reset_game(s);
        }
        return true;
    }

    // 0-player: spectate
    if (s->num_players == 0) return true;

    if (s->wopr_turn) return true;

    int r = s->cursor / 3, c = s->cursor % 3;
    switch (sym) {
        case SDLK_UP:    case SDLK_w: r = (r+2)%3; break;
        case SDLK_DOWN:  case SDLK_s: r = (r+1)%3; break;
        case SDLK_LEFT:  case SDLK_a: c = (c+2)%3; break;
        case SDLK_RIGHT: case SDLK_d: c = (c+1)%3; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: {
            int idx = r*3 + c;
            if (s->board[idx] == 0) {
                s->board[idx] = 1; // player is X
                int res = ttt_winner(s->board);
                if (res == 1) {
                    s->result = 1;
                    strcpy(s->message, "YOU WIN.  UNEXPECTED OUTCOME.");
                } else if (ttt_full(s->board)) {
                    s->result = 2;
                    strcpy(s->message, "DRAW.  SHALL WE PLAY AGAIN?");
                } else {
                    strcpy(s->message, "WOPR CALCULATING...");
                    s->wopr_turn = true;
                    s->ai_delay  = 0.6 + (rand() % 80) / 1000.0;
                }
            }
            return true;
        }
        default: return true;
    }
    s->cursor = r*3 + c;
    return true;
}
