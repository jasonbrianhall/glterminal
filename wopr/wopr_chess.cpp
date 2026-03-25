// wopr_chess.cpp — WOPR Chess sub-game
//
// Wraps beatchess engine with ASCII board rendering.
// Player = WHITE.  WOPR = BLACK.  Depth 4 minimax.
// Controls: type moves in coordinate notation  e2e4 / e7e5 / etc.
// Or use arrow keys to select a piece then arrow + enter to move.

#include "wopr.h"
#include "beatchess.h"
#include "chess_ai_move.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "wopr_render.h"

// ─── State ────────────────────────────────────────────────────────────────

struct WoprChessState {
    ChessGameState game;
    ChessGameStatus status;

    // Cursor-based input
    int  sel_r, sel_c;          // board cursor
    bool has_from;
    int  from_r, from_c;

    // AI threading
    bool          ai_thinking;
    pthread_t     ai_thread;
    ChessMove     ai_result;
    bool          ai_done;

    char message[128];
    bool game_over;
};

struct AiArg {
    ChessGameState   game;
    ChessMove       *out;
    bool            *done;
};

static void *ai_worker(void *arg) {
    AiArg *a = (AiArg *)arg;
    ChessAIConfig cfg = chess_ai_get_default_config();
    cfg.search_depth  = 4;
    ChessAIMoveResult r = chess_ai_compute_move(&a->game, cfg);
    *a->out  = r.move;
    *a->done = true;
    delete a;
    return nullptr;
}

static WoprChessState *cs(WoprState *w) { return (WoprChessState *)w->sub_state; }

// ─── Helpers ──────────────────────────────────────────────────────────────

static const char *piece_glyph(const ChessPiece &p) {
    if (p.color == NONE) return ".";
    static const char *wg[] = {"?","P","N","B","R","Q","K"};
    static const char *bg[] = {"?","p","n","b","r","q","k"};
    return (p.color == WHITE) ? wg[p.type] : bg[p.type];
}

