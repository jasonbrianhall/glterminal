#ifndef TERMINAL_H
#define TERMINAL_H

#include <cstdio>
#include <string>

using namespace std;

// ANSI color codes (BASIC colors 0-15 mapped to standard terminal colors)
void term_color(int foreground, int background);
void term_cls(void);
void term_locate(int row, int col);
void term_width(int cols);
void term_screen(int mode);

#endif // TERMINAL_H
