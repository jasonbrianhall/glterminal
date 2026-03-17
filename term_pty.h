#pragma once
#include "terminal.h"
#include <stdbool.h>

bool term_spawn(Terminal *t, const char *cmd);
bool term_read(Terminal *t);
void term_write(Terminal *t, const char *s, int n);

// Optional write override — when set, term_write() calls this instead of
// writing to pty_fd.  Used by the SSH layer to intercept all output
// (including handle_key / term_paste) without modifying term_ui.cpp.
// Set to nullptr to restore normal PTY behaviour.
extern void (*g_term_write_override)(Terminal *t, const char *s, int n);

#ifdef _WIN32
bool term_child_exited(void);
void term_pty_resize(int cols, int rows);
#endif