static void start_ai(WoprChessState *s) {
    s->ai_thinking = true;
    s->ai_done     = false;
    AiArg *arg     = new AiArg;
    arg->game      = s->game;
    arg->out       = &s->ai_result;
    arg->done      = &s->ai_done;
    pthread_create(&s->ai_thread, nullptr, ai_worker, arg);
    pthread_detach(s->ai_thread);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_chess_enter(WoprState *w) {
    WoprChessState *s = new WoprChessState{};
    chess_init_board(&s->game);
    chess_init_zobrist();
    s->status      = CHESS_PLAYING;
    s->sel_r = 7; s->sel_c = 4;
    s->has_from    = false;
    s->ai_thinking = false;
    s->ai_done     = false;
    s->game_over   = false;
    strcpy(s->message, "YOUR MOVE.  WHITE TO PLAY.");
    w->sub_state = s;
}

void wopr_chess_free(WoprState *w) {
    WoprChessState *s = cs(w);
    if (!s) return;
    // Let the AI thread finish on its own (it's detached)
    delete s;
    w->sub_state = nullptr;
}

void wopr_chess_update(WoprState *w, double dt) {
    WoprChessState *s = cs(w);
    if (!s || s->game_over) return;

    (void)dt;

    if (s->ai_thinking && s->ai_done) {
        s->ai_thinking = false;
        ChessMove mv = s->ai_result;
        if (mv.from_row >= 0) {
            chess_make_move(&s->game, mv);
            s->status = chess_check_game_status(&s->game);
            if (s->status == CHESS_CHECKMATE_WHITE) {
                strcpy(s->message, "CHECKMATE.  WOPR WINS.  FASCINATING.");
                s->game_over = true;
            } else if (s->status == CHESS_STALEMATE) {
                strcpy(s->message, "STALEMATE.  A DRAW.");
                s->game_over = true;
            } else {
                bool chk = chess_is_in_check(&s->game, WHITE);
                if (chk) strcpy(s->message, "CHECK!  YOUR MOVE.");
                else     strcpy(s->message, "YOUR MOVE.");
            }
        } else {
            strcpy(s->message, "WOPR HAS NO LEGAL MOVES.");
            s->game_over = true;
        }
    }
}

void wopr_chess_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WoprChessState *s = cs(w);
    if (!s) return;

    float scale = 1.f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;

    // Title
    const char *title = "CHESS  --  WOPR GAMES DIVISION  (YOU=WHITE  WOPR=BLACK)";
    gl_draw_text(title, x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 1.5f;

    // Board
    // Each cell: 3 chars wide, 1 line tall
    float cell_w = fcw * 4;
    float cell_h = fch * 1.2f;
    float bx = x0 + fcw * 4;  // leave room for rank labels
    float by = y0;

    for (int row = 0; row < 8; row++) {
        // Rank label
        char rl[4]; snprintf(rl, sizeof(rl), "%d ", 8 - row);
        gl_draw_text(rl, bx - fcw * 3, by + row * cell_h,
                     0.f, 0.6f, 0.2f, 1.f, scale);

        for (int col = 0; col < 8; col++) {
            float cx = bx + col * cell_w;
            float cy = by + row * cell_h;

            bool light_sq  = ((row + col) % 2 == 0);
            bool is_cursor = (s->sel_r == row && s->sel_c == col);
            bool is_from   = (s->has_from && s->from_r == row && s->from_c == col);

            // Square background
            float br = light_sq ? 0.05f : 0.0f;
            float bg = light_sq ? 0.22f : 0.08f;
            float bb = light_sq ? 0.05f : 0.0f;
            if (is_from)   { br = 0.4f; bg = 0.4f; bb = 0.0f; }
            if (is_cursor) { br = 0.0f; bg = 0.6f; bb = 0.2f; }
            gl_draw_rect(cx, cy, cell_w - 1, cell_h - 1, br, bg, bb, 0.9f);

            // Piece
            const ChessPiece &p = s->game.board[row][col];
            if (p.type != EMPTY) {
                const char *g = piece_glyph(p);
                float pr = (p.color == WHITE) ? 0.9f : 0.5f;
                float pg = (p.color == WHITE) ? 1.0f : 0.8f;
                float pb = (p.color == WHITE) ? 0.9f : 0.5f;
                gl_draw_text(g,
                             cx + (cell_w - gl_text_width(g, scale)) * 0.5f,
                             cy, pr, pg, pb, 1.f, scale);
            }
        }
    }

    // File labels
    const char *files = "a b c d e f g h";
    // individual labels
    for (int col = 0; col < 8; col++) {
        char fl[2] = { (char)('a' + col), 0 };
        gl_draw_text(fl, bx + col * cell_w + (cell_w - fcw) * 0.5f,
                     by + 8 * cell_h + 2,
                     0.f, 0.6f, 0.2f, 1.f, scale);
    }

    // Message + controls
    float my = by + 9 * cell_h + fch * 0.5f;
    gl_draw_text(s->message, x0, my, 0.f, 1.f, 0.5f, 1.f, scale);
    my += fch * 1.5f;

    if (!s->game_over && !s->ai_thinking) {
        gl_draw_text("ARROWS=MOVE  ENTER=SELECT/PLACE  U=UNDO  R=RESET  ESC=MENU",
                     x0, my, 0.f, 0.5f, 0.15f, 1.f, scale);
    } else if (s->ai_thinking) {
        // Blinking "thinking" indicator
        if ((SDL_GetTicks() / 400) % 2 == 0)
            gl_draw_text("WOPR IS THINKING...", x0, my, 0.f, 1.f, 0.3f, 1.f, scale);
    } else {
        gl_draw_text("R=NEW GAME   ESC=MENU", x0, my, 0.f, 0.5f, 0.15f, 1.f, scale);
    }
}

bool wopr_chess_keydown(WoprState *w, SDL_Keycode sym) {
    WoprChessState *s = cs(w);
    if (!s) return false;
    if (s->ai_thinking) return true;

    if (s->game_over || s->status != CHESS_PLAYING) {
        if (sym == SDLK_r) {
            chess_init_board(&s->game);
            s->status    = CHESS_PLAYING;
            s->has_from  = false;
            s->game_over = false;
            s->ai_done   = false;
            strcpy(s->message, "YOUR MOVE.  WHITE TO PLAY.");
        }
        return true;
    }

    // Only allow moves when it's WHITE's turn
    if (s->game.turn != WHITE) return true;

    switch (sym) {
        case SDLK_UP:    s->sel_r = std::max(0, s->sel_r - 1); break;
        case SDLK_DOWN:  s->sel_r = std::min(7, s->sel_r + 1); break;
        case SDLK_LEFT:  s->sel_c = std::max(0, s->sel_c - 1); break;
        case SDLK_RIGHT: s->sel_c = std::min(7, s->sel_c + 1); break;

        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: {
            int r = s->sel_r, c = s->sel_c;
            if (!s->has_from) {
                // Select piece
                const ChessPiece &p = s->game.board[r][c];
                if (p.type != EMPTY && p.color == WHITE) {
                    s->has_from = true;
                    s->from_r = r; s->from_c = c;
                    strcpy(s->message, "PIECE SELECTED.  CHOOSE DESTINATION.");
                }
            } else {
                // Attempt move
                if (chess_is_valid_move(&s->game, s->from_r, s->from_c, r, c)) {
                    ChessMove mv{s->from_r, s->from_c, r, c, 0};
                    chess_make_move(&s->game, mv);
                    s->has_from = false;
                    s->status = chess_check_game_status(&s->game);
                    if (s->status == CHESS_CHECKMATE_BLACK) {
                        strcpy(s->message, "CHECKMATE!  YOU WIN.  UNEXPECTED OUTCOME.");
                        s->game_over = true;
                    } else if (s->status == CHESS_STALEMATE) {
                        strcpy(s->message, "STALEMATE.  DRAW.");
                        s->game_over = true;
                    } else {
                        strcpy(s->message, "WOPR CALCULATING...");
                        start_ai(s);
                    }
                } else {
                    // Reselect if clicking own piece
                    const ChessPiece &p = s->game.board[r][c];
                    if (p.type != EMPTY && p.color == WHITE) {
                        s->from_r = r; s->from_c = c;
                        strcpy(s->message, "PIECE SELECTED.");
                    } else {
                        s->has_from = false;
                        strcpy(s->message, "INVALID MOVE.");
                    }
                }
            }
            break;
        }

        case SDLK_u: {
            // Simple undo: can't easily undo with the engine so just note it
            strcpy(s->message, "UNDO NOT AVAILABLE IN WOPR MODE.");
            break;
        }

        case SDLK_r: {
            chess_init_board(&s->game);
            s->status = CHESS_PLAYING;
            s->has_from = false;
            s->game_over = false;
            strcpy(s->message, "NEW GAME.  YOUR MOVE.");
            break;
        }

        default: break;
    }
    return true;
}
