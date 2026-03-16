#pragma once
#include "terminal.h"
#include <stdint.h>
#include <string>
#include <vector>

// ============================================================================
// KITTY GRAPHICS PROTOCOL
// Spec: https://sw.kovidgoyal.net/kitty/graphics-protocol/
// ============================================================================

// Call once after gl_init_renderer().
void kitty_init(void);

// Feed a complete APC payload (everything between ESC_ and ESC\, not
// including the delimiters themselves).  Called from term_feed().
void kitty_handle_apc(Terminal *t, const char *payload, int len);

// Render all placed images for this terminal into the current FBO.
// Call from term_render() after the glyph pass, before gl_flush_verts().
void kitty_render(Terminal *t, int ox, int oy);

// For HTML copy: returns <img> tags for any placements whose y_cell falls
// within [row_start, row_end] (inclusive, virtual row coordinates matching
// the selection). Each entry is keyed by y_cell so the caller can insert
// them at the right line break. Call once per copy operation.
struct KittyHtmlImage {
    int         y_cell;
    int         cols;
    std::string img_tag;
};
std::vector<KittyHtmlImage> kitty_get_html_images(Terminal *t, int row_start, int row_end);

// Drop all images associated with this terminal (e.g. on reset / alt-screen swap).
void kitty_clear(Terminal *t);

// Called by scroll_up() — shift all placement y_cells up by `lines`,
// removing any that scroll off the top.
void kitty_scroll(Terminal *t, int lines);

// Free GL resources on shutdown.
void kitty_shutdown(void);
