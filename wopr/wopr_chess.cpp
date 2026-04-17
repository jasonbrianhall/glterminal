// wopr_chess.cpp — WOPR Chess sub-game
// Player = WHITE.  WOPR = BLACK.  Depth 5 minimax.
// Controls: arrow keys to move cursor, ENTER to select/place.

#include "wopr.h"
#include "beatchess.h"
#include "chess_ai_move.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "wopr_render.h"

// ─── State ────────────────────────────────────────────────────────────────

enum ChessScreen { SCREEN_LOBBY, SCREEN_GAME };

struct WoprChessState {
    ChessScreen     screen;
    int             num_players;   // 0 or 1
    int             lobby_sel;     // 0 = "0 PLAYERS", 1 = "1 PLAYER"

    ChessGameState  game;
    ChessGameStatus status;
    int  sel_r, sel_c;
    bool has_from;
    int  from_r, from_c;
    bool ai_thinking;
    pthread_t     ai_thread;
    ChessMove     ai_result;
    bool          ai_done;
    char message[128];
    bool game_over;

    int    move_count;      // total half-moves played (0-player mode)
    double min_think_ms;    // current minimum think delay in ms

    // Board geometry — set each frame by wopr_chess_render, used by mouse hit-test
    float  board_bx, board_by;     // top-left pixel of the board grid
    float  board_cell_w, board_cell_h;

    // Mouse hover (1-player mode): row/col the cursor is over, or -1
    int    hover_r, hover_c;
};

struct AiArg {
    ChessGameState  game;
    ChessMove      *out;
    bool           *done;
    double          min_think_ms;  // delay floor for this move
};

static void *ai_worker(void *arg) {
    AiArg *a = (AiArg *)arg;
    ChessAIConfig cfg = chess_ai_get_default_config();
    cfg.search_depth  = 5;
    cfg.min_think_ms  = a->min_think_ms;
    ChessAIMoveResult r = chess_ai_compute_move(&a->game, cfg);
    *a->out  = r.move;
    *a->done = true;
    delete a;
    return nullptr;
}

static WoprChessState *cs(WoprState *w) { return (WoprChessState *)w->sub_state; }

// ─── Piece glyphs ─────────────────────────────────────────────────────────

static const char *piece_glyph(const ChessPiece &p) {
    if (p.color == NONE) return " ";
    if (p.color == WHITE) {
        switch (p.type) {
            case KING:   return "\xe2\x99\x94"; // ♔
            case QUEEN:  return "\xe2\x99\x95"; // ♕
            case ROOK:   return "\xe2\x99\x96"; // ♖
            case BISHOP: return "\xe2\x99\x97"; // ♗
            case KNIGHT: return "\xe2\x99\x98"; // ♘
            case PAWN:   return "\xe2\x99\x99"; // ♙
            default:     return "?";
        }
    } else {
        switch (p.type) {
            case KING:   return "\xe2\x99\x9a"; // ♚
            case QUEEN:  return "\xe2\x99\x9b"; // ♛
            case ROOK:   return "\xe2\x99\x9c"; // ♜
            case BISHOP: return "\xe2\x99\x9d"; // ♝
            case KNIGHT: return "\xe2\x99\x9e"; // ♞
            case PAWN:   return "\xe2\x99\x9f"; // ♟
            default:     return "?";
        }
    }
}

// ─── AI ───────────────────────────────────────────────────────────────────

static void start_ai(WoprChessState *s) {
    s->ai_thinking = true;
    s->ai_done     = false;
    AiArg *arg       = new AiArg;
    arg->game        = s->game;
    arg->out         = &s->ai_result;
    arg->done        = &s->ai_done;
    arg->min_think_ms = s->min_think_ms;
    pthread_create(&s->ai_thread, nullptr, ai_worker, arg);
    pthread_detach(s->ai_thread);
}

