// wopr_tictactoe.cpp — WOPR Tic-Tac-Toe sub-game
//
// Player is X, WOPR is O.  Minimax AI — unbeatable unless player finds the draw.
// Controls: arrow keys / WASD to move cursor, ENTER/SPACE to place.

#include "wopr.h"
#include <string.h>
#include <limits.h>

#include "wopr_render.h"

// ─── State ────────────────────────────────────────────────────────────────

struct TttState {
    int  board[9];   // 0=empty, 1=X(player), -1=O(WOPR)
    int  cursor;     // 0-8
    int  result;     // 0=playing, 1=player wins, -1=WOPR wins, 2=draw
    bool wopr_turn;
    double ai_delay; // seconds until WOPR "thinks"
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

static int minimax(int board[9], bool maximizing) {
    int w = ttt_winner(board);
    if (w) return w * 10;
    if (ttt_full(board)) return 0;

    int best = maximizing ? INT_MIN : INT_MAX;
    for (int i = 0; i < 9; i++) {
        if (!board[i]) {
            board[i] = maximizing ? -1 : 1; // WOPR=-1 maximizes negation
            int val = minimax(board, !maximizing);
            board[i] = 0;
            if (maximizing) best = val < best ? val : best; // WOPR minimizes score (wants -1)
            else            best = val > best ? val : best;
        }
    }
    return best;
}

static int wopr_best_move(const int board[9]) {
    int best_val = INT_MAX, best_idx = -1;
    int tmp[9];
    memcpy(tmp, board, sizeof(tmp));
    for (int i = 0; i < 9; i++) {
        if (!tmp[i]) {
            tmp[i] = -1;
            int val = minimax(tmp, true); // player's turn next, maximizes
            tmp[i] = 0;
            if (val < best_val) { best_val = val; best_idx = i; }
        }
    }
    return best_idx;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_ttt_enter(WoprState *w) {
    TttState *s = new TttState{};
    memset(s->board, 0, sizeof(s->board));
    s->cursor   = 4; // center
    s->result   = 0;
    s->wopr_turn = false;
    s->ai_delay  = 0.0;
    strcpy(s->message, "YOUR MOVE, PROFESSOR.");
    w->sub_state = s;
}

void wopr_ttt_free(WoprState *w) {
    delete ttt(w);
    w->sub_state = nullptr;
}

void wopr_ttt_update(WoprState *w, double dt) {
    TttState *s = ttt(w);
    if (!s || s->result) return;

    if (s->wopr_turn) {
        s->ai_delay -= dt;
        if (s->ai_delay <= 0) {
            int mv = wopr_best_move(s->board);
            if (mv >= 0) {
                s->board[mv] = -1;
                int res = ttt_winner(s->board);
                if (res == -1) {
                    s->result = -1;
                    strcpy(s->message, "WOPR WINS.  INTERESTING GAME.");
                } else if (ttt_full(s->board)) {
                    s->result = 2;
                    strcpy(s->message, "DRAW.  SHALL WE PLAY AGAIN?");
                } else {
                    strcpy(s->message, "YOUR MOVE, PROFESSOR.");
                }
            }
            s->wopr_turn = false;
        }
    }
}

void wopr_ttt_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    TttState *s = ttt(w);
    if (!s) return;

    float scale = 1.0f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;

    // Title
    const char *title = "TIC-TAC-TOE  --  WOPR GAMES DIVISION";
    gl_draw_text(title, x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 2;

    // Board  (3x3 grid, each cell 5 chars wide x 3 lines tall)
    // Grid lines
    float cell = fcw * 6;
    float bx = x0 + fcw * 2;
    float by = y0;

    static const char *sym[3] = {"   ", " X ", " O "};
    static const char *cell_sym[] = {".", "X", "O"};

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int idx = row*3 + col;
            int val = s->board[idx]; // 0, 1, -1
            bool sel = (s->cursor == idx && !s->result && !s->wopr_turn);

            float cx = bx + col * (cell + fcw);
            float cy = by + row * (fch * 2 + fch * 0.5f);

            // Cell background
            if (sel) {
                gl_draw_rect(cx - 2, cy - 2, cell, fch + 4,
                             0.f, 0.55f, 0.15f, 0.4f);
            }

            // Cell content
            const char *glyph = (val == 0) ? "." : (val == 1) ? "X" : "O";
            float gr = (val ==  1) ? 0.3f : (val == -1) ? 1.f : 0.5f;
            float gg = (val ==  1) ? 1.f  : (val == -1) ? 0.3f : 0.8f;
            float gb = (val ==  1) ? 0.3f : (val == -1) ? 0.3f : 0.4f;
            gl_draw_text(glyph,
                         cx + (cell - gl_text_width(glyph, scale)) * 0.5f,
                         cy, gr, gg, gb, 1.f, scale);
        }

        // Divider
        if (row < 2) {
            float dy = by + (row+1) * (fch * 2 + fch * 0.5f) - fch * 0.4f;
            const char *div = "------+-------+------";
            gl_draw_text(div, bx, dy, 0.f, 0.5f, 0.15f, 1.f, scale);
        }
    }

    // Status / message
    float my = by + 3 * (fch * 2 + fch * 0.5f) + fch;
    gl_draw_text(s->message, x0, my, 0.f, 1.f, 0.6f, 1.f, scale);
    my += fch * 1.5f;

    if (s->result) {
        gl_draw_text("R=REMATCH   ESC=MENU", x0, my, 0.f, 0.7f, 0.2f, 1.f, scale);
    } else {
        gl_draw_text("ARROWS MOVE   ENTER PLACE   ESC MENU", x0, my,
                     0.f, 0.5f, 0.15f, 1.f, scale);
    }
}

bool wopr_ttt_keydown(WoprState *w, SDL_Keycode sym) {
    TttState *s = ttt(w);
    if (!s) return false;

    if (s->result) {
        if (sym == SDLK_r) {
            // Rematch
            memset(s->board, 0, sizeof(s->board));
            s->cursor   = 4;
            s->result   = 0;
            s->wopr_turn = false;
            strcpy(s->message, "YOUR MOVE, PROFESSOR.");
        }
        return true;
    }

    if (s->wopr_turn) return true; // ignore input while AI "thinks"

    int r = s->cursor / 3, c = s->cursor % 3;
    switch (sym) {
        case SDLK_UP:    case SDLK_w: r = (r+2)%3; break;
        case SDLK_DOWN:  case SDLK_s: r = (r+1)%3; break;
        case SDLK_LEFT:  case SDLK_a: c = (c+2)%3; break;
        case SDLK_RIGHT: case SDLK_d: c = (c+1)%3; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: {
            int idx = r*3 + c;
            if (s->board[idx] == 0) {
                s->board[idx] = 1;
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
                    s->ai_delay  = 0.8 + (rand() % 80) / 100.0;
                }
            }
            return true;
        }
        default: return true;
    }
    s->cursor = r*3 + c;
    return true;
}
