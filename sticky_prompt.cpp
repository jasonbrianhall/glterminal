#include "sticky_prompt.h"
#include "terminal.h"
#include "term_color.h"
#include "gl_renderer.h"
#include <SDL2/SDL.h>
#include <string.h>

// Forward declarations for functions from other modules
extern void draw_text(const char *text, float x, float y, int font_size, int cell_h, 
                      float r, float g, float b, float a, uint8_t attrs);
extern void cp_to_utf8(uint32_t cp, char *buf);
extern int g_font_size;
extern int g_blink_text_on;
extern bool s_basic_palette_active;

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
// ============================================================================

void sticky_prompt_render_split(Terminal *t, int ox, int oy) {
    float cw = t->cell_w, ch = t->cell_h;
    
    Cell blank = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    
    // ========================================================================
    // PART 1: Render scrollable area (rows 0 through rows-2)
    // ========================================================================
    
    auto resolve_cell_scrollable = [&](int row, int col) -> Cell* {
        if (t->sb_offset > 0) {
            int sb_row_idx = t->sb_count - t->sb_offset + row;
            if (sb_row_idx < 0) return &blank;
            if (sb_row_idx < t->sb_count) return sb_row(t, sb_row_idx) + col;
            int live_row = sb_row_idx - t->sb_count;
            return (live_row < t->rows) ? &CELL(t, live_row, col) : &blank;
        }
        return &CELL(t, row, col);
    };
    
    // Render backgrounds for scrollable rows (0..rows-2)
    for (int row = 0; row < t->rows - 1; row++) {
        if (!term_row_is_dirty(t, row)) continue;
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell_scrollable(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor bc = tcolor_resolve(bg);
            float bg_alpha = (s_basic_palette_active && bg == TCOLOR_PALETTE(0)) ? 0.f : 1.f;
            draw_rect(px, py, cw, ch, bc.r, bc.g, bc.b, bg_alpha);
        }
    }
    
    // Render glyphs for scrollable rows
    for (int row = 0; row < t->rows - 1; row++) {
        if (!term_row_is_dirty(t, row)) continue;
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell_scrollable(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor fc = tcolor_resolve(fg);
            if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
                fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg)+8));
            if (c->attrs & ATTR_DIM) { fc.r *= 0.5f; fc.g *= 0.5f; fc.b *= 0.5f; }
            
            uint32_t cp = c->cp;
            bool blink_hidden = (c->attrs & ATTR_BLINK) && !g_blink_text_on;
            if (cp && cp != ' ' && !blink_hidden) {
                char tmp[5] = {};
                cp_to_utf8(cp, tmp);
                float baseline = py + ch * 0.82f;
                draw_text(tmp, px, baseline, g_font_size, (int)ch, fc.r, fc.g, fc.b, 1.f, c->attrs);
            }
            if ((c->attrs & ATTR_UNDERLINE) && !blink_hidden)
                draw_rect(px, py+ch-2, cw, 2, fc.r, fc.g, fc.b, 1.f);
        }
    }
    
    // ========================================================================
    // PART 2: Render fixed input line (row rows-1, ALWAYS at bottom)
    // ========================================================================
    
    int input_row = t->rows - 1;
    float input_y = oy + input_row * ch;
    
    // Background for input line
    for (int col = 0; col < t->cols; col++) {
        float px = ox + col*cw;
        Cell *c = &CELL(t, input_row, col);
        TermColorVal fg = c->fg, bg = c->bg;
        if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
        TermColor bc = tcolor_resolve(bg);
        float bg_alpha = (s_basic_palette_active && bg == TCOLOR_PALETTE(0)) ? 0.f : 1.f;
        draw_rect(px, input_y, cw, ch, bc.r, bc.g, bc.b, bg_alpha);
    }
    
    // Glyphs for input line
    for (int col = 0; col < t->cols; col++) {
        float px = ox + col*cw;
        Cell *c = &CELL(t, input_row, col);
        TermColorVal fg = c->fg, bg = c->bg;
        if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
        TermColor fc = tcolor_resolve(fg);
        if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
            fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg)+8));
        if (c->attrs & ATTR_DIM) { fc.r *= 0.5f; fc.g *= 0.5f; fc.b *= 0.5f; }
        
        uint32_t cp = c->cp;
        bool blink_hidden = (c->attrs & ATTR_BLINK) && !g_blink_text_on;
        if (cp && cp != ' ' && !blink_hidden) {
            char tmp[5] = {};
            cp_to_utf8(cp, tmp);
            float baseline = input_y + ch * 0.82f;
            draw_text(tmp, px, baseline, g_font_size, (int)ch, fc.r, fc.g, fc.b, 1.f, c->attrs);
        }
        if ((c->attrs & ATTR_UNDERLINE) && !blink_hidden)
            draw_rect(px, input_y+ch-2, cw, 2, fc.r, fc.g, fc.b, 1.f);
    }
    
    // Draw cursor if visible (only in the fixed input line)
    if (t->cursor_on && input_row == t->cur_row) {
        float cx = ox + t->cur_col * cw;
        switch (t->cursor_shape) {
        case 0: draw_rect(cx, input_y, cw, ch, 1,1,1, 0.3f); break;
        case 2: draw_rect(cx, input_y, 2, ch, 1,1,1, 0.85f); break;
        default: draw_rect(cx, input_y+ch-3, cw, 3, 1,1,1, 0.85f); break;
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
    
    // Clear dirty flags
    term_clear_dirty(t);
}
