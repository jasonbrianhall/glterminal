#pragma once
#include "terminal.h"
#include <stdbool.h>

bool term_spawn(Terminal *t, const char *cmd);
bool term_read(Terminal *t);
void term_write(Terminal *t, const char *s, int n);

#ifdef _WIN32
bool term_child_exited(void);
void term_pty_resize(int cols, int rows);
#endif
