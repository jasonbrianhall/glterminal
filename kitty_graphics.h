#pragma once
#include "terminal.h"
#include <stdint.h>

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

// Drop all images associated with this terminal (e.g. on reset / alt-screen swap).
void kitty_clear(Terminal *t);

// Called by scroll_up() — shift all placement y_cells up by `lines`,
// removing any that scroll off the top.
void kitty_scroll(Terminal *t, int lines);

// Free GL resources on shutdown.
void kitty_shutdown(void);
