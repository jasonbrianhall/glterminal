// wopr_checkers.cpp — WOPR Checkers sub-game
//
// Standard 8×8 checkers / draughts.
// Player vs WOPR (minimax AI, depth 7) or WOPR vs WOPR.
// Rules: forced captures, multi-jumps, kings on back rank.
// Controls: mouse click or arrow keys + Enter.
// Pieces drawn with gl_draw_rect (no external textures needed).

#include "wopr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <vector>
#include <pthread.h>
#include "wopr_render.h"

// ─── Board constants ───────────────────────────────────────────────────────

// Piece values stored in board[8][8]
//   0  = empty
//   1  = Red man,   2 = Red king
//  -1  = Black man, -2 = Black king
// Red always starts at rows 5-7, moves "up" (toward row 0)
// Black starts at rows 0-2, moves "down" (toward row 7)

typedef signed char CkPiece;
struct CkBoard { CkPiece c[8][8]; };

static inline bool ck_is_red(CkPiece p)   { return p > 0; }
static inline bool ck_is_black(CkPiece p) { return p < 0; }
static inline bool ck_is_king(CkPiece p)  { return p == 2 || p == -2; }
static inline bool ck_is_man(CkPiece p)   { return p == 1 || p == -1; }

struct CkMove {
    int  r0, c0;                // from
    int  r1, c1;                // to (final square after all jumps)
    bool is_jump;
    // full path for multi-jump: each intermediate landing square
    // path[0] = from, path[n] = final landing
    int path_r[9], path_c[9];  // up to 8 jumps
    int path_len;               // number of squares in path (including start)
    // jumped piece squares
    int cap_r[8], cap_c[8];
    int cap_len;
};

// ─── Move generation ───────────────────────────────────────────────────────

static bool ck_in(int r, int c) { return r>=0&&r<8&&c>=0&&c<8; }

// Generate all non-jump moves for one piece (used when no captures available)
static void ck_gen_slides(const CkBoard &b, int r, int c,
                           std::vector<CkMove> &out) {
    CkPiece p = b.c[r][c];
    // Direction rows the piece can move forward
    int drs[2] = { ck_is_red(p)?-1:1, 0 };
    int num_dr = 1;
    if (ck_is_king(p)) { drs[0]=-1; drs[1]=1; num_dr=2; }
    int dcs[2] = {-1, 1};
    for (int di=0; di<num_dr; di++) {
        for (int dci=0; dci<2; dci++) {
            int nr=r+drs[di], nc=c+dcs[dci];
            if (ck_in(nr,nc) && b.c[nr][nc]==0) {
                CkMove m{};
                m.r0=r; m.c0=c; m.r1=nr; m.c1=nc;
                m.is_jump=false;
                m.path_r[0]=r; m.path_c[0]=c;
                m.path_r[1]=nr; m.path_c[1]=nc;
                m.path_len=2;
                m.cap_len=0;
                out.push_back(m);
            }
        }
    }
}

// Recursive jump builder
static void ck_gen_jumps_from(CkBoard b, int r, int c, CkPiece orig,
                               int *path_r, int *path_c, int path_len,
                               int *cap_r, int *cap_c, int cap_len,
                               bool just_kinged,
                               std::vector<CkMove> &out) {
    int drs[2] = {-1,1};
    int dcs[2] = {-1,1};
    bool found = false;
    for (int di=0; di<2; di++) {
        // Men can only jump forward (or any dir if king)
        if (ck_is_man(orig)) {
            int fwd = ck_is_red(orig) ? -1 : 1;
            if (drs[di] != fwd) continue;
        }
        // Kings can jump any direction
        for (int dci=0; dci<2; dci++) {
            int mr=r+drs[di], mc=c+dcs[dci];  // jumped square
            int lr=r+2*drs[di], lc=c+2*dcs[dci];  // landing
            if (!ck_in(lr,lc)) continue;
            CkPiece mid = b.c[mr][mc];
            if (mid == 0) continue;
            // Must jump enemy
            if (ck_is_red(orig) && ck_is_red(mid)) continue;
            if (ck_is_black(orig) && ck_is_black(mid)) continue;
            if (b.c[lr][lc] != 0) continue;
            // Check not already captured this piece in this chain
            bool already = false;
            for (int k=0; k<cap_len; k++) if (cap_r[k]==mr&&cap_c[k]==mc) { already=true; break; }
            if (already) continue;

            found = true;
            // Apply jump to temp board
            CkBoard nb = b;
            nb.c[r][c] = 0;
            nb.c[mr][mc] = 0;
            nb.c[lr][lc] = orig;
            // King promotion at end row?
            bool kinged = false;
            if (ck_is_man(orig)) {
                if (ck_is_red(orig) && lr==0)  { nb.c[lr][lc]=2; kinged=true; }
                if (ck_is_black(orig) && lr==7) { nb.c[lr][lc]=-2; kinged=true; }
            }
            CkPiece next_orig = nb.c[lr][lc];

            path_r[path_len]=lr; path_c[path_len]=lc;
            cap_r[cap_len]=mr; cap_c[cap_len]=mc;

            // If just kinged, stop chain (per standard rules)
            if (kinged) {
                CkMove m{};
                m.r0=path_r[0]; m.c0=path_c[0];
                m.r1=lr; m.c1=lc; m.is_jump=true;
                for (int k=0;k<=path_len;k++) { m.path_r[k]=path_r[k]; m.path_c[k]=path_c[k]; }
                m.path_len=path_len+1;
                for (int k=0;k<=cap_len;k++) { m.cap_r[k]=cap_r[k]; m.cap_c[k]=cap_c[k]; }
                m.cap_len=cap_len+1;
                out.push_back(m);
            } else {
                ck_gen_jumps_from(nb, lr, lc, next_orig,
                                   path_r, path_c, path_len+1,
                                   cap_r, cap_c, cap_len+1,
                                   false, out);
            }
        }
    }
    if (!found && cap_len > 0) {
        // Terminal node of jump chain
        CkMove m{};
        m.r0=path_r[0]; m.c0=path_c[0];
        m.r1=r; m.c1=c; m.is_jump=true;
        for (int k=0;k<path_len;k++) { m.path_r[k]=path_r[k]; m.path_c[k]=path_c[k]; }
        m.path_len=path_len;
        for (int k=0;k<cap_len;k++) { m.cap_r[k]=cap_r[k]; m.cap_c[k]=cap_c[k]; }
        m.cap_len=cap_len;
        out.push_back(m);
    }
}

