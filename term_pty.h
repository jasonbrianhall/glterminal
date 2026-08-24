#pragma once
#include "terminal.h"
#include <stdbool.h>

// TERM/ttype value used when spawning the local child shell (term_pty.cpp /
// term_pty_win.cpp) and when requesting a PTY over SSH (ssh_session.cpp).
// Defaults to "xterm-256color"; overridable via --term on the command line.
extern const char *g_term_type;

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
