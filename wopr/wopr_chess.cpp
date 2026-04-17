// wopr_chess.cpp — WOPR Chess sub-game
// Player = WHITE.  WOPR = BLACK.  Depth 5 minimax.
// Controls: arrow keys / ENTER or mouse click to select/move.
// Renders piece sprites from embedded BMP data (chess_pieces.h) via GL textures.

#include "wopr.h"
#include "beatchess.h"
#include "chess_ai_move.h"
#include "wopr_chess_pieces.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>
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

    int    move_count;
    double min_think_ms;

    // Board geometry — written by render, read by mouse hit-test
    float  board_bx, board_by;
    float  board_cell_w, board_cell_h;

    // Mouse hover (1-player): cell under cursor, or -1
    int    hover_r, hover_c;

    // Last-move highlight
    int  last_from_r, last_from_c, last_to_r, last_to_c;
    bool has_last_move;

    // Current window size (set by render, used by piece draw)
    int win_w, win_h;
};

struct AiArg {
    ChessGameState  game;
    ChessMove      *out;
    bool           *done;
    double          min_think_ms;
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

// ─── AI ───────────────────────────────────────────────────────────────────

static void start_ai(WoprChessState *s) {
    s->ai_thinking = true;
    s->ai_done     = false;
    AiArg *arg        = new AiArg;
    arg->game         = s->game;
    arg->out          = &s->ai_result;
    arg->done         = &s->ai_done;
    arg->min_think_ms = s->min_think_ms;
    pthread_create(&s->ai_thread, nullptr, ai_worker, arg);
    pthread_detach(s->ai_thread);
}

static void start_game(WoprChessState *s) {
    chess_init_board(&s->game);
    chess_init_zobrist();
    s->status        = CHESS_PLAYING;
    s->sel_r = 7; s->sel_c = 4;
    s->has_from      = false;
    s->ai_thinking   = false;
    s->ai_done       = false;
    s->game_over     = false;
    s->screen        = SCREEN_GAME;
    s->move_count    = 0;
    s->has_last_move = false;
    s->hover_r = s->hover_c = -1;

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
    s->min_think_ms = 1500.0;
    s->board_bx = s->board_by = s->board_cell_w = s->board_cell_h = 0.f;
    s->hover_r = s->hover_c = -1;
    s->has_last_move  = false;
    s->last_from_r = s->last_from_c = s->last_to_r = s->last_to_c = -1;
    s->win_w = 800; s->win_h = 600;
    w->sub_state = s;

    chess_pieces_gl_init();   // upload BMP textures (no-op if already done)
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
            s->last_from_r = mv.from_row; s->last_from_c = mv.from_col;
            s->last_to_r   = mv.to_row;   s->last_to_c   = mv.to_col;
            s->has_last_move = true;
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
                    s->move_count++;
                    s->min_think_ms *= 0.80;
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

// ─── Square color palette ────────────────────────────────────────────────

static void sq_base_color(bool light, float *r, float *g, float *b) {
    if (light) {
        *r = 0.87f; *g = 0.80f; *b = 0.62f;   // warm ivory
    } else {
        *r = 0.18f; *g = 0.35f; *b = 0.38f;   // deep slate-teal
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
            float lr = 0.f, lg = sel ? 1.0f : 0.4f, lb = sel ? 0.4f : 0.1f;
            if (sel) gl_draw_text(">", x0, y0 + i * fch * 2.5f, 0.f, 1.f, 0.4f, 1.f, scale);
            gl_draw_text(opts[i], x0 + fcw * 3, y0 + i * fch * 2.5f, lr, lg, lb, 1.f, scale);
        }
        y0 += fch * 7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER/CLICK=START  ESC=MENU",
                     x0, y0, 0.f, 0.3f, 0.1f, 1.f, scale);
        return;
    }

    // ── Game ───────────────────────────────────────────────────────────────
    {
        extern SDL_Window *g_sdl_window;
        if (g_sdl_window)
            SDL_GetWindowSize(g_sdl_window, &s->win_w, &s->win_h);
    }