// Returns true = red to move, false = black to move
static void ck_gen_moves(const CkBoard &b, bool red_turn,
                          std::vector<CkMove> &out) {
    out.clear();
    std::vector<CkMove> jumps, slides;
    for (int r=0; r<8; r++) for (int c=0; c<8; c++) {
        CkPiece p = b.c[r][c];
        if (p==0) continue;
        if (red_turn   && !ck_is_red(p))   continue;
        if (!red_turn  && !ck_is_black(p)) continue;
        // Try jumps
        int pr[9]={r}, pc[9]={c};
        int cr[8], cc[8];
        ck_gen_jumps_from(b, r, c, p, pr, pc, 1, cr, cc, 0, false, jumps);
        if (jumps.empty()) ck_gen_slides(b, r, c, slides);
    }
    // Forced capture rule
    if (!jumps.empty()) out = jumps;
    else                out = slides;
}

// Apply a move to a board, return new board
static CkBoard ck_apply(const CkBoard &b, const CkMove &m) {
    CkBoard nb = b;
    CkPiece p  = nb.c[m.r0][m.c0];
    nb.c[m.r0][m.c0] = 0;
    for (int k=0; k<m.cap_len; k++) nb.c[m.cap_r[k]][m.cap_c[k]] = 0;
    nb.c[m.r1][m.c1] = p;
    // King promotion
    if (p==1  && m.r1==0) nb.c[m.r1][m.c1]=2;
    if (p==-1 && m.r1==7) nb.c[m.r1][m.c1]=-2;
    return nb;
}

// ─── Evaluation ────────────────────────────────────────────────────────────

static int ck_eval(const CkBoard &b) {
    int score = 0;
    for (int r=0; r<8; r++) for (int c=0; c<8; c++) {
        CkPiece p = b.c[r][c];
        if (p==0) continue;
        int v;
        if (p==1)  v = 100 + (7-r)*4;   // red man, forward progress bonus
        else if (p==-1) v = -(100 + r*4);
        else if (p==2)  v = 200;         // red king
        else            v = -200;
        // Edge penalty (kings are safer centralized)
        if (c==0||c==7) v -= (p>0?8:-8);
        score += v;
    }
    // Small random noise (±2) prevents identical positions from always
    // evaluating the same and being chosen deterministically
    score += (rand() % 5) - 2;
    return score;  // positive = good for red
}

// ─── Minimax ───────────────────────────────────────────────────────────────

static int ck_minimax(const CkBoard &b, bool red_turn, int depth,
                       int alpha, int beta) {
    std::vector<CkMove> moves;
    ck_gen_moves(b, red_turn, moves);
    if (moves.empty()) return red_turn ? -10000 : 10000;  // no moves = loss
    if (depth == 0)    return ck_eval(b);

    if (red_turn) {
        int best = -100000;
        for (auto &m : moves) {
            int v = ck_minimax(ck_apply(b, m), false, depth-1, alpha, beta);
            if (v > best) best = v;
            if (v > alpha) alpha = v;
            if (alpha >= beta) break;
        }
        return best;
    } else {
        int best = 100000;
        for (auto &m : moves) {
            int v = ck_minimax(ck_apply(b, m), true, depth-1, alpha, beta);
            if (v < best) best = v;
            if (v < beta) beta = v;
            if (alpha >= beta) break;
        }
        return best;
    }
}

struct CkAIResult { CkMove move; bool valid; };

