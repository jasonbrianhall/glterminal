#pragma once
#include "terminal.h"
#include <stdbool.h>

bool term_spawn(Terminal *t, const char *cmd);
bool term_read(Terminal *t);
void term_write(Terminal *t, const char *s, int n);
