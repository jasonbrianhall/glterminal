#include "terminal.h"
#include "term_pty.h"   // term_write, term_feed
#include "ft_font.h"    // s_ft_face, g_font_size
#include "gl_terminal.h" // TERM_COLS_DEFAULT etc.
#include "kitty_graphics.h"
#include "basic_graphics.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#  include <sys/ioctl.h>
#endif

extern int g_font_size;
// g_sdl_window needed for OSC title; forward-declared here, defined in main
extern SDL_Window *g_sdl_window;
// Current window dimensions — needed to pass to basic_handle_osc for coordinate mapping.
// Defined in gl_terminal_main.cpp (local to main, but we only need a rough snapshot).
extern int g_basic_win_w;
extern int g_basic_win_h;

// When false, ESC _ (APC) sequences are silently discarded instead of being
// passed to kitty_handle_apc(). Set to false for SSH sessions where tmux
// and other multiplexers send APC sequences that aren't kitty graphics.
bool g_kitty_enabled = true;

// ============================================================================
// SCROLLBACK
// ============================================================================

static void sb_push(Terminal *t, int row) {
    //SDL_Log("[Scroll] sb_push called: sb_buf=%p sb_cap=%d scroll_top=%d scroll_bot=%d rows=%d\n", (void*)t->sb_buf, t->sb_cap, t->scroll_top, t->scroll_bot, t->rows);
    if (!t->sb_buf || t->sb_cap == 0) return;
    if (t->scroll_top != 0 || t->scroll_bot != t->rows - 1) return;
    int slot = (t->sb_head + t->sb_count) % t->sb_cap;
    memcpy(t->sb_buf + slot * t->cols, &CELL(t, row, 0), sizeof(Cell) * t->cols);
    if (t->sb_count < t->sb_cap) {
        t->sb_count++;
    } else {
        t->sb_head = (t->sb_head + 1) % t->sb_cap;
    }
    //if (t->sb_count <= 5 || t->sb_count % 50 == 0)
        //SDL_Log("[Scroll] sb_push: sb_count now %d\n", t->sb_count);
}

Cell* sb_row(Terminal *t, int idx) {
    int slot = (t->sb_head + idx) % t->sb_cap;
    return t->sb_buf + slot * t->cols;
}

Cell* vcell(Terminal *t, int vrow, int col) {
    static Cell blank = {' ', 7, 0, 0, {0,0,0}};
    if (col < 0 || col >= t->cols) return &blank;
    if (vrow < 0) return &blank;
    if (vrow < t->sb_count) return sb_row(t, vrow) + col;
    int live = vrow - t->sb_count;
    if (live < t->rows) return &CELL(t, live, col);
    return &blank;
}

// ============================================================================
// SCROLL / CURSOR
// ============================================================================