static CkAIResult ck_best_move(const CkBoard &b, bool red_turn, int depth) {
    std::vector<CkMove> moves;
    ck_gen_moves(b, red_turn, moves);
    if (moves.empty()) return {{}, false};

    // Shuffle move order first — breaks symmetry so alpha-beta explores
    // different branches each call, preventing deterministic loops
    for (int i = (int)moves.size()-1; i > 0; i--) {
        int j = rand() % (i+1);
        std::swap(moves[i], moves[j]);
    }

    int best_score = red_turn ? -100000 : 100000;
    std::vector<CkMove> best_moves;

    for (auto &m : moves) {
        int v = ck_minimax(ck_apply(b, m), !red_turn, depth-1, -100000, 100000);
        bool better = red_turn ? (v > best_score) : (v < best_score);
        bool equal  = (v == best_score);
        if (better) { best_score = v; best_moves.clear(); best_moves.push_back(m); }
        else if (equal) { best_moves.push_back(m); }
    }

    // Pick randomly among all equally-scored moves
    CkMove best = best_moves[rand() % best_moves.size()];
    return {best, true};
}

// ─── State ─────────────────────────────────────────────────────────────────

enum CkScreen { CK_LOBBY, CK_COLOR, CK_GAME };

struct WoprCkState {
    CkScreen screen;
    int      lobby_sel;   // 0=0players, 1=1player
    int      color_sel;   // 0=red, 1=black
    bool     player_is_red;
    int      num_players;

    CkBoard  board;
    bool     red_turn;
    bool     game_over;
    char     message[128];

    // Selection
    int      sel_r, sel_c;
    bool     has_from;
    int      from_r, from_c;
    // Legal destinations from selected piece (for highlighting)
    std::vector<CkMove> from_moves;

    // Hover
    int      hover_r, hover_c;

    // Multi-jump state: when a piece has more jumps available mid-turn
    bool     mid_jump;
    int      jump_r, jump_c;    // piece that must continue jumping
    CkBoard  board_pre_jump;    // board state at start of this turn (for display)

    // Last move highlight
    int      last_r0, last_c0, last_r1, last_c1;
    bool     has_last;

    // AI thread
    bool     ai_thinking, ai_done;
    pthread_t ai_thread;
    CkAIResult ai_result;

    // Board geometry (set in render)
    float    bx, by, cell_sz;
    int      win_w, win_h;

    // Jump / slide animation
    bool     anim_active;
    float    anim_t;          // 0 → anim_legs (one unit per leg)
    int      anim_legs;       // number of path segments
    // World-space waypoints for the animated piece (pixel coords, set at move time)
    float    anim_wx[9], anim_wy[9];   // up to 8 jumps = 9 waypoints
    CkPiece  anim_piece;      // what piece is animating
    int      anim_dst_r, anim_dst_c;   // final board square (suppress static draw there)
    float    anim_speed;      // legs per second
};

static WoprCkState *ck(WoprState *w) { return (WoprCkState *)w->sub_state; }

// ─── Board init ────────────────────────────────────────────────────────────

static void ck_init_board(CkBoard &b) {
    memset(b.c, 0, sizeof(b.c));
    for (int r=0; r<8; r++) for (int c=0; c<8; c++) {
        if ((r+c)%2 == 0) continue;  // checkers only on dark squares (r+c odd)
        if (r < 3) b.c[r][c] = -1;  // black pieces at top
        if (r > 4) b.c[r][c] =  1;  // red pieces at bottom
    }
}

// ─── AI thread ─────────────────────────────────────────────────────────────

struct CkAIArg {
    CkBoard    board;
    bool       red_turn;
    CkAIResult *out;
    bool       *done;
};

static void *ck_ai_worker(void *arg) {
    CkAIArg *a = (CkAIArg *)arg;
    *a->out  = ck_best_move(a->board, a->red_turn, 7);
    *a->done = true;
    delete a;
    return nullptr;
}

static void ck_start_ai(WoprCkState *s) {
    s->ai_thinking = true;
    s->ai_done     = false;
    CkAIArg *arg   = new CkAIArg;
    arg->board     = s->board;
    arg->red_turn  = s->red_turn;
    arg->out       = &s->ai_result;
    arg->done      = &s->ai_done;
    pthread_create(&s->ai_thread, nullptr, ck_ai_worker, arg);
    pthread_detach(s->ai_thread);
}

// ─── Game start ────────────────────────────────────────────────────────────

