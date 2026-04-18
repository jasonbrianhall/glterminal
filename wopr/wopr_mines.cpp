// wopr_mines.cpp — WOPR Minesweeper sub-game
//
// Wraps the existing Minesweeper class with phosphor-green ASCII rendering.
// Controls: arrows / mouse move cursor, SPACE/LMB reveals, F/RMB flags,
//           chord on revealed cell (LMB or SPACE), R resets, 1/2/3 difficulty.

#include "wopr.h"
#include "minesweeper.h"
#include <stdio.h>
#include <string.h>

#include "wopr_render.h"

// ─── State wrapper ────────────────────────────────────────────────────────

struct WoprMinesState {
    Minesweeper ms;
    char  message[128];

    // Mouse tracking — pixel coords of the board origin and cell size,
    // written by render() and read by mousedown()/mousemove().
    float grid_x0   = 0.f;
    float grid_y0   = 0.f;
    float cell_w    = 0.f;
    float cell_h    = 0.f;

    // Difficulty button hit-rects [3]: {x, y, w, h}
    float btn_x[3]  = {};
    float btn_y[3]  = {};
    float btn_w[3]  = {};
    float btn_h     = 0.f;

    // Hovered cell (-1 = none)
    int   hover_row = -1;
    int   hover_col = -1;
};

static WoprMinesState *ms(WoprState *w) { return (WoprMinesState *)w->sub_state; }
static void wopr_mines_set_cursor(SDL_SystemCursor id);  // forward decl