    const char *title = (s->num_players == 0)
        ? "CHESS  --  WOPR VS WOPR"
        : "CHESS  --  YOU (WHITE) vs WOPR (BLACK)";
    gl_draw_text(title, x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 1.5f;

    // ── Board sizing: fill most of the available space ─────────────────────
    float label_pad = fcw * 2.5f;
    float avail_h   = (float)s->win_h - y0 - fch * 6.f;
    float avail_w   = (float)s->win_w - x0 - label_pad - fcw * 2.f;
    float board_px  = std::min(avail_h, avail_w);
    board_px        = std::max(board_px, 160.f);
    float cell_sz   = board_px / 8.f;

    float bx = x0 + label_pad;
    float by = y0;

    s->board_bx     = bx;
    s->board_by     = by;
    s->board_cell_w = cell_sz;
    s->board_cell_h = cell_sz;

    // ── Pass 1: colored squares ────────────────────────────────────────────
    for (int row = 0; row < 8; row++) {
        // Rank label
        char rl[3]; snprintf(rl, sizeof(rl), "%d", 8 - row);
        gl_draw_text(rl, bx - label_pad, by + row * cell_sz + cell_sz * 0.28f,
                     0.6f, 0.8f, 0.6f, 1.f, scale);

        for (int col = 0; col < 8; col++) {
            float cx = bx + col * cell_sz;
            float cy = by + row * cell_sz;

            bool light   = ((row + col) % 2 == 0);
            bool is_from = (s->has_from && s->from_r == row && s->from_c == col);
            bool is_cur  = (s->num_players == 1 && s->sel_r == row && s->sel_c == col);
            bool is_hov  = (s->num_players == 1 && !s->ai_thinking &&
                            s->hover_r == row && s->hover_c == col &&
                            !is_from && !is_cur);
            bool is_lt   = (s->has_last_move &&
                            ((s->last_to_r   == row && s->last_to_c   == col) ||
                             (s->last_from_r == row && s->last_from_c == col)));

            float sr, sg, sb;
            sq_base_color(light, &sr, &sg, &sb);

            // Last-move: gold tint
            if (is_lt)   { sr = sr*0.45f+0.55f; sg = sg*0.45f+0.48f; sb = sb*0.45f+0.08f; }
            // Hover: cyan wash
            if (is_hov)  { sr = sr*0.3f+0.15f;  sg = sg*0.3f+0.55f;  sb = sb*0.3f+0.50f; }
            // Selected origin: bright yellow
            if (is_from) { sr = 0.92f; sg = 0.82f; sb = 0.12f; }
            // Keyboard cursor: bright green
            if (is_cur && !is_from) { sr = 0.12f; sg = 0.82f; sb = 0.32f; }

            gl_draw_rect(cx, cy, cell_sz, cell_sz, sr, sg, sb, 1.0f);
        }
    }

    // File labels
    for (int col = 0; col < 8; col++) {
        char fl[2] = { (char)('a' + col), 0 };
        gl_draw_text(fl, bx + col * cell_sz + cell_sz * 0.38f,
                     by + 8 * cell_sz + fch * 0.25f,
                     0.6f, 0.8f, 0.6f, 1.f, scale);
    }

    // Thin border
    float bord = 2.f;
    gl_draw_rect(bx - bord, by - bord, 8*cell_sz + 2*bord, bord,    0.f, 0.5f, 0.35f, 1.f);
    gl_draw_rect(bx - bord, by+8*cell_sz, 8*cell_sz+2*bord, bord,   0.f, 0.5f, 0.35f, 1.f);
    gl_draw_rect(bx - bord, by - bord, bord, 8*cell_sz + 2*bord,    0.f, 0.5f, 0.35f, 1.f);
    gl_draw_rect(bx+8*cell_sz, by-bord, bord, 8*cell_sz + 2*bord,   0.f, 0.5f, 0.35f, 1.f);

    // ── Flush colored quads before switching to texture shader ─────────────
    gl_flush_verts();

    // ── Pass 2: piece sprites ──────────────────────────────────────────────
    float pad  = cell_sz * 0.07f;
    float psz  = cell_sz - 2.f * pad;

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const ChessPiece &p = s->game.board[row][col];
            if (p.type == EMPTY) continue;

            float px = bx + col * cell_sz + pad;
            float py = by + row * cell_sz + pad;

            // Dim selected piece so the yellow square shows through
            float alpha = (s->has_from && s->from_r == row && s->from_c == col)
                          ? 0.60f : 1.0f;

            draw_chess_piece(p.type, p.color, px, py, psz,
                             s->win_w, s->win_h, alpha);
        }
    }

    // ── Status text ────────────────────────────────────────────────────────
    float my = by + 8 * cell_sz + fch * 2.0f;

    // Check indicator in red
    if (s->status == CHESS_PLAYING && !s->game_over &&
        chess_is_in_check(const_cast<ChessGameState*>(&s->game), s->game.turn)) {
        gl_draw_text("CHECK!", x0, my, 1.f, 0.15f, 0.1f, 1.f, scale * 1.2f);
        my += fch * 1.6f;
    }

    gl_draw_text(s->message, x0, my, 0.f, 1.f, 0.5f, 1.f, scale);
    my += fch * 1.5f;

    if (s->game_over) {
        gl_draw_text("R=LOBBY   ESC=MENU", x0, my, 0.f, 0.5f, 0.15f, 1.f, scale);
    } else if (s->ai_thinking) {
        if ((SDL_GetTicks() / 400) % 2 == 0)
            gl_draw_text("WOPR IS THINKING...", x0, my, 0.8f, 0.6f, 0.f, 1.f, scale);
    } else if (s->num_players == 1) {
        gl_draw_text("CLICK=SELECT/MOVE  ARROWS=MOVE  ENTER=CONFIRM  R=LOBBY  ESC=MENU",
                     x0, my, 0.f, 0.45f, 0.15f, 1.f, scale);
    }
}