static void ck_start_game(WoprCkState *s) {
    ck_init_board(s->board);
    s->red_turn    = true;    // red always moves first
    s->game_over   = false;
    s->has_from    = false;
    s->mid_jump    = false;
    s->hover_r = s->hover_c = -1;
    s->sel_r = 5; s->sel_c = 1;
    s->has_last    = false;
    s->ai_thinking = false;
    s->ai_done     = false;
    s->screen      = CK_GAME;
    s->from_moves.clear();
    s->anim_active = false;
    s->anim_t      = 0.f;

    if (s->num_players == 0) {
        strcpy(s->message, "WOPR VS WOPR.  RED CALCULATING...");
        ck_start_ai(s);
    } else if (!s->player_is_red) {
        strcpy(s->message, "WOPR (RED) CALCULATING...");
        ck_start_ai(s);
    } else {
        strcpy(s->message, "YOUR MOVE.  RED TO PLAY.");
    }
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────

void wopr_checkers_enter(WoprState *w) {
    WoprCkState *s = new WoprCkState{};
    s->screen       = CK_LOBBY;
    s->lobby_sel    = 1;
    s->color_sel    = 0;
    s->player_is_red = true;
    s->num_players  = 1;
    s->bx = s->by = s->cell_sz = 0.f;
    s->win_w = 800; s->win_h = 600;
    w->sub_state = s;
}

void wopr_checkers_free(WoprState *w) {
    delete ck(w);
    w->sub_state = nullptr;
}

// ─── Compute legal moves from a square (for UI highlight) ──────────────────

static void ck_compute_from_moves(WoprCkState *s) {
    s->from_moves.clear();
    if (!s->has_from) return;
    std::vector<CkMove> all;
    ck_gen_moves(s->board, s->red_turn, all);
    for (auto &m : all)
        if (m.r0==s->from_r && m.c0==s->from_c)
            s->from_moves.push_back(m);
}

// ─── Animation ─────────────────────────────────────────────────────────────

// Convert board row/col to pixel center, using stored board geometry
static void ck_board_to_px(WoprCkState *s, int r, int c, float &px, float &py) {
    bool flipped = (s->num_players==1 && !s->player_is_red);
    int vr = flipped ? 7-r : r;
    int vc = flipped ? 7-c : c;
    px = s->bx + (vc + 0.5f) * s->cell_sz;
    py = s->by + (vr + 0.5f) * s->cell_sz;
}

static void ck_start_anim(WoprCkState *s, const CkMove &m, CkPiece piece) {
    if (s->cell_sz <= 0.f) return;  // geometry not yet set, skip
    // Build waypoint list from path (path_r/path_c are in logical board coords)
    int n = m.path_len;
    if (n < 2) return;
    if (n > 9) n = 9;
    for (int i = 0; i < n; i++)
        ck_board_to_px(s, m.path_r[i], m.path_c[i], s->anim_wx[i], s->anim_wy[i]);
    s->anim_legs     = n - 1;
    s->anim_piece    = piece;
    s->anim_dst_r    = m.r1;
    s->anim_dst_c    = m.c1;
    s->anim_t        = 0.f;
    s->anim_active   = true;
    // Speed: slides ~0.15s, each jump leg ~0.18s
    s->anim_speed    = m.is_jump ? (1.f / 0.18f) : (1.f / 0.15f);
}

// ─── Apply move & check game end ───────────────────────────────────────────

static void ck_do_move(WoprCkState *s, const CkMove &m) {
    s->last_r0=m.r0; s->last_c0=m.c0; s->last_r1=m.r1; s->last_c1=m.c1;
    s->has_last = true;

    // Start animation before board updates (captures piece type while it's still there)
    CkPiece moving_piece = s->board.c[m.r0][m.c0];
    ck_start_anim(s, m, moving_piece);

    s->board = ck_apply(s->board, m);
    s->has_from = false;
    s->from_moves.clear();
    s->mid_jump = false;
    s->red_turn = !s->red_turn;

    // Check for winner
    std::vector<CkMove> opp_moves;
    ck_gen_moves(s->board, s->red_turn, opp_moves);
    if (opp_moves.empty()) {
        // The side that just moved wins
        bool red_won = !s->red_turn;   // we already flipped
        if (s->num_players == 0) { ck_start_game(s); return; }
        if ((red_won && s->player_is_red) || (!red_won && !s->player_is_red))
            strcpy(s->message, "YOU WIN!  WOPR IS DEFEATED.");
        else
            strcpy(s->message, "WOPR WINS.  BETTER LUCK NEXT TIME.");
        s->game_over = true;
        return;
    }

    if (s->num_players == 0) {
        const char *side = s->red_turn ? "RED" : "BLACK";
        snprintf(s->message, sizeof(s->message), "%s CALCULATING...", side);
        ck_start_ai(s);
    } else {
        bool player_turn = (s->red_turn == s->player_is_red);
        if (player_turn) strcpy(s->message, "YOUR MOVE.");
        else { strcpy(s->message, "WOPR CALCULATING..."); ck_start_ai(s); }
    }
}

// ─── Update ────────────────────────────────────────────────────────────────

void wopr_checkers_update(WoprState *w, double dt) {
    WoprCkState *s = ck(w);
    if (!s || s->screen != CK_GAME || s->game_over) return;

    // Advance jump/slide animation
    if (s->anim_active) {
        s->anim_t += (float)dt * s->anim_speed;
        if (s->anim_t >= (float)s->anim_legs) {
            s->anim_t      = 0.f;
            s->anim_active = false;
        }
    }

    if (s->ai_thinking && s->ai_done) {
        s->ai_thinking = false;
        if (s->ai_result.valid) {
            ck_do_move(s, s->ai_result.move);
        } else {
            strcpy(s->message, "WOPR RESIGNS.");
            s->game_over = true;
        }
    }
}

// ─── Render helpers ────────────────────────────────────────────────────────

// Draw a filled circle using horizontal scanline rects.
// Outer ring drawn first (wider radius, ring color), then inner fill on top.
static void draw_circle_filled(float cx, float cy, float r,
                                float fr, float fg, float fb, float alpha) {
    // 24 scanline slices gives a smooth circle at typical piece sizes
    const int SLICES = 24;
    float step = (2.f * r) / SLICES;
    for (int i = 0; i < SLICES; i++) {
        float y   = -r + i * step;
        float y2  = y + step;
        float ymid = (y + y2) * 0.5f;
        float half_w = sqrtf(std::max(0.f, r*r - ymid*ymid));
        if (half_w < 0.5f) continue;
        gl_draw_rect(cx - half_w, cy + y, 2.f * half_w, step,
                     fr, fg, fb, alpha);
    }
}

static void draw_checker(float cx, float cy, float r,
                          bool is_red, bool is_king, bool selected, float alpha) {
    // Colors
    float pr, pg, pb;    // piece fill
    float or_, og, ob;   // ring outline
    if (is_red) {
        pr=0.82f; pg=0.12f; pb=0.10f;
        or_=1.0f; og=0.55f; ob=0.45f;
    } else {
        pr=0.12f; pg=0.12f; pb=0.14f;
        or_=0.60f; og=0.60f; ob=0.62f;
    }
    if (selected) {
        // Tint gold when selected
        pr=pr*0.4f+0.55f; pg=pg*0.4f+0.50f; pb=pb*0.4f+0.08f;
    }

    // Outer ring: draw circle at r*1.10 with ring color
    draw_circle_filled(cx, cy, r * 1.10f, or_, og, ob, alpha * 0.9f);

    // Thin shadow ring just inside outer edge (darker)
    draw_circle_filled(cx, cy, r * 1.02f,
                       or_*0.4f, og*0.4f, ob*0.4f, alpha * 0.5f);

    // Main fill circle
    draw_circle_filled(cx, cy, r, pr, pg, pb, alpha);

    // Inner bevel ring (slightly lighter, 70% radius)
    draw_circle_filled(cx, cy, r * 0.70f,
                       pr*0.55f+0.45f, pg*0.55f+0.45f, pb*0.55f+0.45f,
                       alpha * 0.18f);

    // Specular glint: small bright circle, offset top-left
    draw_circle_filled(cx - r*0.28f, cy - r*0.28f, r * 0.22f,
                       1.f, 1.f, 1.f, alpha * 0.30f);

    // King crown: three prong rects + base bar, in gold/white
    if (is_king) {
        float kr = is_red ? 1.0f : 0.9f;
        float kg = is_red ? 0.9f : 0.9f;
        float kb = is_red ? 0.1f : 0.9f;
        float kw = r * 0.22f, kh = r * 0.40f;
        // Three prongs
        gl_draw_rect(cx - r*0.48f, cy - r*0.72f, kw, kh, kr,kg,kb, alpha);
        gl_draw_rect(cx - kw*0.5f, cy - r*0.88f, kw, kh*0.65f, kr,kg,kb, alpha);
        gl_draw_rect(cx + r*0.22f, cy - r*0.72f, kw, kh, kr,kg,kb, alpha);
        // Base bar connecting prongs
        gl_draw_rect(cx - r*0.48f, cy - r*0.36f, r*0.96f, kw*0.55f, kr,kg,kb, alpha);
    }
}

// ─── Render ────────────────────────────────────────────────────────────────

void wopr_checkers_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WoprCkState *s = ck(w);
    if (!s) return;

    float scale=1.f, fch=(float)ch, fcw=(float)cw;
    float x0=(float)ox, y0=(float)oy;

    // ── Lobby ─────────────────────────────────────────────────────────────
    if (s->screen == CK_LOBBY) {
        gl_draw_text("CHECKERS  --  WOPR GAMES DIVISION",
                     x0,y0, 0.f,1.f,0.6f,1.f,scale);
        y0 += fch*3.f;
        gl_draw_text("HOW MANY PLAYERS?", x0,y0, 0.f,1.f,0.5f,1.f,scale);
        y0 += fch*3.f;
        const char *opts[2] = {"0  --  WOPR VS WOPR","1  --  PLAYER VS WOPR"};
        for (int i=0;i<2;i++) {
            bool sel=(s->lobby_sel==i);
            float lg=sel?1.f:0.4f, lb=sel?0.4f:0.1f;
            if (sel) gl_draw_text(">",x0,y0+i*fch*2.5f, 0.f,1.f,0.4f,1.f,scale);
            gl_draw_text(opts[i],x0+fcw*3,y0+i*fch*2.5f, 0.f,lg,lb,1.f,scale);
        }
        y0 += fch*7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER/CLICK=START  ESC=MENU",
                     x0,y0, 0.f,0.3f,0.1f,1.f,scale);
        return;
    }

    // ── Color selection ───────────────────────────────────────────────────
    if (s->screen == CK_COLOR) {
        gl_draw_text("CHECKERS  --  WOPR GAMES DIVISION",
                     x0,y0, 0.f,1.f,0.6f,1.f,scale);
        y0 += fch*3.f;
        gl_draw_text("CHOOSE YOUR COLOR:", x0,y0, 0.f,1.f,0.5f,1.f,scale);
        y0 += fch*3.f;
        const char *opts[2] = {"RED   --  YOU MOVE FIRST","BLACK --  WOPR MOVES FIRST"};
        for (int i=0;i<2;i++) {
            bool sel=(s->color_sel==i);
            float lg=sel?1.f:0.4f, lb=sel?0.4f:0.1f;
            if (sel) gl_draw_text(">",x0,y0+i*fch*2.5f, 0.f,1.f,0.4f,1.f,scale);
            gl_draw_text(opts[i],x0+fcw*3,y0+i*fch*2.5f, 0.f,lg,lb,1.f,scale);
        }
        y0 += fch*7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER/CLICK=CONFIRM  ESC=BACK",
                     x0,y0, 0.f,0.3f,0.1f,1.f,scale);
        return;
    }

    // ── Game ──────────────────────────────────────────────────────────────
    {
        extern SDL_Window *g_sdl_window;
        if (g_sdl_window) SDL_GetWindowSize(g_sdl_window, &s->win_w, &s->win_h);
    }

    const char *title;
    if (s->num_players==0) title="CHECKERS  --  WOPR VS WOPR";
    else if (s->player_is_red) title="CHECKERS  --  YOU (RED) vs WOPR (BLACK)";
    else                        title="CHECKERS  --  WOPR (RED) vs YOU (BLACK)";
    gl_draw_text(title, x0,y0, 0.f,1.f,0.6f,1.f,scale);
    y0 += fch*1.5f;

    // Board sizing
    float label_pad = fcw*2.5f;
    float avail_h   = (float)s->win_h - y0 - fch*6.f;
    float avail_w   = (float)s->win_w - x0 - label_pad - fcw*2.f;
    float board_px  = std::min(avail_h, avail_w);
    board_px        = std::max(board_px, 160.f);
    float cell_sz   = board_px / 8.f;
    float bx        = x0 + label_pad;
    float by        = y0;

    s->bx=bx; s->by=by; s->cell_sz=cell_sz;

    // Checkers are flipped if player is black (black at bottom)
    bool flipped = (s->num_players==1 && !s->player_is_red);
    auto vrow = [&](int r){ return flipped ? 7-r : r; };
    auto vcol = [&](int c){ return flipped ? 7-c : c; };

    // Build set of legal destination squares for selected piece
    std::vector<std::pair<int,int>> legal_dests;
    for (auto &m : s->from_moves) legal_dests.push_back({m.r1,m.c1});

    // ── Pass 1: board squares ──────────────────────────────────────────────
    for (int row=0; row<8; row++) {
        // Rank label
        int rank = flipped ? (row+1) : (8-row);
        char rl[4]; snprintf(rl,sizeof(rl),"%d",rank);
        gl_draw_text(rl, bx-label_pad, by+row*cell_sz+cell_sz*0.28f,
                     0.5f,0.7f,0.5f,1.f,scale);

        for (int col=0; col<8; col++) {
            int lr=vrow(row), lc=vcol(col);
            float cx=bx+col*cell_sz, cy=by+row*cell_sz;
            bool dark = ((lr+lc)%2==1);   // checkers played on dark squares

            // Base square color
            float sr,sg,sb;
            if (dark) { sr=0.20f; sg=0.36f; sb=0.20f; }  // dark green
            else      { sr=0.82f; sg=0.78f; sb=0.60f; }  // ivory

            // Highlights
            bool is_from = (s->has_from && lr==s->from_r && lc==s->from_c);
            bool is_dest = false;
            for (auto &d : legal_dests) if (d.first==lr&&d.second==lc) { is_dest=true; break; }
            bool is_last = (s->has_last && ((lr==s->last_r0&&lc==s->last_c0)||(lr==s->last_r1&&lc==s->last_c1)));
            bool is_hov  = (s->hover_r==lr && s->hover_c==lc && dark);
            bool is_jmid = (s->mid_jump && lr==s->jump_r && lc==s->jump_c);

            if (is_last && dark)  { sr=sr*0.4f+0.38f; sg=sg*0.4f+0.38f; sb=sb*0.4f+0.05f; }
            if (is_hov && !is_from) { sg=std::min(1.f,sg+0.25f); }
            if (is_from)  { sr=0.85f; sg=0.75f; sb=0.08f; }
            if (is_dest)  { sr=0.15f; sg=0.70f; sb=0.35f; }
            if (is_jmid)  { sr=0.95f; sg=0.55f; sb=0.05f; }

            gl_draw_rect(cx,cy,cell_sz,cell_sz, sr,sg,sb,1.f);

            // Dot on legal dest
            if (is_dest && !s->board.c[lr][lc]) {
                float dc=cell_sz*0.18f;
                gl_draw_rect(cx+cell_sz*0.5f-dc, cy+cell_sz*0.5f-dc,
                             2*dc,2*dc, 0.1f,0.9f,0.4f,0.7f);
            }
        }
    }

    // File labels
    for (int col=0; col<8; col++) {
        char fl[2]={(char)(flipped?('h'-col):('a'+col)),0};
        gl_draw_text(fl, bx+col*cell_sz+cell_sz*0.38f, by+8*cell_sz+fch*0.25f,
                     0.5f,0.7f,0.5f,1.f,scale);
    }

    // Board border
    float bord=2.f;
    gl_draw_rect(bx-bord,by-bord,8*cell_sz+2*bord,bord,         0.f,0.5f,0.2f,1.f);
    gl_draw_rect(bx-bord,by+8*cell_sz,8*cell_sz+2*bord,bord,    0.f,0.5f,0.2f,1.f);
    gl_draw_rect(bx-bord,by-bord,bord,8*cell_sz+2*bord,         0.f,0.5f,0.2f,1.f);
    gl_draw_rect(bx+8*cell_sz,by-bord,bord,8*cell_sz+2*bord,    0.f,0.5f,0.2f,1.f);

    // ── Pass 2: pieces (skip the one currently animating at its dest) ────────
    for (int row=0; row<8; row++) {
        for (int col=0; col<8; col++) {
            int lr=vrow(row), lc=vcol(col);
            CkPiece p=s->board.c[lr][lc];
            if (!p) continue;
            // Suppress destination square while animation is in flight
            if (s->anim_active && lr==s->anim_dst_r && lc==s->anim_dst_c) continue;
            float pcx=bx+col*cell_sz+cell_sz*0.5f;
            float pcy=by+row*cell_sz+cell_sz*0.5f;
            float r=cell_sz*0.38f;
            bool sel=(s->has_from && lr==s->from_r && lc==s->from_c);
            float alpha=(sel)?0.65f:1.f;
            draw_checker(pcx,pcy,r, p>0, ck_is_king(p), sel, alpha);
        }
    }

    // ── Pass 3: animated piece (arc interpolation) ────────────────────────
    if (s->anim_active && s->anim_legs > 0) {
        // Which leg are we on?
        int   leg   = (int)s->anim_t;
        if (leg >= s->anim_legs) leg = s->anim_legs - 1;
        float frac  = s->anim_t - (float)leg;   // 0→1 within this leg

        float x0a = s->anim_wx[leg],   y0a = s->anim_wy[leg];
        float x1a = s->anim_wx[leg+1], y1a = s->anim_wy[leg+1];

        // Linear interpolation in X, parabolic arc in Y (piece rises then falls)
        float apx = x0a + frac * (x1a - x0a);
        float flat_y = y0a + frac * (y1a - y0a);

        // Arc height proportional to distance — bigger for jumps
        float dist = sqrtf((x1a-x0a)*(x1a-x0a) + (y1a-y0a)*(y1a-y0a));
        float arc_h = dist * (s->anim_legs > 1 ? 0.55f : 0.30f);
        // Parabola: 0 at frac=0, peak at frac=0.5, 0 at frac=1
        float arc_offset = -arc_h * 4.f * frac * (1.f - frac);

        float apy = flat_y + arc_offset;

        // Shadow on board at ground position (flat_y), slightly transparent
        float sr2 = cell_sz * 0.32f;
        draw_circle_filled(apx, flat_y, sr2 * (0.5f + frac*0.5f),
                           0.f, 0.f, 0.f, 0.22f);

        // The piece itself
        float pr2 = cell_sz * 0.38f;
        CkPiece ap = s->anim_piece;
        draw_checker(apx, apy, pr2, ap > 0, ck_is_king(ap), false, 1.0f);
    }

    // ── Status ────────────────────────────────────────────────────────────
    float my = by + 8*cell_sz + fch*2.0f;
    gl_draw_text(s->message, x0, my, 0.f,1.f,0.5f,1.f,scale);
    my += fch*1.5f;

    if (s->game_over)
        gl_draw_text("R=LOBBY   ESC=MENU", x0,my, 0.f,0.5f,0.15f,1.f,scale);
    else if (s->ai_thinking) {
        if ((SDL_GetTicks()/400)%2==0)
            gl_draw_text("WOPR IS THINKING...", x0,my, 0.8f,0.6f,0.f,1.f,scale);
    } else if (s->num_players==1) {
        gl_draw_text("CLICK=SELECT/MOVE  ARROWS=MOVE  ENTER=CONFIRM  R=LOBBY  ESC=MENU",
                     x0,my, 0.f,0.45f,0.15f,1.f,scale);
    }
}