// ─── Helper: hit-test pixel (px,py) → board (row,col), returns false if miss
static bool pixel_to_cell(WoprMinesState *s, int px, int py, int &row, int &col)
{
    if (s->cell_w <= 0.f || s->cell_h <= 0.f) return false;
    float fx = (float)px - s->grid_x0;
    float fy = (float)py - s->grid_y0;
    if (fx < 0.f || fy < 0.f) return false;
    col = (int)(fx / s->cell_w);
    row = (int)(fy / s->cell_h);
    if (col < 0 || col >= s->ms.width)  return false;
    if (row < 0 || row >= s->ms.height) return false;
    return true;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_mines_enter(WoprState *w) {
    WoprMinesState *s = new WoprMinesState{};
    s->ms.setDifficulty(Difficulty::EASY);
    s->ms.state = GameState::PLAYING;
    strcpy(s->message, "LOCATE AND FLAG ALL EXPLOSIVE DEVICES.");
    w->sub_state = s;
}

void wopr_mines_free(WoprState *w) {
    delete ms(w);
    w->sub_state = nullptr;
    wopr_mines_set_cursor(SDL_SYSTEM_CURSOR_ARROW);
}

void wopr_mines_update(WoprState *w, double /*dt*/) {
    WoprMinesState *s = ms(w);
    if (!s) return;
    Minesweeper &m = s->ms;
    if (m.state != GameState::PLAYING) return;
    if (m.gameOver)
        strcpy(s->message, "DETONATION DETECTED.  MISSION FAILED.  [R] RETRY");
    else if (m.won)
        strcpy(s->message, "ALL DEVICES NEUTRALIZED.  WELL DONE.  [R] RETRY");
}

// ─── Render ───────────────────────────────────────────────────────────────

void wopr_mines_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WoprMinesState *s = ms(w);
    if (!s) return;
    Minesweeper &m = s->ms;

    const float scale = 1.f;
    float x0  = (float)ox;
    float y0  = (float)oy;
    float fch = (float)ch;
    float fcw = (float)cw;

    // ── Title bar ──────────────────────────────────────────────────────────
    gl_draw_text("MINESWEEPER  --  FIELD DETECTION GRID",
                 x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 1.5f;

    // ── Stats row ─────────────────────────────────────────────────────────
    int flagged_count = 0;
    for (int r = 0; r < m.height; r++)
        for (int c = 0; c < m.width; c++)
            if (m.flagged[r][c]) flagged_count++;
    int remaining = m.mines - flagged_count;

    char info[128];
    snprintf(info, sizeof(info),
             "MINES REMAINING: %d   TIME: %s",
             remaining, m.timer.getTimeString().c_str());
    gl_draw_text(info, x0, y0, 0.f, 0.9f, 0.4f, 1.f, scale);
    y0 += fch * 2.0f;

    // ── Difficulty buttons: [EASY]  [MEDIUM]  [HARD] ──────────────────────
    {
        const char *labels[3] = { "[1] EASY", "[2] MEDIUM", "[3] HARD" };
        const Difficulty diffs[3] = { Difficulty::EASY, Difficulty::MEDIUM, Difficulty::HARD };
        float bx = x0;
        float btn_h_val = fch * 1.4f;
        for (int i = 0; i < 3; i++) {
            float bw = fcw * (float)(strlen(labels[i]) + 2);
            bool active = (m.difficulty == diffs[i]);
            float br = 0.f, bg = active ? 0.35f : 0.08f, bb = active ? 0.1f : 0.f;
            gl_draw_rect(bx, y0, bw, btn_h_val, br, bg, bb, 0.95f);
            // Border
            gl_draw_rect(bx,          y0,              bw, 1.f,       0.f, 0.6f, 0.2f, 1.f);
            gl_draw_rect(bx,          y0+btn_h_val-1,  bw, 1.f,       0.f, 0.6f, 0.2f, 1.f);
            gl_draw_rect(bx,          y0,              1.f, btn_h_val, 0.f, 0.6f, 0.2f, 1.f);
            gl_draw_rect(bx+bw-1,     y0,              1.f, btn_h_val, 0.f, 0.6f, 0.2f, 1.f);

            float lr = active ? 0.f : 0.f;
            float lg = active ? 1.f : 0.55f;
            float lb = active ? 0.4f : 0.2f;
            gl_draw_text(labels[i], bx + fcw, y0 + fch * 0.9f, lr, lg, lb, 1.f, scale);

            // Store hit-rect for mouse
            s->btn_x[i] = bx;
            s->btn_y[i] = y0;
            s->btn_w[i] = bw;
            s->btn_h    = btn_h_val;

            bx += bw + fcw * 2.f;
        }
    }
    y0 += fch * 2.2f;

    // ── Grid ──────────────────────────────────────────────────────────────
    float avail_w = (cols > 0) ? (float)cols * fcw : fcw * 80.f;
    float cell_w  = (avail_w - fcw * 2.f) / (float)m.width;
    float cell_h  = fch * 1.5f;

    // Clamp cell size so cells don't get absurdly large on small boards
    if (cell_w > fch * 2.5f) cell_w = fch * 2.5f;

    // Store for mouse hit-test
    s->grid_x0 = x0;
    s->grid_y0 = y0;
    s->cell_w  = cell_w;
    s->cell_h  = cell_h;

    // Outer border around entire grid
    float grid_w_px = cell_w * m.width;
    float grid_h_px = cell_h * m.height;
    gl_draw_rect(x0 - 2,          y0 - 2,
                 grid_w_px + 4,   2.f,        0.f, 0.5f, 0.15f, 1.f);
    gl_draw_rect(x0 - 2,          y0 + grid_h_px,
                 grid_w_px + 4,   2.f,        0.f, 0.5f, 0.15f, 1.f);
    gl_draw_rect(x0 - 2,          y0 - 2,
                 2.f,             grid_h_px + 4, 0.f, 0.5f, 0.15f, 1.f);
    gl_draw_rect(x0 + grid_w_px,  y0 - 2,
                 2.f,             grid_h_px + 4, 0.f, 0.5f, 0.15f, 1.f);

    for (int row = 0; row < m.height; row++) {
        for (int col = 0; col < m.width; col++) {
            float cx = x0 + col * cell_w;
            float cy = y0 + row * cell_h;

            bool is_cursor  = (m.cursorY == row && m.cursorX == col);
            bool is_hover   = (s->hover_row == row && s->hover_col == col);
            bool is_revealed = m.revealed[row][col];
            bool is_flagged  = m.flagged[row][col];
            bool is_mine     = m.minefield[row][col];

            // ── Cell background ──
            float br = 0.f, bg, bb = 0.f;
            if (is_cursor) {
                bg = 0.45f; bb = 0.12f;
            } else if (is_hover && !is_revealed && !m.gameOver && !m.won) {
                bg = 0.22f; bb = 0.06f;
            } else if (is_revealed) {
                bg = 0.025f;
            } else {
                bg = 0.07f;
            }
            gl_draw_rect(cx, cy, cell_w - 1.f, cell_h - 1.f, br, bg, bb, 0.92f);

            // ── Thin grid lines (right + bottom edge of each cell) ──
            gl_draw_rect(cx + cell_w - 1.f, cy, 1.f, cell_h, 0.f, 0.12f, 0.04f, 1.f);
            gl_draw_rect(cx, cy + cell_h - 1.f, cell_w, 1.f, 0.f, 0.12f, 0.04f, 1.f);

            // ── Glyph ──
            char  glyph[4] = "#";
            float gr = 0.f, gg = 0.5f, gb = 0.15f;

            if (is_flagged && !is_revealed) {
                // Flag
                glyph[0] = 'F';
                gr = 1.f; gg = 0.75f; gb = 0.f;
                // Highlight flagged cells with a faint gold tint
                gl_draw_rect(cx, cy, cell_w - 1.f, cell_h - 1.f,
                             0.12f, 0.09f, 0.f, 0.6f);
            } else if (!is_revealed) {
                glyph[0] = '#'; gr = 0.f; gg = 0.45f; gb = 0.12f;
            } else if (is_mine) {
                // Exploded mine — red background flash
                gl_draw_rect(cx, cy, cell_w - 1.f, cell_h - 1.f,
                             0.3f, 0.02f, 0.02f, 0.85f);
                glyph[0] = '*'; gr = 1.f; gg = 0.25f; gb = 0.25f;
            } else {
                int adj = m.countAdjacentMines(col, row);
                if (adj == 0) {
                    glyph[0] = ' ';
                } else {
                    glyph[0] = '0' + adj;
                    switch (adj) {
                        case 1: gr=0.35f; gg=0.55f; gb=1.0f;  break; // blue
                        case 2: gr=0.2f;  gg=0.85f; gb=0.2f;  break; // green
                        case 3: gr=1.0f;  gg=0.25f; gb=0.25f; break; // red
                        case 4: gr=0.2f;  gg=0.2f;  gb=0.9f;  break; // dark blue
                        case 5: gr=0.8f;  gg=0.15f; gb=0.15f; break; // dark red
                        case 6: gr=0.15f; gg=0.9f;  gb=0.9f;  break; // cyan
                        case 7: gr=0.6f;  gg=0.f;   gb=0.6f;  break; // purple
                        default:gr=0.7f;  gg=0.7f;  gb=0.7f;  break; // grey
                    }
                }
            }

            // Cursor brightens glyph
            if (is_cursor) { gr = gr * 0.4f + 0.f; gg = 1.f; gb = gb * 0.4f + 0.3f; }

            // Centre glyph horizontally within cell
            float gx = cx + (cell_w - fcw) * 0.5f;
            float gy = cy + cell_h * 0.62f;
            gl_draw_text(glyph, gx, gy, gr, gg, gb, 1.f, scale);
        }
    }

    // ── Status / message row ──────────────────────────────────────────────
    float my = y0 + grid_h_px + fch * 1.0f;

    // Color message based on outcome
    float mr = 0.f, mg = 1.f, mb = 0.5f;
    if (m.gameOver) { mr = 1.f; mg = 0.2f; mb = 0.2f; }
    else if (m.won) { mr = 0.4f; mg = 1.f; mb = 0.4f; }

    gl_draw_text(s->message, x0, my, mr, mg, mb, 1.f, scale);
    my += fch * 1.5f;
    gl_draw_text(
        "ARROWS/MOUSE=MOVE  LMB/SPACE=REVEAL  RMB/F=FLAG  CHORD=LMB ON NUMBER  R=RESET  ESC=MENU",
        x0, my, 0.f, 0.4f, 0.12f, 1.f, scale);
}

// ─── Keyboard input ───────────────────────────────────────────────────────

bool wopr_mines_keydown(WoprState *w, SDL_Keycode sym) {
    WoprMinesState *s = ms(w);
    if (!s) return false;
    Minesweeper &m = s->ms;

    // Difficulty shortcuts
    if (sym == SDLK_1) { m.setDifficulty(Difficulty::EASY);   m.state = GameState::PLAYING; strcpy(s->message, "LOCATE AND FLAG ALL EXPLOSIVE DEVICES."); return true; }
    if (sym == SDLK_2) { m.setDifficulty(Difficulty::MEDIUM); m.state = GameState::PLAYING; strcpy(s->message, "LOCATE AND FLAG ALL EXPLOSIVE DEVICES."); return true; }
    if (sym == SDLK_3) { m.setDifficulty(Difficulty::HARD);   m.state = GameState::PLAYING; strcpy(s->message, "LOCATE AND FLAG ALL EXPLOSIVE DEVICES."); return true; }

    if (sym == SDLK_r) {
        m.reset();
        m.state = GameState::PLAYING;
        strcpy(s->message, "LOCATE AND FLAG ALL EXPLOSIVE DEVICES.");
        return true;
    }

    if (m.gameOver || m.won) return true;

    switch (sym) {
        case SDLK_UP:
            if (m.cursorY > 0) { m.cursorY--; s->hover_row = m.cursorY; s->hover_col = m.cursorX; }
            break;
        case SDLK_DOWN:
            if (m.cursorY < m.height-1) { m.cursorY++; s->hover_row = m.cursorY; s->hover_col = m.cursorX; }
            break;
        case SDLK_LEFT:
            if (m.cursorX > 0) { m.cursorX--; s->hover_row = m.cursorY; s->hover_col = m.cursorX; }
            break;
        case SDLK_RIGHT:
            if (m.cursorX < m.width-1) { m.cursorX++; s->hover_row = m.cursorY; s->hover_col = m.cursorX; }
            break;
        case SDLK_SPACE:
        case SDLK_RETURN:
            if (m.revealed[m.cursorY][m.cursorX]) {
                m.revealAdjacentCells(m.cursorY, m.cursorX);
            } else if (!m.flagged[m.cursorY][m.cursorX]) {
                m.reveal(m.cursorX, m.cursorY);
            }
            break;
        case SDLK_f:
            m.toggleFlag(m.cursorX, m.cursorY);
            break;
        default:
            break;
    }
    return true;
}

// ─── Mouse input ──────────────────────────────────────────────────────────

// Declare mousedown in wopr.h stub — we implement it here and it is dispatched
// from wopr_mousedown() in wopr.cpp.  Add to wopr.cpp dispatch:
//   case WoprPhase::PLAYING_MINES: wopr_mines_mousedown(w, x, y, button); break;
// and mousemove:
//   case WoprPhase::PLAYING_MINES: wopr_mines_mousemove(w, x, y);         break;

void wopr_mines_mousedown(WoprState *w, int px, int py, int button) {
    WoprMinesState *s = ms(w);
    if (!s) return;
    Minesweeper &m = s->ms;

    // ── Difficulty button hit-test ────────────────────────────────────────
    if (button == 1) {
        const Difficulty diffs[3] = { Difficulty::EASY, Difficulty::MEDIUM, Difficulty::HARD };
        for (int i = 0; i < 3; i++) {
            if ((float)px >= s->btn_x[i] && (float)px <= s->btn_x[i] + s->btn_w[i] &&
                (float)py >= s->btn_y[i] && (float)py <= s->btn_y[i] + s->btn_h) {
                m.setDifficulty(diffs[i]);
                m.state = GameState::PLAYING;
                strcpy(s->message, "LOCATE AND FLAG ALL EXPLOSIVE DEVICES.");
                return;
            }
        }
    }

    if (m.gameOver || m.won) {
        if (button == 1) {
            m.reset();
            m.state = GameState::PLAYING;
            strcpy(s->message, "LOCATE AND FLAG ALL EXPLOSIVE DEVICES.");
        }
        return;
    }

    int row, col;
    if (!pixel_to_cell(s, px, py, row, col)) return;

    // Update keyboard cursor to follow mouse
    m.cursorY = row;
    m.cursorX = col;

    if (button == 1) {
        // Left click: reveal or chord
        if (m.revealed[row][col]) {
            m.revealAdjacentCells(row, col);
        } else if (!m.flagged[row][col]) {
            m.reveal(col, row);
        }
    } else if (button == 3) {
        // Right click: toggle flag
        m.toggleFlag(col, row);
    }
}

static void wopr_mines_set_cursor(SDL_SystemCursor id) {
    static SDL_SystemCursor last = SDL_SYSTEM_CURSOR_ARROW;
    if (id != last) { SDL_SetCursor(SDL_CreateSystemCursor(id)); last = id; }
}

void wopr_mines_mousemove(WoprState *w, int px, int py) {
    WoprMinesState *s = ms(w);
    if (!s) return;
    Minesweeper &m = s->ms;

    int row, col;
    if (pixel_to_cell(s, px, py, row, col)) {
        s->hover_row = row;
        s->hover_col = col;
    } else {
        s->hover_row = -1;
        s->hover_col = -1;
    }

    SDL_SystemCursor cursor_id = SDL_SYSTEM_CURSOR_ARROW;

    // Hand on difficulty buttons (always clickable)
    for (int i = 0; i < 3; i++) {
        if (s->btn_w[i] > 0.f &&
            (float)px >= s->btn_x[i] && (float)px <= s->btn_x[i] + s->btn_w[i] &&
            (float)py >= s->btn_y[i] && (float)py <= s->btn_y[i] + s->btn_h) {
            cursor_id = SDL_SYSTEM_CURSOR_HAND;
            wopr_mines_set_cursor(cursor_id);
            return;
        }
    }

    // Hand on game-over/won board (click to restart)
    if (m.gameOver || m.won) {
        if (s->hover_row >= 0)
            cursor_id = SDL_SYSTEM_CURSOR_HAND;
        wopr_mines_set_cursor(cursor_id);
        return;
    }

    // During play: hand on any interactive cell
    if (s->hover_row >= 0) {
        bool revealed = m.revealed[s->hover_row][s->hover_col];
        bool flagged  = m.flagged[s->hover_row][s->hover_col];
        if (!revealed) {
            // Unrevealed: left-click reveals, right-click flags — always interactive
            cursor_id = SDL_SYSTEM_CURSOR_HAND;
        } else {
            // Revealed number cell: chordable if it has adjacent mines
            int adj = m.countAdjacentMines(s->hover_col, s->hover_row);
            if (adj > 0)
                cursor_id = SDL_SYSTEM_CURSOR_HAND;
        }
        (void)flagged;  // flagged cells are unrevealed, already handled above
    }

    wopr_mines_set_cursor(cursor_id);
}