static void scroll_up(Terminal *t) {
    int top = t->scroll_top;
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    if (top == 0) sb_push(t, top);
    if (bot > top)
        memmove(&CELL(t,top,0), &CELL(t,top+1,0), sizeof(Cell)*t->cols*(bot-top));
    for (int c = 0; c < t->cols; c++)
        CELL(t,bot,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
    term_dirty_rows(t, top, bot);
    // Shift image placements up with the scroll region
    if (top == 0 && bot == t->rows - 1)
        kitty_scroll(t, 1);
}

static void scroll_down(Terminal *t) {
    int top = t->scroll_top;
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    if (bot > top)
        memmove(&CELL(t,top+1,0), &CELL(t,top,0), sizeof(Cell)*t->cols*(bot-top));
    for (int c = 0; c < t->cols; c++)
        CELL(t,top,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
    term_dirty_rows(t, top, bot);
}

static void newline(Terminal *t) {
    int bot = SDL_min(t->scroll_bot, t->rows - 1);
    if (t->cur_row < bot) { t->cur_row++; return; }
    if (t->cur_row == bot) { scroll_up(t); return; }
    if (t->cur_row < t->rows - 1) t->cur_row++;
}

// Public wrapper used by kitty_graphics to advance the cursor with proper scrolling
void term_newline(Terminal *t) { newline(t); }

// ============================================================================
// SGR
// ============================================================================

static void sgr(Terminal *t, const char *p) {
    char buf[128]; strncpy(buf, p, 127); buf[127]='\0';
    int params[32]; int pc = 0;
    char *tok = strtok(buf, ";");
    while (tok && pc < 32) { params[pc++] = atoi(tok); tok = strtok(NULL, ";"); }
    if (!pc) { params[0]=0; pc=1; }

    for (int i = 0; i < pc; i++) {
        int v = params[i];
        if      (v == 0)  { t->cur_fg=TCOLOR_PALETTE(7); t->cur_bg=TCOLOR_PALETTE(0); t->cur_attrs=0; }
        else if (v == 1)  t->cur_attrs |= ATTR_BOLD;
        else if (v == 2)  t->cur_attrs |= ATTR_DIM;
        else if (v == 3)  t->cur_attrs |= ATTR_ITALIC;
        else if (v == 4)  t->cur_attrs |= ATTR_UNDERLINE;
        else if (v == 5)  t->cur_attrs |= ATTR_BLINK;
        else if (v == 7)  t->cur_attrs |= ATTR_REVERSE;
        else if (v == 9)  t->cur_attrs |= ATTR_STRIKE;
        else if (v == 22) t->cur_attrs &= ~(ATTR_BOLD | ATTR_DIM);
        else if (v == 23) t->cur_attrs &= ~ATTR_ITALIC;
        else if (v == 24) t->cur_attrs &= ~ATTR_UNDERLINE;
        else if (v == 25) t->cur_attrs &= ~ATTR_BLINK;
        else if (v == 27) t->cur_attrs &= ~ATTR_REVERSE;
        else if (v == 29) t->cur_attrs &= ~ATTR_STRIKE;
        else if (v == 53) t->cur_attrs |= ATTR_OVERLINE;
        else if (v == 55) t->cur_attrs &= ~ATTR_OVERLINE;
        else if (v>=30 && v<=37)   t->cur_fg = TCOLOR_PALETTE(v-30);
        else if (v == 38) {
            if (i+1 < pc && params[i+1] == 5 && i+2 < pc) {
                t->cur_fg = TCOLOR_PALETTE(params[i+2] & 0xFF); i += 2;
            } else if (i+1 < pc && params[i+1] == 2 && i+4 < pc) {
                t->cur_fg = TCOLOR_RGB(params[i+2], params[i+3], params[i+4]); i += 4;
            }
        }
        else if (v == 39)            t->cur_fg = TCOLOR_PALETTE(7);
        else if (v>=40 && v<=47)   t->cur_bg = TCOLOR_PALETTE(v-40);
        else if (v == 48) {
            if (i+1 < pc && params[i+1] == 5 && i+2 < pc) {
                t->cur_bg = TCOLOR_PALETTE(params[i+2] & 0xFF); i += 2;
            } else if (i+1 < pc && params[i+1] == 2 && i+4 < pc) {
                t->cur_bg = TCOLOR_RGB(params[i+2], params[i+3], params[i+4]); i += 4;
            }
        }
        else if (v == 49)            t->cur_bg = TCOLOR_PALETTE(0);
        else if (v>=90 && v<=97)   t->cur_fg = TCOLOR_PALETTE(v-90+8);
        else if (v>=100 && v<=107) t->cur_bg = TCOLOR_PALETTE(v-100+8);
    }
}

// ============================================================================
// CSI DISPATCH
// ============================================================================

static void dispatch_csi(Terminal *t) {
    if (!t->csi_len) return;
    char final = t->csi[t->csi_len-1];
    t->csi[t->csi_len-1] = '\0';
    const char *p = t->csi;
    switch (final) {
    case 'm': sgr(t, p); break;
    case 'H': case 'f': {
        int row=1,col=1; sscanf(p,"%d;%d",&row,&col);
        t->cur_row = SDL_clamp(row-1, 0, t->rows-1);
        t->cur_col = SDL_clamp(col-1, 0, t->cols-1);
        break;
    }
    case 'A': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_max(0,t->cur_row-n); break; }
    case 'B': { int n=atoi(p); if(n<1)n=1; t->cur_row=SDL_min(t->rows-1,t->cur_row+n); break; }
    case 'C': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_min(t->cols-1,t->cur_col+n); break; }
    case 'D': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_max(0,t->cur_col-n); break; }
    case 'G': { int n=atoi(p); if(n<1)n=1; t->cur_col=SDL_clamp(n-1,0,t->cols-1); break; }
    case 'J': {
        int n=atoi(p);
        if (n==2||n==3) {
            for(int r=0;r<t->rows;r++) for(int c=0;c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
            t->cur_row=t->cur_col=0;
            term_dirty_all(t);
        } else if(n==1) {
            for(int r=0;r<t->cur_row;r++) for(int c=0;c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
            term_dirty_rows(t, 0, t->cur_row);
        } else {
            for(int r=t->cur_row;r<t->rows;r++)
                for(int c=(r==t->cur_row?t->cur_col:0);c<t->cols;c++) CELL(t,r,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
            term_dirty_rows(t, t->cur_row, t->rows - 1);
        }
        break;
    }
    case 'K': {
        int n=atoi(p);
        int s=(n==1)?0:t->cur_col, e=(n==0)?t->cols:t->cur_col+1;
        for(int c=s;c<e&&c<t->cols;c++) CELL(t,t->cur_row,c)={' ',t->cur_fg,t->cur_bg,0,{0,0,0}};
        term_dirty_row(t, t->cur_row);
        break;
    }
    case 'h': case 'l': {
        // DECSET / DECRST (only parse ?25 for cursor visibility)
        int mode = atoi(p);
        if (final == 'h' && mode == 25) t->cursor_on = true;
        if (final == 'l' && mode == 25) t->cursor_on = false;
        break;
    }
    case 'M': {
        // DL - delete line(s): remove n lines at cursor row, pulling rows
        // below up and blanking the bottom of the scroll region.
        int n = atoi(p); if (n < 1) n = 1;
        int bot = SDL_min(t->scroll_bot, t->rows - 1);
        if (t->cur_row >= t->scroll_top && t->cur_row <= bot) {
            int save_top = t->scroll_top;
            t->scroll_top = t->cur_row;
            for (int i = 0; i < n; i++) scroll_up(t);
            t->scroll_top = save_top;
        }
        break;
    }
    case 'L': {
        // IL - insert line(s): push rows at/after cursor down, blanking
        // n lines at the cursor row within the scroll region.
        int n = atoi(p); if (n < 1) n = 1;
        int bot = SDL_min(t->scroll_bot, t->rows - 1);
        if (t->cur_row >= t->scroll_top && t->cur_row <= bot) {
            int save_top = t->scroll_top;
            t->scroll_top = t->cur_row;
            for (int i = 0; i < n; i++) scroll_down(t);
            t->scroll_top = save_top;
        }
        break;
    }
    case 'P': {
        // DCH - delete character(s): shift cells after cursor left,
        // blank the vacated cells at end of line.
        int n = atoi(p); if (n < 1) n = 1;
        int row = t->cur_row, col = t->cur_col;
        if (col < t->cols) {
            int count = t->cols - col;
            if (n > count) n = count;
            int remain = count - n;
            if (remain > 0)
                memmove(&CELL(t,row,col), &CELL(t,row,col+n), sizeof(Cell)*remain);
            for (int c = t->cols - n; c < t->cols; c++)
                CELL(t,row,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
            term_dirty_row(t, row);
        }
        break;
    }
    case '@': {
        // ICH - insert character(s): shift cells at/after cursor right,
        // blank n cells at the cursor.
        int n = atoi(p); if (n < 1) n = 1;
        int row = t->cur_row, col = t->cur_col;
        if (col < t->cols) {
            int count = t->cols - col;
            if (n > count) n = count;
            int remain = count - n;
            if (remain > 0)
                memmove(&CELL(t,row,col+n), &CELL(t,row,col), sizeof(Cell)*remain);
            for (int c = col; c < col + n && c < t->cols; c++)
                CELL(t,row,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
            term_dirty_row(t, row);
        }
        break;
    }
    case 'X': {
        // ECH - erase character(s): blank n cells from cursor, no shifting.
        int n = atoi(p); if (n < 1) n = 1;
        int row = t->cur_row;
        for (int c = t->cur_col; c < t->cur_col + n && c < t->cols; c++)
            CELL(t,row,c) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
        term_dirty_row(t, row);
        break;
    }
    case 'r': {
        // DECSTBM - set scroll region (1-based, inclusive). No args = full screen.
        int top = 1, bot = t->rows;
        if (p[0]) sscanf(p, "%d;%d", &top, &bot);
        top = SDL_clamp(top, 1, t->rows) - 1;
        bot = SDL_clamp(bot, 1, t->rows) - 1;
        if (top < bot) {
            t->scroll_top = top;
            t->scroll_bot = bot;
        } else {
            t->scroll_top = 0;
            t->scroll_bot = t->rows - 1;
        }
        t->cur_row = t->scroll_top;
        t->cur_col = 0;
        break;
    }
    case 'c': {
        // DA - device attributes. '>' prefix = secondary (DA2), else primary (DA1).
        char resp[32];
        int len;
        if (p[0] == '>')
            len = snprintf(resp, sizeof(resp), "\x1b[>1;100;0c");
        else
            len = snprintf(resp, sizeof(resp), "\x1b[?1;2c");
        term_write(t, resp, len);
        break;
    }
    case 'n': {
        // DSR - device status report
        int n = atoi(p);
        char resp[32];
        if (n == 6) {
            int len = snprintf(resp, sizeof(resp), "\x1b[%d;%dR", t->cur_row+1, t->cur_col+1);
            term_write(t, resp, len);
        } else if (n == 5) {
            const char *ok = "\x1b[0n";
            term_write(t, ok, (int)strlen(ok));
        }
        break;
    }
    case 't': {
        // Window manipulation - only report queries are answered; the rest
        // (raise/lower/iconify/move/resize) are no-ops since we're windowed.
        int op = atoi(p);
        char resp[64];
        int len = 0;
        if (op == 18) {
            // Report text area size in characters
            len = snprintf(resp, sizeof(resp), "\x1b[8;%d;%dt", t->rows, t->cols);
        } else if (op == 14) {
            // Report text area size in pixels
            int px_w = (int)(t->cols * t->cell_w);
            int px_h = (int)(t->rows * t->cell_h);
            len = snprintf(resp, sizeof(resp), "\x1b[4;%d;%dt", px_h, px_w);
        }
        if (len > 0) term_write(t, resp, len);
        break;
    }
    }
    t->csi_len = 0;
}

void term_feed(Terminal *t, const char *data, int size) {
    for (int i = 0; i < size; i++) {
        unsigned char ch = (unsigned char)data[i];
        int prev_row = t->cur_row, prev_col = t->cur_col;

        switch (t->state) {
        case PS_NORMAL:
            if (ch == 0x1b) {
                t->state = PS_ESC;
            } else if (ch == 0x08) {
                if (t->cur_col > 0) t->cur_col--;
            } else if (ch == 0x09) {
                t->cur_col = (t->cur_col + 8) & ~7;
                if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;
            } else if (ch == 0x0a || ch == 0x0b || ch == 0x0c) {
                newline(t);
            } else if (ch == 0x0d) {
                t->cur_col = 0;
            } else if (ch >= 32 && ch < 127) {
                if (t->cur_col < t->cols) {
                    CELL(t, t->cur_row, t->cur_col) = {(char)ch, t->cur_fg, t->cur_bg, t->cur_attrs, {0,0,0}};
                    term_dirty_row(t, t->cur_row);
                }
                t->cur_col++;
                if (t->autowrap && t->cur_col >= t->cols) {
                    t->cur_col = 0;
                    newline(t);
                }
            } else if (ch == 0x7f) {
                // DEL is treated as backspace
                if (t->cur_col > 0) {
                    t->cur_col--;
                    CELL(t, t->cur_row, t->cur_col) = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};
                    term_dirty_row(t, t->cur_row);
                }
            }
            break;

        case PS_ESC:
            if (ch == '[') {
                t->csi_len = 0;
                t->state = PS_CSI;
            } else if (ch == ']') {
                t->osc_len = 0;
                t->state = PS_OSC;
            } else if (ch == '_') {
                t->apc_len = 0;
                t->apc_esc_pending = false;
                t->state = PS_APC;
            } else if (ch == 'P') {
                t->state = PS_DCS;
            } else if (ch == '^') {
                t->state = PS_PM;
            } else if (ch == 'W') {
                t->state = PS_SOS;
            } else if (ch == 'c') {
                // RIS — reset terminal
                term_init(t);
                t->state = PS_NORMAL;
            } else if (ch == 'M') {
                // RI — reverse index (move up)
                int top = t->scroll_top;
                if (t->cur_row > top) {
                    t->cur_row--;
                } else {
                    scroll_down(t);
                }
                t->state = PS_NORMAL;
            } else if (ch == 'E') {
                // NEL — move to next line
                t->cur_col = 0;
                newline(t);
                t->state = PS_NORMAL;
            } else if (ch == '7') {
                // DECSC — save cursor & attributes
                t->saved7_row = t->cur_row;
                t->saved7_col = t->cur_col;
                t->saved7_fg = t->cur_fg;
                t->saved7_bg = t->cur_bg;
                t->saved7_attrs = t->cur_attrs;
                t->state = PS_NORMAL;
            } else if (ch == '8') {
                // DECRC — restore cursor & attributes
                t->cur_row = SDL_clamp(t->saved7_row, 0, t->rows - 1);
                t->cur_col = SDL_clamp(t->saved7_col, 0, t->cols - 1);
                t->cur_fg = t->saved7_fg;
                t->cur_bg = t->saved7_bg;
                t->cur_attrs = t->saved7_attrs;
                t->state = PS_NORMAL;
            } else {
                t->state = PS_NORMAL;
            }
            break;

        case PS_CSI:
            if (ch >= '0' && ch <= '9') {
                if (t->csi_len < (int)sizeof(t->csi) - 1) t->csi[t->csi_len++] = (char)ch;
            } else if (ch == ';' || ch == '?' || ch == '>' || ch == '<' || ch == '=') {
                if (t->csi_len < (int)sizeof(t->csi) - 1) t->csi[t->csi_len++] = (char)ch;
            } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
                if (t->csi_len < (int)sizeof(t->csi) - 1) t->csi[t->csi_len++] = (char)ch;
                dispatch_csi(t);
                t->state = PS_NORMAL;
            } else if (ch == 0x1b) {
                t->state = PS_ESC;
            } else {
                // Unknown character in CSI; abort
                t->csi_len = 0;
                t->state = PS_NORMAL;
            }
            break;

        case PS_APC:
            if (ch == 0x1b) {
                t->apc_esc_pending = true;
            } else if (t->apc_esc_pending) {
                t->apc_esc_pending = false;
                if (ch == '\\') {
                    t->apc_buf[t->apc_len] = '\0';
                    if (g_kitty_enabled)
                        kitty_handle_apc(t, t->apc_buf, t->apc_len);
                    t->apc_len = 0;
                    t->state = PS_NORMAL;
                } else {
                    if (!t->apc_buf || t->apc_len >= t->apc_cap - 1) {
                        int new_cap = t->apc_cap ? t->apc_cap * 2 : 65536;
                        if (new_cap > 4*1024*1024) {
                            SDL_Log("[APC] buffer exceeded 4MB (apc_len=%d) — aborting sequence\n", t->apc_len);
                            t->apc_len = 0; t->state = PS_NORMAL; break;
                        }
                        t->apc_buf = (char*)realloc(t->apc_buf, new_cap);
                        t->apc_cap = new_cap;
                    }
                    if (t->apc_buf) t->apc_buf[t->apc_len++] = (char)ch;
                }
            } else {
                if (!t->apc_buf || t->apc_len >= t->apc_cap - 1) {
                    int new_cap = t->apc_cap ? t->apc_cap * 2 : 65536;
                    if (new_cap > 4*1024*1024) {
                        SDL_Log("[APC] buffer exceeded 4MB (apc_len=%d) — aborting sequence\n", t->apc_len);
                        t->apc_len = 0; t->state = PS_NORMAL; break;
                    }
                    t->apc_buf = (char*)realloc(t->apc_buf, new_cap);
                    t->apc_cap = new_cap;
                }
                if (t->apc_buf) t->apc_buf[t->apc_len++] = (char)ch;
            }
            break;
        // DCS / PM / SOS — all use the same rule: absorb everything until
        // ST (ESC \) or BEL.  Tmux sends DCS sequences constantly for its
        // passthrough and clipboard protocols.  Without this sink the payload
        // bytes reach PS_NORMAL and get misinterpreted as CSI/text, corrupting
        // the terminal state and eventually crashing.
        case PS_DCS:
        case PS_PM:
        case PS_SOS:
            if (ch == 0x07) {
                t->state = PS_NORMAL;  // BEL = ST shorthand
            } else if (ch == 0x1b) {
                t->apc_esc_pending = true;  // reuse flag — next char must be '\'
            } else if (t->apc_esc_pending) {
                t->apc_esc_pending = false;
                if (ch == '\\') t->state = PS_NORMAL;
                // else: not ST, keep sinking
            }
            break;
        case PS_OSC:
            if (ch == 0x07 || ch == 0x1b) {
                t->osc[t->osc_len] = '\0';
                const char *semi = strchr(t->osc, ';');
                if (semi) {
                    int ps = atoi(t->osc);
                    //SDL_Log("[OSC] ps=%d payload='%s'\n", ps, semi + 1);
                    if ((ps == 0 || ps == 2) && g_sdl_window)
                        SDL_SetWindowTitle(g_sdl_window, semi + 1);
                    else if (ps == 666)
                        basic_handle_osc(t, semi + 1, (int)(t->osc + t->osc_len - (semi + 1)),
                                         g_basic_win_w, g_basic_win_h);
                }
                t->osc_len = 0;
                t->state = (ch == 0x1b) ? PS_ESC : PS_NORMAL;
            } else {
                if (t->osc_len < (int)sizeof(t->osc) - 1)
                    t->osc[t->osc_len++] = (char)ch;
            }
            break;
        }

        // If the cursor moved, dirty the old and new rows so the cursor
        // is redrawn correctly. Covers arrow keys, cursor positioning,
        // newlines, backspace, tab — anything that changes cur_row or cur_col.
        if (t->cur_row != prev_row || t->cur_col != prev_col) {
            term_dirty_row(t, prev_row);
            term_dirty_row(t, t->cur_row);
        }
    }
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void term_init(Terminal *t) {
    memset(t, 0, sizeof(*t));
    t->cur_fg               = 7;
    t->pty_fd               = -1;
    t->child                = -1;
    t->state                = PS_NORMAL;
    t->cursor_on            = true;
    t->cursor_blink_enabled = true;
    t->cursor_shape         = 1;
    t->autowrap             = true;
    t->saved7_fg            = TCOLOR_PALETTE(7);
    t->saved7_bg            = TCOLOR_PALETTE(0);

    if (s_ft_face) {
        FT_Set_Pixel_Sizes(s_ft_face, 0, g_font_size);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, '0');
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT)) {
            t->cell_w = (float)(s_ft_face->glyph->advance.x >> 6);
            t->cell_h = (float)(int)(g_font_size * 1.4f);
        }
    }
    if (t->cell_w < 1) t->cell_w = 10;
    if (t->cell_h < 1) t->cell_h = 20;

    t->sb_cap = SCROLLBACK_LINES;
    t->cols   = TERM_COLS_DEFAULT;
    t->rows   = TERM_ROWS_DEFAULT;

    t->sb_buf = (Cell*)calloc(t->sb_cap * t->cols, sizeof(Cell));
    t->cells  = (Cell*)malloc(sizeof(Cell) * t->cols * t->rows);
    for (int i = 0; i < t->cols * t->rows; i++)
        t->cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};

    t->scroll_top = 0;
    t->scroll_bot = t->rows - 1;

    t->apc_buf         = nullptr;
    t->apc_len         = 0;
    t->apc_cap         = 0;
    t->apc_esc_pending = false;

    term_dirty_all(t);
    //SDL_Log("[Term] init: %dx%d cells %.0fx%.0f px\n", t->cols, t->rows, t->cell_w, t->cell_h);
}