// ─── Shared move attempt ───────────────────────────────────────────────────

static void ck_attempt(WoprCkState *s, int r, int c) {
    bool player_is_cur = (s->red_turn == s->player_is_red);
    if (!player_is_cur || s->ai_thinking || s->game_over) return;

    CkPiece p = s->board.c[r][c];

    if (!s->has_from) {
        // Selecting a piece
        bool owned = s->red_turn ? ck_is_red(p) : ck_is_black(p);
        if (!owned) {
            snprintf(s->message,sizeof(s->message),"NOT YOUR PIECE.");
            return;
        }
        s->has_from=true; s->from_r=r; s->from_c=c;
        ck_compute_from_moves(s);
        if (s->from_moves.empty()) {
            // No moves from this piece (shouldn't happen if legal, but handle it)
            s->has_from=false;
            snprintf(s->message,sizeof(s->message),"NO LEGAL MOVES FROM THAT PIECE.");
            return;
        }
        strcpy(s->message,"PIECE SELECTED.  CHOOSE DESTINATION.");
    } else {
        // Deselect by clicking same square
        if (r==s->from_r && c==s->from_c) {
            s->has_from=false;
            s->from_moves.clear();
            strcpy(s->message,"YOUR MOVE.");
            return;
        }
        // Try to find this destination in legal moves
        CkMove *found=nullptr;
        for (auto &m : s->from_moves)
            if (m.r1==r && m.c1==c) { found=&m; break; }
        if (found) {
            ck_do_move(s, *found);
        } else {
            // Maybe selecting a different own piece
            bool owned = s->red_turn ? ck_is_red(p) : ck_is_black(p);
            if (owned) {
                s->from_r=r; s->from_c=c;
                ck_compute_from_moves(s);
                strcpy(s->message,"PIECE SELECTED.  CHOOSE DESTINATION.");
            } else {
                s->has_from=false;
                s->from_moves.clear();
                strcpy(s->message,"INVALID MOVE.  TRY AGAIN.");
            }
        }
    }
}

