#include "terminal.h"
#include "term_pty.h"   // term_write, term_feed

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
                // RIS — reset terminal (do NOT call term_init here: that
                // wipes pty_fd/child and kills the live shell connection)
                term_soft_reset(t);
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
