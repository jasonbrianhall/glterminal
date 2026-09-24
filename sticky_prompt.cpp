#include "sticky_prompt.h"
#include "terminal.h"
#include "term_color.h"
#include "gl_renderer.h"
#include "ft_font.h"
#include "kitty_graphics.h"
#include "sixel_graphics.h"
#include <cstdio>
#include <functional>
#include <SDL2/SDL.h>
#include <string.h>

// Forward declarations for functions from other modules
extern int g_font_size;
extern bool g_blink_text_on;
extern bool s_basic_palette_active;
extern bool cell_in_sel(Terminal *t, int r, int c);
extern bool g_line_numbers_enabled;
// Shared cell-drawing helpers from term_ui.cpp (same code as term_render)
extern void term_draw_decorations(const Cell *c, float px, float py, float cw, float ch, TermColor fc);
extern bool term_draw_block_element(uint32_t cp, float px, float py, float cw, float ch, TermColor fc);
extern void term_detect_urls(Terminal *t, std::function<Cell*(int row, int col)> resolve_cell);
extern void term_draw_url_underline(int row, int col, float px, float py, float cw, float ch);

// ============================================================================
// STATE
// ============================================================================

bool g_sticky_prompt_enabled = false;

// ============================================================================
// TOGGLE
// ============================================================================

void sticky_prompt_toggle() {
    g_sticky_prompt_enabled = !g_sticky_prompt_enabled;
}

// ============================================================================
// GET INPUT LINE
// ============================================================================

Cell* sticky_prompt_get_input_line(Terminal *t) {
    return &CELL(t, t->rows - 1, 0);
}

// ============================================================================
// SPLIT RENDERING
// Rows 0..rows-2 follow the scrollback offset; row rows-1 always shows the
// live bottom line. Cell drawing uses the same helpers as term_render() so
// both modes look identical (block elements, underline styles, links,
// wide characters, images).
// ============================================================================