// ─── Shared move logic (keyboard + mouse both call this) ──────────────────

static void attempt_move(WoprChessState *s, int r, int c) {
    s->sel_r = r; s->sel_c = c;

    if (!s->has_from) {
        const ChessPiece &p = s->game.board[r][c];
        if (p.type != EMPTY && p.color == WHITE) {
            s->has_from = true;
            s->from_r = r; s->from_c = c;
            strcpy(s->message, "PIECE SELECTED.  CHOOSE DESTINATION.");
        }
    } else {
        if (r == s->from_r && c == s->from_c) {
            // Same square — deselect
            s->has_from = false;
            strcpy(s->message, "YOUR MOVE.  WHITE TO PLAY.");
            return;
        }
        if (chess_is_valid_move(&s->game, s->from_r, s->from_c, r, c)) {
            s->last_from_r = s->from_r; s->last_from_c = s->from_c;
            s->last_to_r   = r;          s->last_to_c   = c;
            s->has_last_move = true;
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
                strcpy(s->message, "PIECE SELECTED.  CHOOSE DESTINATION.");
            } else {
                s->has_from = false;
                strcpy(s->message, "INVALID MOVE.  TRY AGAIN.");
            }
        }
    }
}

// ─── Mouse ────────────────────────────────────────────────────────────────

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
    if (pixel_to_board(s, x, y, &r, &c)) { s->hover_r = r; s->hover_c = c; }
    else                                  { s->hover_r = s->hover_c = -1; }
}

void wopr_chess_mousedown(WoprState *w, int x, int y, int button) {
    WoprChessState *s = cs(w);
    if (!s) return;

    if (s->screen == SCREEN_LOBBY) {
        if (button == SDL_BUTTON_LEFT) { s->num_players = s->lobby_sel; start_game(s); }
        return;
    }

    if (button != SDL_BUTTON_LEFT) return;
    if (s->ai_thinking || s->game_over || s->status != CHESS_PLAYING) return;
    if (s->num_players == 0) return;
    if (s->game.turn != WHITE) return;

    int r, c;
    if (!pixel_to_board(s, x, y, &r, &c)) return;
    attempt_move(s, r, c);
}

// ─── Keyboard ────────────────────────────────────────────────────────────

bool wopr_chess_keydown(WoprState *w, SDL_Keycode sym) {
    WoprChessState *s = cs(w);
    if (!s) return false;

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

    if (s->ai_thinking) return true;
    if (sym == SDLK_r) { s->screen = SCREEN_LOBBY; s->min_think_ms = 1500.0; return true; }
    if (s->game_over || s->status != CHESS_PLAYING) return true;
    if (s->num_players == 0) return true;
    if (s->game.turn != WHITE) return true;

    switch (sym) {
        case SDLK_UP:    s->sel_r = std::max(0, s->sel_r - 1); break;
        case SDLK_DOWN:  s->sel_r = std::min(7, s->sel_r + 1); break;
        case SDLK_LEFT:  s->sel_c = std::max(0, s->sel_c - 1); break;
        case SDLK_RIGHT: s->sel_c = std::min(7, s->sel_c + 1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            attempt_move(s, s->sel_r, s->sel_c);
            break;
        default: break;
    }
    return true;
}
