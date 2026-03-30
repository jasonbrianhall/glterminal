#include "terminal.h"

// BASIC color to ANSI color mapping
// BASIC: 0=black, 1=blue, 2=green, 3=cyan, 4=red, 5=magenta, 6=yellow, 7=white
//        8-15 are bright versions
static int ansi_fg_map[] = {
  30,  // 0: black
  34,  // 1: blue
  32,  // 2: green
  36,  // 3: cyan
  31,  // 4: red
  35,  // 5: magenta
  33,  // 6: yellow
  37,  // 7: white
  90,  // 8: bright black (gray)
  94,  // 9: bright blue
  92,  // 10: bright green
  96,  // 11: bright cyan
  91,  // 12: bright red
  95,  // 13: bright magenta
  93,  // 14: bright yellow
  97   // 15: bright white
};

static int ansi_bg_map[] = {
  40,  // 0: black
  44,  // 1: blue
  42,  // 2: green
  46,  // 3: cyan
  41,  // 4: red
  45,  // 5: magenta
  43,  // 6: yellow
  47,  // 7: white
  100, // 8: bright black (gray)
  104, // 9: bright blue
  102, // 10: bright green
  106, // 11: bright cyan
  101, // 12: bright red
  105, // 13: bright magenta
  103, // 14: bright yellow
  107  // 15: bright white
};

void term_color(int foreground, int background) {
  if (foreground < 0 || foreground > 15)
    foreground = 7;
  if (background < 0 || background > 15)
    background = 0;

  printf("\033[%d;%dm", ansi_fg_map[foreground], ansi_bg_map[background]);
}

void term_cls(void) {
  printf("\033[2J");    // clear entire screen
  printf("\033[H");     // move cursor to home (1,1)
}

void term_locate(int row, int col) {
  // BASIC uses 1-based indexing; ANSI uses same
  if (row < 1) row = 1;
  if (col < 1) col = 1;
  printf("\033[%d;%dH", row, col);
}

void term_width(int cols) {
  // Try to set terminal width (may not work everywhere)
  // For now, just accept the command without doing much
  (void)cols;  // unused
}

void term_screen(int mode) {
  // SCREEN mode switching (text modes)
  // For simplicity, just clear the screen
  term_cls();
  (void)mode;  // unused
}

void term_reset(void) {
  printf("\033[0m");    // reset all attributes
}