// ─── Mouse ─────────────────────────────────────────────────────────────────

static bool ck_px_to_board(WoprCkState *s, int mx, int my, int *out_r, int *out_c) {
    if (s->cell_sz<=0.f) return false;
    float rx=(float)mx-s->bx, ry=(float)my-s->by;
    int col=(int)(rx/s->cell_sz), row=(int)(ry/s->cell_sz);
    if (col<0||col>7||row<0||row>7) return false;
    bool flipped=(s->num_players==1 && !s->player_is_red);
    *out_r = flipped ? 7-row : row;
    *out_c = flipped ? 7-col : col;
    return true;
}

void wopr_checkers_mousemove(WoprState *w, int x, int y) {
    WoprCkState *s = ck(w);
    if (!s||s->screen!=CK_GAME) return;
    int r,c;
    if (ck_px_to_board(s,x,y,&r,&c)) { s->hover_r=r; s->hover_c=c; }
    else { s->hover_r=s->hover_c=-1; }
}

void wopr_checkers_mousedown(WoprState *w, int x, int y, int button) {
    WoprCkState *s = ck(w);
    if (!s||button!=SDL_BUTTON_LEFT) return;

    if (s->screen==CK_LOBBY) {
        if (s->lobby_sel==0) { s->num_players=0; ck_start_game(s); }
        else { s->num_players=1; s->screen=CK_COLOR; }
        return;
    }
    if (s->screen==CK_COLOR) {
        s->player_is_red=(s->color_sel==0);
        ck_start_game(s); return;
    }
    if (s->num_players==0) return;
    int r,c;
    if (!ck_px_to_board(s,x,y,&r,&c)) return;
    ck_attempt(s,r,c);
}