void term_free(Terminal *t) {
    if (t->sb_buf) {
        free(t->sb_buf);
        t->sb_buf = nullptr;
    }
    if (t->cells) {
        free(t->cells);
        t->cells = nullptr;
    }
    if (t->alt_cells) {
        free(t->alt_cells);
        t->alt_cells = nullptr;
    }
    if (t->apc_buf) {
        free(t->apc_buf);
        t->apc_buf = nullptr;
    }
}

void term_resize(Terminal *t, int win_w, int win_h) {
    int new_cols = (int)((win_w - 4) / t->cell_w);
    int new_rows = (int)((win_h - 4) / t->cell_h);
    if (new_cols < 2) new_cols = 2;
    if (new_rows < 2) new_rows = 2;
    if (new_cols > TERM_MAX_COLS) new_cols = TERM_MAX_COLS;
    if (new_rows > TERM_MAX_ROWS) new_rows = TERM_MAX_ROWS;
    if (new_cols == t->cols && new_rows == t->rows) return;

    Cell *new_cells = (Cell*)malloc(sizeof(Cell) * new_cols * new_rows);
    for (int i = 0; i < new_cols * new_rows; i++)
        new_cells[i] = {' ', t->cur_fg, t->cur_bg, 0, {0,0,0}};

    int copy_rows = (t->rows < new_rows) ? t->rows : new_rows;
    int copy_cols = (t->cols < new_cols) ? t->cols : new_cols;
    for (int r = 0; r < copy_rows; r++)
        for (int c = 0; c < copy_cols; c++)
            new_cells[r * new_cols + c] = CELL(t, r, c);

    free(t->cells);
    t->cells = new_cells;

    if (t->alt_cells) { free(t->alt_cells); t->alt_cells = nullptr; t->in_alt_screen = false; }

    if (t->sb_buf) free(t->sb_buf);
    t->sb_buf   = (Cell*)calloc(t->sb_cap * new_cols, sizeof(Cell));
    t->sb_head  = 0; t->sb_count = 0; t->sb_offset = 0;
    t->cols = new_cols; t->rows = new_rows;

    if (t->cur_row >= t->rows) t->cur_row = t->rows - 1;
    if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;

#ifndef _WIN32
    if (t->pty_fd >= 0) {
        struct winsize ws = {
            .ws_row    = (unsigned short)new_rows,
            .ws_col    = (unsigned short)new_cols,
            .ws_xpixel = (unsigned short)(new_cols * (int)t->cell_w),
            .ws_ypixel = (unsigned short)(new_rows * (int)t->cell_h),
        };
        ioctl(t->pty_fd, TIOCSWINSZ, &ws);
    }
#else
    term_pty_resize(new_cols, new_rows);
#endif

    t->scroll_top = 0; t->scroll_bot = new_rows - 1;
    term_dirty_all(t);
    SDL_Log("[Term] resized to %dx%d\n", new_cols, new_rows);
}

