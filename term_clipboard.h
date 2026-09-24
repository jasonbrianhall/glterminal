#pragma once
#include "terminal.h"

// Copy the selection as rich text (HTML + plain text) so pasting into
// Word/LibreOffice keeps bold, colors, underline, etc.
void term_copy_selection_rich(Terminal *t);