// ─── Keyboard ──────────────────────────────────────────────────────────────

bool wopr_checkers_keydown(WoprState *w, SDL_Keycode sym) {
    WoprCkState *s = ck(w);
    if (!s) return false;

    if (s->screen==CK_LOBBY) {
        switch(sym) {
            case SDLK_UP:   s->lobby_sel=0; break;
            case SDLK_DOWN: s->lobby_sel=1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                if (s->lobby_sel==0) { s->num_players=0; ck_start_game(s); }
                else { s->num_players=1; s->screen=CK_COLOR; }
                break;
            default: break;
        }
        return true;
    }
    if (s->screen==CK_COLOR) {
        switch(sym) {
            case SDLK_UP:   s->color_sel=0; break;
            case SDLK_DOWN: s->color_sel=1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                s->player_is_red=(s->color_sel==0);
                ck_start_game(s); break;
            case SDLK_ESCAPE: s->screen=CK_LOBBY; break;
            default: break;
        }
        return true;
    }

    if (sym==SDLK_r) { s->screen=CK_LOBBY; return true; }
    if (s->game_over||s->ai_thinking) return true;
    if (s->num_players==0) return true;
    if (s->red_turn != s->player_is_red) return true;

    switch(sym) {
        case SDLK_UP:    s->sel_r=std::max(0,s->sel_r-1); break;
        case SDLK_DOWN:  s->sel_r=std::min(7,s->sel_r+1); break;
        case SDLK_LEFT:  s->sel_c=std::max(0,s->sel_c-1); break;
        case SDLK_RIGHT: s->sel_c=std::min(7,s->sel_c+1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            ck_attempt(s,s->sel_r,s->sel_c);
            break;
        default: break;
    }
    return true;
}