// Update cell dimensions based on current g_font_size.
// Called during initialization after settings are loaded but before window is sized,
// and also by term_set_font_size() during normal operation.
void term_update_cell_dims(Terminal *t) {
    if (s_ft_face) {
        FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)g_font_size);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, '0');
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT)) {
            t->cell_w = (float)(s_ft_face->glyph->advance.x >> 6);
            t->cell_h = (float)(int)(g_font_size * 1.4f);
        }
    }
    if (t->cell_w < 1) t->cell_w = 6;
    if (t->cell_h < 1) t->cell_h = 8;
}

void term_set_font_size(Terminal *t, int new_size, int win_w, int win_h) {
    if (new_size < FONT_SIZE_MIN) new_size = FONT_SIZE_MIN;
    if (new_size > FONT_SIZE_MAX) new_size = FONT_SIZE_MAX;
    if (new_size == g_font_size) return;

    g_font_size = new_size;
    term_update_cell_dims(t);
    term_resize(t, win_w, win_h);
    SDL_Log("[Term] font size %d, cell %.0fx%.0f, grid %dx%d\n", g_font_size, t->cell_w, t->cell_h, t->cols, t->rows);
}

// USAGE IN felixterminal.cpp:
// Add this line in the shutdown section (around line 2248, before ft_shutdown()):
//    term_free(&term);
//
// Example:
//    kitty_shutdown();
//    basic_graphics_shutdown();
//    menu_font_shutdown();
//    term_free(&term);  // <-- ADD THIS LINE
//    if (use_ssh) { ... }
