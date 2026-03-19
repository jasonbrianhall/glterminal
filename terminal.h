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

typedef enum { PS_NORMAL, PS_ESC, PS_CSI, PS_OSC, PS_CHARSET, PS_APC } ParseState;

struct Terminal {
    Cell         *cells;
    int           cols, rows;
    int           cur_row, cur_col;
    TermColorVal  cur_fg, cur_bg;
    uint8_t       cur_attrs;
    ParseState    state;
    char          csi[256];
    int           csi_len;
    char          osc[512];
    int           osc_len;
    // APC buffer — for Kitty graphics protocol (ESC _ ... ESC \)
    char         *apc_buf;
    int           apc_len;
    int           apc_cap;
    bool          apc_esc_pending;  // saw ESC inside APC, waiting for '\' to complete ST
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
void term_resize(Terminal *t, int win_w, int win_h);
void term_set_font_size(Terminal *t, int new_size, int win_w, int win_h);
void term_newline(Terminal *t);  // advance cursor one line, scrolling if needed

// ============================================================================
// VT100 PARSER
// ============================================================================

void term_feed(Terminal *t, const char *buf, int len);

// ============================================================================
// SCROLLBACK ACCESSORS
// ============================================================================

Cell* sb_row(Terminal *t, int idx);
Cell* vcell(Terminal *t, int vrow, int col);
