#pragma once
#include "terminal.h"
#include <stdint.h>

// ============================================================================
// STICKY PROMPT (CORRECT VERSION)
// 
// When enabled (F8), the bottom line (prompt/input line) stays fixed at the
// bottom of the screen while the rest of the scrollback scrolls behind it.
// 
// Effect:
//   Scroll up in history → everything scrolls BUT the input prompt stays at bottom
//   Type a command → input appears in the fixed prompt line
//   Press ENTER → command executes normally
//   Scroll more → input line never leaves, always visible
//
// This allows typing commands while viewing old output without losing
// the prompt or having to scroll back down.
// ============================================================================

extern bool g_sticky_prompt_enabled;

// Toggle sticky prompt on/off (called by F8 handler)
void sticky_prompt_toggle();

// Called from term_render() INSTEAD of normal rendering when sticky is active
// Renders: scrollback (rows 0..rows-2) with normal scrolling
//          bottom row fixed (prompt/input line at bottom of screen)
void sticky_prompt_render_split(Terminal *t, int ox, int oy);

// Helper: get the current input line (bottom row of terminal)
// Returns: cells from the current bottom line (where prompt is)
Cell* sticky_prompt_get_input_line(Terminal *t);
