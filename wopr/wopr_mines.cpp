// wopr_mines.cpp — WOPR Minesweeper sub-game
//
// Wraps the existing Minesweeper class with phosphor-green ASCII rendering.
// Controls: arrows move, SPACE reveals, F flags, R resets, N new game.

#include "wopr.h"
#include "minesweeper.h"
#include <stdio.h>
#include <string.h>

#include "wopr_render.h"

// ─── State wrapper ────────────────────────────────────────────────────────

struct WoprMinesState {
    Minesweeper ms;
    char message[128];
};

static WoprMinesState *ms(WoprState *w) { return (WoprMinesState *)w->sub_state; }

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
}

void wopr_mines_update(WoprState *w, double /*dt*/) {
    WoprMinesState *s = ms(w);
    if (!s) return;
    Minesweeper &m = s->ms;

    if (m.state != GameState::PLAYING) return;
    if (m.gameOver)
        strcpy(s->message, "DETONATION DETECTED.  MISSION FAILED.  R=RETRY");
    else if (m.won)
        strcpy(s->message, "ALL DEVICES NEUTRALIZED.  WELL DONE.  R=RETRY");
}

void wopr_mines_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WoprMinesState *s = ms(w);
    if (!s) return;
    Minesweeper &m = s->ms;

    float scale = 1.f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;

    // Title
    gl_draw_text("MINESWEEPER  --  FIELD DETECTION GRID",
                 x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 1.5f;

    // Mine/flag counts
    int flagged = 0;
    for (int y = 0; y < m.height; y++)
        for (int x = 0; x < m.width; x++)
            if (m.flagged[y][x]) flagged++;
    char info[64];
    snprintf(info, sizeof(info), "MINES: %d   FLAGGED: %d   TIME: %s",
             m.mines, flagged, m.timer.getTimeString().c_str());
    gl_draw_text(info, x0, y0, 0.f, 0.8f, 0.3f, 1.f, scale);
    y0 += fch * 1.5f;

    // Cell size: make cells square-ish and large enough to see glyphs
    // Size grid to fill available width
    float avail_w = (cols > 0) ? (float)cols * fcw : fcw * 80;
    float cell_w  = (avail_w - fcw * 2) / (float)m.width;
    float cell_h  = fch * 1.5f;

    for (int row = 0; row < m.height; row++) {
        for (int col = 0; col < m.width; col++) {
            float cx = x0 + col * cell_w;
            float cy = y0 + row * cell_h;

            bool is_cursor   = (m.cursorY == row && m.cursorX == col);
            bool is_revealed = m.revealed[row][col];

            // Background — exactly like chess
            float br = 0.f, bg = is_revealed ? 0.03f : 0.08f, bb = 0.f;
            if (is_cursor) { bg = 0.5f; bb = 0.15f; }
            gl_draw_rect(cx, cy, cell_w - 1, cell_h - 1, br, bg, bb, 0.9f);

            // Glyph — drawn at cy + cell_h * 0.6f, centred, exactly like chess pieces
            char glyph[4] = "#";
            float gr = 0.f, gg = 0.5f, gb = 0.15f;

            if (m.flagged[row][col] && !is_revealed) {
                glyph[0] = 'F'; gr=1.f; gg=0.8f; gb=0.f;
            } else if (!is_revealed) {
                glyph[0] = '#'; gr=0.f; gg=0.5f; gb=0.15f;
            } else if (m.minefield[row][col]) {
                glyph[0] = '*'; gr=1.f; gg=0.2f; gb=0.2f;
            } else {
                int adj = m.countAdjacentMines(col, row);
                if (adj == 0) {
                    glyph[0] = '.'; gr=0.f; gg=0.12f; gb=0.04f;
                } else {
                    glyph[0] = '0' + adj;
                    switch (adj) {
                        case 1: gr=0.3f; gg=0.5f; gb=1.0f; break;
                        case 2: gr=0.2f; gg=0.8f; gb=0.2f; break;
                        case 3: gr=1.0f; gg=0.2f; gb=0.2f; break;
                        case 4: gr=1.0f; gg=0.9f; gb=0.1f; break;
                        case 5: gr=0.8f; gg=0.2f; gb=0.8f; break;
                        case 6: gr=0.2f; gg=0.9f; gb=0.9f; break;
                        default:gr=1.0f; gg=1.0f; gb=1.0f; break;
                    }
                }
            }

            if (is_cursor) { gr *= 0.5f; gg = 1.f; gb *= 0.5f; }
            float tx = cx;
            float ty = cy + cell_h * 0.6f;
            gl_draw_text(glyph, tx, ty, gr, gg, gb, 1.f, scale);
        }
    }

    // Status
    float my = y0 + m.height * cell_h + fch * 0.8f;
    gl_draw_text(s->message, x0, my, 0.f, 1.f, 0.5f, 1.f, scale);
    my += fch * 1.5f;
    gl_draw_text("ARROWS=MOVE  SPACE=REVEAL  F=FLAG  R=RESET  1/2/3=DIFFICULTY  ESC=MENU",
                 x0, my, 0.f, 0.5f, 0.15f, 1.f, scale);
}

bool wopr_mines_keydown(WoprState *w, SDL_Keycode sym) {
    WoprMinesState *s = ms(w);
    if (!s) return false;
    Minesweeper &m = s->ms;

    // Difficulty shortcuts
    if (sym == SDLK_1) { m.setDifficulty(Difficulty::EASY);   m.state = GameState::PLAYING; return true; }
    if (sym == SDLK_2) { m.setDifficulty(Difficulty::MEDIUM); m.state = GameState::PLAYING; return true; }
    if (sym == SDLK_3) { m.setDifficulty(Difficulty::HARD);   m.state = GameState::PLAYING; return true; }

    if (sym == SDLK_r) {
        m.reset();
        m.state = GameState::PLAYING;
        strcpy(s->message, "LOCATE AND FLAG ALL EXPLOSIVE DEVICES.");
        return true;
    }

    if (m.gameOver || m.won) return true;

    switch (sym) {
        case SDLK_UP:    if (m.cursorY > 0) m.cursorY--; break;
        case SDLK_DOWN:  if (m.cursorY < m.height-1) m.cursorY++; break;
        case SDLK_LEFT:  if (m.cursorX > 0) m.cursorX--; break;
        case SDLK_RIGHT: if (m.cursorX < m.width-1)  m.cursorX++; break;
        case SDLK_SPACE: case SDLK_RETURN:
            if (m.revealed[m.cursorY][m.cursorX]) {
                // Chord: reveal adjacent cells if flag count matches mine count
                m.revealAdjacentCells(m.cursorY, m.cursorX);
            } else if (!m.flagged[m.cursorY][m.cursorX]) {
                m.reveal(m.cursorX, m.cursorY);
            }
            break;
        case SDLK_f:
            m.toggleFlag(m.cursorX, m.cursorY);
            break;
        default: break;
    }
    return true;
}