static void start_game(WoprChessState *s) {
    chess_init_board(&s->game);
    chess_init_zobrist();
    s->status      = CHESS_PLAYING;
    s->sel_r = 7; s->sel_c = 4;
    s->has_from    = false;
    s->ai_thinking = false;
    s->ai_done     = false;
    s->game_over   = false;
    s->screen      = SCREEN_GAME;
    s->move_count   = 0;

    if (s->num_players == 0) {
        strcpy(s->message, "WOPR VS WOPR.  WHITE CALCULATING...");
        start_ai(s);
    } else {
        strcpy(s->message, "YOUR MOVE.  WHITE TO PLAY.");
    }
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_chess_enter(WoprState *w) {
    WoprChessState *s = new WoprChessState{};
    s->screen       = SCREEN_LOBBY;
    s->lobby_sel    = 1;
    s->min_think_ms = 1500.0;  // resets only when re-entering from menu
    s->board_bx = s->board_by = s->board_cell_w = s->board_cell_h = 0.f;
    s->hover_r = s->hover_c = -1;
    w->sub_state = s;
}

void wopr_chess_free(WoprState *w) {
    WoprChessState *s = cs(w);
    if (!s) return;
    delete s;
    w->sub_state = nullptr;
}

void wopr_chess_update(WoprState *w, double dt) {
    WoprChessState *s = cs(w);
    if (!s || s->screen != SCREEN_GAME || s->game_over) return;
    (void)dt;

    if (s->ai_thinking && s->ai_done) {
        s->ai_thinking = false;
        ChessMove mv = s->ai_result;
        if (mv.from_row >= 0) {
            chess_make_move(&s->game, mv);
            s->status = chess_check_game_status(&s->game);

            if (s->status == CHESS_CHECKMATE_WHITE) {
                if (s->num_players == 0) { start_game(s); return; }
                strcpy(s->message, "CHECKMATE.  WOPR WINS.");
                s->game_over = true;
            } else if (s->status == CHESS_CHECKMATE_BLACK) {
                if (s->num_players == 0) { start_game(s); return; }
                strcpy(s->message, "CHECKMATE!  YOU WIN.");
                s->game_over = true;
            } else if (s->status == CHESS_STALEMATE) {
                if (s->num_players == 0) { start_game(s); return; }
                strcpy(s->message, "STALEMATE.  DRAW.");
                s->game_over = true;
            } else {
                if (s->num_players == 0) {
                    // Decay delay: each move it gets faster, floor at 0
                    s->move_count++;
                    s->min_think_ms *= 0.80;  // 20% faster each move
                    if (s->min_think_ms < 10.0) s->min_think_ms = 0.0;

                    const char *side = (s->game.turn == WHITE) ? "WHITE" : "BLACK";
                    if (chess_is_in_check(&s->game, s->game.turn))
                        snprintf(s->message, sizeof(s->message), "CHECK!  %s CALCULATING...", side);
                    else
                        snprintf(s->message, sizeof(s->message), "%s CALCULATING...", side);
                    start_ai(s);
                } else {
                    if (chess_is_in_check(&s->game, WHITE))
                        strcpy(s->message, "CHECK!  YOUR MOVE.");
                    else
                        strcpy(s->message, "YOUR MOVE.");
                }
            }
        } else {
            strcpy(s->message, "WOPR RESIGNS.");
            s->game_over = true;
        }
    }
}

// ─── Render ───────────────────────────────────────────────────────────────

void wopr_chess_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WoprChessState *s = cs(w);
    if (!s) return;

    float scale = 1.f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;

    // ── Lobby ──────────────────────────────────────────────────────────────
    if (s->screen == SCREEN_LOBBY) {
        gl_draw_text("CHESS  --  WOPR GAMES DIVISION",
                     x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
        y0 += fch * 3.f;

        gl_draw_text("HOW MANY PLAYERS?",
                     x0, y0, 0.f, 1.f, 0.5f, 1.f, scale);
        y0 += fch * 3.f;

        const char *opts[2] = { "0  --  WOPR VS WOPR", "1  --  PLAYER VS WOPR" };
        for (int i = 0; i < 2; i++) {
            bool sel = (s->lobby_sel == i);
            float r = 0.f;
            float g = sel ? 1.0f : 0.4f;
            float b = sel ? 0.4f : 0.1f;
            if (sel) gl_draw_text(">", x0, y0 + i * fch * 2.5f, 0.f, 1.f, 0.4f, 1.f, scale);
            gl_draw_text(opts[i], x0 + fcw * 3, y0 + i * fch * 2.5f, r, g, b, 1.f, scale);
        }

        y0 += fch * 7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER=START  ESC=MENU",
                     x0, y0, 0.f, 0.3f, 0.1f, 1.f, scale);
        return;
    }

    // ── Game ───────────────────────────────────────────────────────────────
    const char *title = (s->num_players == 0)
        ? "CHESS  --  WOPR VS WOPR"
        : "CHESS  --  WOPR GAMES DIVISION  (YOU=WHITE  WOPR=BLACK)";
    gl_draw_text(title, x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 1.5f;

    float cell_h = fch * 2.0f;
    float cell_w = cell_h * 1.4f;
    float avail_w   = (cols > 0) ? (float)cols * fcw : fcw * (8 * 4 + 4);
    float max_cell_w = (avail_w - fcw * 4) / 8.f;
    if (cell_w > max_cell_w) { cell_w = max_cell_w; cell_h = cell_w / 1.4f; }
    float bx = x0 + fcw * 4;
    float by = y0;
    float piece_scale = cell_h / fch * 0.75f;

    // Store board geometry for mouse hit-testing
    s->board_bx     = bx;
    s->board_by     = by;
    s->board_cell_w = cell_w;
    s->board_cell_h = cell_h;

    for (int row = 0; row < 8; row++) {
        char rl[4]; snprintf(rl, sizeof(rl), "%d ", 8 - row);
        gl_draw_text(rl, bx - fcw * 3, by + row * cell_h,
                     0.f, 0.6f, 0.2f, 1.f, scale);

        for (int col = 0; col < 8; col++) {
            float cx = bx + col * cell_w;
            float cy = by + row * cell_h;

            bool light_sq  = ((row + col) % 2 == 0);
            bool is_cursor = (s->num_players == 1 && s->sel_r == row && s->sel_c == col);
            bool is_from   = (s->has_from && s->from_r == row && s->from_c == col);
            bool is_hover  = (s->num_players == 1 && !s->ai_thinking &&
                              s->hover_r == row && s->hover_c == col);

            float br = light_sq ? 0.05f : 0.0f;
            float bg = light_sq ? 0.22f : 0.08f;
            float bb = light_sq ? 0.05f : 0.0f;
            if (is_hover)  { br = 0.0f; bg = 0.35f; bb = 0.12f; }  // dim green hover
            if (is_from)   { br = 0.4f; bg = 0.4f; bb = 0.0f; }
            if (is_cursor) { br = 0.0f; bg = 0.6f; bb = 0.2f; }
            gl_draw_rect(cx, cy, cell_w - 1, cell_h - 1, br, bg, bb, 0.9f);

            const ChessPiece &p = s->game.board[row][col];
            if (p.type != EMPTY) {
                const char *g = piece_glyph(p);
                float pr = (p.color == WHITE) ? 1.0f : 0.0f;
                float pg = (p.color == WHITE) ? 1.0f : 0.0f;
                float pb = (p.color == WHITE) ? 1.0f : 0.0f;
                float tx = cx + cell_w * 0.5f;
                float ty = cy + cell_h * 0.6f;
                gl_draw_text(g, tx, ty, pr, pg, pb, 1.f, piece_scale);
            }
        }
    }

    for (int col = 0; col < 8; col++) {
        char fl[2] = { (char)('a' + col), 0 };
        gl_draw_text(fl, bx + col * cell_w + (cell_w - fcw) * 0.5f,
                     by + 8 * cell_h + fch * 1.2f, 0.f, 0.6f, 0.2f, 1.f, scale);
    }

    float my = by + 9 * cell_h + fch * 0.5f;
    gl_draw_text(s->message, x0, my, 0.f, 1.f, 0.5f, 1.f, scale);
    my += fch * 1.5f;

    if (s->game_over) {
        gl_draw_text("R=LOBBY   ESC=MENU", x0, my, 0.f, 0.5f, 0.15f, 1.f, scale);
    } else if (s->ai_thinking) {
        if ((SDL_GetTicks() / 400) % 2 == 0)
            gl_draw_text("WOPR IS THINKING...", x0, my, 0.f, 1.f, 0.3f, 1.f, scale);
    } else if (s->num_players == 1) {
        gl_draw_text("CLICK=SELECT/MOVE  ARROWS=MOVE  ENTER=CONFIRM  R=LOBBY  ESC=MENU",
                     x0, my, 0.f, 0.5f, 0.15f, 1.f, scale);
    }
}

// ─── Input ────────────────────────────────────────────────────────────────

bool wopr_chess_keydown(WoprState *w, SDL_Keycode sym) {
    WoprChessState *s = cs(w);
    if (!s) return false;

    // ── Lobby ──────────────────────────────────────────────────────────────
    if (s->screen == SCREEN_LOBBY) {
        switch (sym) {
            case SDLK_UP:   s->lobby_sel = 0; break;
            case SDLK_DOWN: s->lobby_sel = 1; break;
            case SDLK_0:    s->lobby_sel = 0; break;
            case SDLK_1:    s->lobby_sel = 1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                s->num_players = s->lobby_sel;
                start_game(s);
                break;
            default: break;
        }
        return true;
    }

    // ── Game ───────────────────────────────────────────────────────────────
    if (s->ai_thinking) return true;

    // R always returns to lobby and resets speed
    if (sym == SDLK_r) { s->screen = SCREEN_LOBBY; s->min_think_ms = 1500.0; return true; }

    if (s->game_over || s->status != CHESS_PLAYING) return true;

    // 0-player: spectate only
    if (s->num_players == 0) return true;

    if (s->game.turn != WHITE) return true;

    switch (sym) {
        case SDLK_UP:    s->sel_r = std::max(0, s->sel_r - 1); break;
        case SDLK_DOWN:  s->sel_r = std::min(7, s->sel_r + 1); break;
        case SDLK_LEFT:  s->sel_c = std::max(0, s->sel_c - 1); break;
        case SDLK_RIGHT: s->sel_c = std::min(7, s->sel_c + 1); break;

        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: {
            int r = s->sel_r, c = s->sel_c;
            if (!s->has_from) {
                const ChessPiece &p = s->game.board[r][c];
                if (p.type != EMPTY && p.color == WHITE) {
                    s->has_from = true;
                    s->from_r = r; s->from_c = c;
                    strcpy(s->message, "PIECE SELECTED.  CHOOSE DESTINATION.");
                }
            } else {
                if (chess_is_valid_move(&s->game, s->from_r, s->from_c, r, c)) {
                    ChessMove mv{s->from_r, s->from_c, r, c, 0};
                    chess_make_move(&s->game, mv);
                    s->has_from = false;
                    s->status = chess_check_game_status(&s->game);
                    if (s->status == CHESS_CHECKMATE_BLACK) {
                        strcpy(s->message, "CHECKMATE!  YOU WIN.");
                        s->game_over = true;
                    } else if (s->status == CHESS_STALEMATE) {
                        strcpy(s->message, "STALEMATE.  DRAW.");
                        s->game_over = true;
                    } else {
                        strcpy(s->message, "WOPR CALCULATING...");
                        start_ai(s);
                    }
                } else {
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

        default: break;
    }
    return true;
}

// ─── Mouse ────────────────────────────────────────────────────────────────

// Convert pixel (px,py) → board (row,col), returns false if outside board
static bool pixel_to_board(WoprChessState *s, int px, int py, int *out_r, int *out_c) {
    if (s->board_cell_w <= 0.f || s->board_cell_h <= 0.f) return false;
    float rel_x = (float)px - s->board_bx;
    float rel_y = (float)py - s->board_by;
    int col = (int)(rel_x / s->board_cell_w);
    int row = (int)(rel_y / s->board_cell_h);
    if (col < 0 || col > 7 || row < 0 || row > 7) return false;
    *out_r = row; *out_c = col;
    return true;
}

void wopr_chess_mousemove(WoprState *w, int x, int y) {
    WoprChessState *s = cs(w);
    if (!s || s->screen != SCREEN_GAME || s->num_players != 1) return;
    int r, c;
    if (pixel_to_board(s, x, y, &r, &c)) {
        s->hover_r = r; s->hover_c = c;
    } else {
        s->hover_r = s->hover_c = -1;
    }
}

void wopr_chess_mousedown(WoprState *w, int x, int y, int button) {
    WoprChessState *s = cs(w);
    if (!s) return;

    // Lobby: click on option row selects it; double-click (or any click on selected) starts
    if (s->screen == SCREEN_LOBBY) {
        // We don't have pixel positions of the lobby rows stored, so just
        // treat any left-click as pressing Enter (confirm current selection).
        if (button == SDL_BUTTON_LEFT) {
            s->num_players = s->lobby_sel;
            start_game(s);
        }
        return;
    }

    if (button != SDL_BUTTON_LEFT) return;
    if (s->ai_thinking || s->game_over || s->status != CHESS_PLAYING) return;
    if (s->num_players == 0) return;
    if (s->game.turn != WHITE) return;

    int r, c;
    if (!pixel_to_board(s, x, y, &r, &c)) return;

    // Move the keyboard cursor to wherever the mouse clicked
    s->sel_r = r; s->sel_c = c;

    if (!s->has_from) {
        const ChessPiece &p = s->game.board[r][c];
        if (p.type != EMPTY && p.color == WHITE) {
            s->has_from = true;
            s->from_r = r; s->from_c = c;
            strcpy(s->message, "PIECE SELECTED.  CLICK DESTINATION.");
        }
    } else {
        if (chess_is_valid_move(&s->game, s->from_r, s->from_c, r, c)) {
            ChessMove mv{s->from_r, s->from_c, r, c, 0};
            chess_make_move(&s->game, mv);
            s->has_from = false;
            s->status = chess_check_game_status(&s->game);
            if (s->status == CHESS_CHECKMATE_BLACK) {
                strcpy(s->message, "CHECKMATE!  YOU WIN.");
                s->game_over = true;
            } else if (s->status == CHESS_STALEMATE) {
                strcpy(s->message, "STALEMATE.  DRAW.");
                s->game_over = true;
            } else {
                strcpy(s->message, "WOPR CALCULATING...");
                start_ai(s);
            }
        } else {
            // Clicked a different own piece — re-select it
            const ChessPiece &p = s->game.board[r][c];
            if (p.type != EMPTY && p.color == WHITE) {
                s->from_r = r; s->from_c = c;
                strcpy(s->message, "PIECE SELECTED.  CLICK DESTINATION.");
            } else {
                s->has_from = false;
                strcpy(s->message, "INVALID MOVE.");
            }
        }
    }
}
