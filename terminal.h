#pragma once
#include "gl_terminal.h"  // ATTR_* defines
#include "term_color.h"
#include <stdint.h>
#include <sys/types.h>

// ============================================================================
// CELL
// ============================================================================

struct Cell {
    uint32_t     cp;
    TermColorVal fg, bg;
    uint8_t      attrs, _pad[3];
};

#define CELL(t,r,c) ((t)->cells[(r)*(t)->cols+(c)])

// ============================================================================
// TERMINAL
// ============================================================================

typedef enum { PS_NORMAL, PS_ESC, PS_CSI, PS_OSC, PS_CHARSET, PS_APC, PS_DCS, PS_PM, PS_SOS } ParseState;

struct Terminal {
    Cell         *cells;
    int           cols, rows;
    int           cur_row, cur_col;
    TermColorVal  cur_fg, cur_bg;
    uint8_t       cur_attrs;
    ParseState    state;
    char          charset_slot;   // which Gn ('(' ')' '*' '+') a PS_CHARSET byte designates
    bool          g0_line_drawing; // true if G0 currently designated as DEC special graphics
    char          csi[256];
    int           csi_len;
    char          osc[512];
    int           osc_len;
    // UTF-8 decode state — persists across term_feed() calls since a
    // multi-byte sequence can be split across separate reads.
    uint32_t      utf8_cp;
    int           utf8_remaining;   // continuation bytes still expected
    // APC buffer — for Kitty graphics protocol (ESC _ ... ESC \)
    char         *apc_buf;
    int           apc_len;
    int           apc_cap;
    bool          apc_esc_pending;  // saw ESC inside APC, waiting for '\' to complete ST
    // DCS buffer — for Sixel graphics (ESC P ... ESC \) and passthrough sinks
    // (tmux, etc). Params (before the sixel 'q' introducer) and body are
    // split at dispatch time in term_feed().
    char         *dcs_buf;
    int           dcs_len;
    int           dcs_cap;
    int           dcs_params_len; // bytes in dcs_buf before the sixel body starts
    bool          dcs_is_sixel;    // set once we see the 'q' introducer
    bool          dcs_determined;  // true once we've decided sixel vs. plain sink
    int           pty_fd;
    pid_t         child;
    float         cell_w, cell_h;
    double        blink;
    double        cursor_blink;
    bool          cursor_on;
    bool          cursor_blink_enabled;
    int           cursor_shape;        // 0=block, 1=underline bar, 2=beam
    bool          autowrap;
    bool          mouse_report;
    bool          bracketed_paste;
    bool          app_cursor_keys;
    bool          mouse_sgr;
    // ESC 7/8 saved cursor
    int           saved7_row, saved7_col;
    TermColorVal  saved7_fg, saved7_bg;
    uint8_t       saved7_attrs;
    // Scroll region (DECSTBM, 0-based inclusive)
    int           scroll_top, scroll_bot;
    // Alternate screen
    Cell         *alt_cells;
    int           saved_cur_row, saved_cur_col;
    TermColorVal  saved_cur_fg, saved_cur_bg;
    uint8_t       saved_cur_attrs;
    bool          in_alt_screen;
    // Scrollback ring buffer
    Cell         *sb_buf;
    int           sb_cap;
    int           sb_cols;     // Width of scrollback buffer (never shrinks, only grows)
    int           sb_head;
    int           sb_count;
    int           sb_offset;   // 0=live, N=N rows back
    // Selection
    int           sel_start_row, sel_start_col;
    int           sel_end_row,   sel_end_col;
    bool          sel_active;
    bool          sel_exists;

    // Per-row dirty flags — set by terminal.cpp on any cell write,
    // cleared by term_render after each row is drawn.
    // Use uint8_t array; TERM_MAX_ROWS is 256 so this is 256 bytes.
    uint8_t       dirty_rows[TERM_MAX_ROWS];
    bool          all_dirty;   // when true, skip per-row check and redraw everything
};

// ============================================================================
// DIRTY ROW HELPERS
// ============================================================================

static inline void term_dirty_row(Terminal *t, int row) {
    if (row >= 0 && row < t->rows) t->dirty_rows[row] = 1;
}
static inline void term_dirty_rows(Terminal *t, int top, int bot) {
    for (int r = top; r <= bot && r < t->rows; r++) t->dirty_rows[r] = 1;
}
static inline void term_dirty_all(Terminal *t) {
    t->all_dirty = true;
}
static inline bool term_row_is_dirty(Terminal *t, int row) {
    return t->all_dirty || (row >= 0 && row < t->rows && t->dirty_rows[row]);
}
static inline void term_clear_dirty(Terminal *t) {
    t->all_dirty = false;
    for (int r = 0; r < t->rows; r++) t->dirty_rows[r] = 0;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void term_init(Terminal *t);
// RIS (ESC c) and the UI "Reset" action: clears the screen/attributes back
// to defaults WITHOUT touching pty_fd/child or reallocating at the wrong
// size — unlike term_init(), which is for first-time setup only.
void term_soft_reset(Terminal *t);
void term_resize(Terminal *t, int win_w, int win_h);
void term_set_font_size(Terminal *t, int new_size, int win_w, int win_h);
void term_update_cell_dims(Terminal *t);  // Update cell dimensions without resizing grid
void term_newline(Terminal *t);  // advance cursor one line, scrolling if needed
void term_free(Terminal *t);
// ============================================================================
// VT100 PARSER
// ============================================================================

void term_feed(Terminal *t, const char *buf, int len);

// Kitty graphics protocol — disable for SSH sessions to prevent APC crashes.
extern bool g_kitty_enabled;

// ============================================================================
// SCROLLBACK ACCESSORS
// ============================================================================

Cell* sb_row(Terminal *t, int idx);
Cell* vcell(Terminal *t, int vrow, int col);