void sticky_prompt_render_split(Terminal *t, int ox, int oy) {
    float cw = t->cell_w, ch = t->cell_h;

    // Adjust ox for line numbers if enabled
    float line_num_width = 0;
    if (g_line_numbers_enabled) {
        line_num_width = cw * 6;  // 6 character widths for line numbers
        ox += line_num_width;  // Shift terminal to the right
    }

    Cell blank = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    const int input_row = t->rows - 1;

    // Screen row -> cell actually shown there
    auto resolve_cell = [&](int row, int col) -> Cell* {
        if (row == input_row) return &CELL(t, input_row, col);
        if (t->sb_offset > 0) {
            int sb_row_idx = t->sb_count - t->sb_offset + row;
            if (sb_row_idx < 0) return &blank;
            if (sb_row_idx < t->sb_count) return sb_row(t, sb_row_idx) + col;
            int live_row = sb_row_idx - t->sb_count;
            return (live_row < t->rows) ? &CELL(t, live_row, col) : &blank;
        }
        return &CELL(t, row, col);
    };
    // Screen row -> virtual row (selection coordinates)
    auto vrow_of = [&](int row) {
        return row == input_row ? t->sb_count + input_row
                                : row + t->sb_count - t->sb_offset;
    };
    // The input row is redrawn every frame (it's cheap and can change
    // without its own dirty flag while scrolled); other rows only when dirty.
    auto row_needs_draw = [&](int row) {
        return row == input_row || term_row_is_dirty(t, row);
    };

    term_detect_urls(t, resolve_cell);

    // Pass 1: backgrounds
    for (int row = 0; row < t->rows; row++) {
        if (!row_needs_draw(row)) continue;
        float py = oy + row * ch;
        int vrow = vrow_of(row);
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col * cw;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            if (cell_in_sel(t, vrow, col)) {
                // Same look as term_render(): selection = inverted colors
                TermColor sc = tcolor_resolve(fg);
                draw_rect(px, py, cw, ch, sc.r, sc.g, sc.b, 1.0f);
            } else {
                TermColor bc = tcolor_resolve(bg);
                float bg_alpha = (s_basic_palette_active && bg == TCOLOR_PALETTE(0)) ? 0.f : 1.f;
                draw_rect(px, py, cw, ch, bc.r, bc.g, bc.b, bg_alpha);
            }
        }
    }

    // Pass 2: glyphs, decorations, link underlines
    for (int row = 0; row < t->rows; row++) {
        if (!row_needs_draw(row)) continue;
        float py = oy + row * ch;
        int vrow = vrow_of(row);
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col * cw;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor fc = tcolor_resolve(fg);
            if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
                fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg)+8));
            if (c->attrs & ATTR_DIM) { fc.r *= 0.5f; fc.g *= 0.5f; fc.b *= 0.5f; }
            if (cell_in_sel(t, vrow, col)) fc = tcolor_resolve(bg);

            uint32_t cp = c->cp;
            bool blink_hidden = ((c->attrs & ATTR_BLINK) && !g_blink_text_on) || cell_is_hidden(c);
            if (cp && cp != ' ' && !blink_hidden) {
                if (!term_draw_block_element(cp, px, py, cw, ch, fc)) {
                    char tmp[5] = {};
                    cp_to_utf8(cp, tmp);
                    float baseline = py + ch * 0.82f;
                    draw_text(tmp, px, baseline, g_font_size, (int)ch, fc.r, fc.g, fc.b, 1.f, c->attrs,
                              cell_is_wide(c) ? cw * 2.f : 0.f);
                }
            }
            if (!blink_hidden)
                term_draw_decorations(c, px, py, cw, ch, fc);
            term_draw_url_underline(row, col, px, py, cw, ch);
        }
    }

    // Images, clipped so they never cover the fixed input line
    kitty_render(t, ox, oy, input_row);
    sixel_render(t, ox, oy, input_row);

    // Cursor: on the input line when it's there; when not scrolled back the
    // screen is live, so show it wherever it is (e.g. top row after `clear`)
    if (term_cursor_visible(t) &&
        (t->cur_row == input_row || t->sb_offset == 0)) {
        float cx = ox + t->cur_col * cw;
        float cy = oy + t->cur_row * ch;
        switch (t->cursor_shape) {
        case 0: draw_rect(cx, cy, cw, ch, 1,1,1, 0.3f); break;
        case 2: draw_rect(cx, cy, 2, ch, 1,1,1, 0.85f); break;
        default: draw_rect(cx, cy+ch-3, cw, 3, 1,1,1, 0.85f); break;
        }
    }

    // Draw scrollbar (if scrolled)
    if (t->sb_offset > 0 && t->sb_count > 0) {
        float win_h = (t->rows - 1) * ch;
        int total_rows = t->sb_count + t->rows;
        float bar_h = win_h * (t->rows - 1) / total_rows;
        if (bar_h < 8) bar_h = 8;
        float bar_y = oy + (win_h - bar_h) * (float)(total_rows - (t->rows-1) - t->sb_offset) / (total_rows - (t->rows-1));
        float bar_x = ox + t->cols * cw - 4;
        draw_rect(bar_x, oy, 4, win_h, 0,0,0, 0.3f);
        draw_rect(bar_x, bar_y, 4, bar_h, 0.6f, 0.6f, 0.7f, 0.8f);
    }

    // Draw line numbers (if enabled)
    if (g_line_numbers_enabled) {
        for (int row = 0; row < t->rows; row++) {
            int vrow = vrow_of(row);
            char line_num[16];
            snprintf(line_num, sizeof(line_num), "%5d", vrow);
            // Draw at original ox position (before we shifted right)
            float line_num_x = ox - line_num_width;
            float line_num_y = oy + row * ch;
            draw_text(line_num, line_num_x, line_num_y + ch * 0.82f, g_font_size, (int)ch, 0.6f, 0.6f, 0.7f, 0.8f, 0);
        }
    }

    // Clear dirty flags
    term_clear_dirty(t);
}
