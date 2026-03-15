#pragma once
#include "terminal.h"
#include <string>

// ============================================================================
// URL DETECTION
// ============================================================================

// Update hover state based on mouse position. Returns true if hover changed (needs redraw).
bool        url_update_hover(Terminal *t, int mouse_px, int mouse_py, int ox, int oy);
// Returns the URL string under the given pixel, or empty string if none.
std::string url_at_pixel(Terminal *t, int mouse_px, int mouse_py, int ox, int oy);
// Returns index of currently hovered URL span, or -1. Use instead of url_at_pixel for cursor decisions.
int         term_hovered_url_index();
// Open a URL with xdg-open.
void        open_url(const std::string &url);

// ============================================================================
// SELECTION / CLIPBOARD
// ============================================================================

void pixel_to_cell(Terminal *t, int px, int py, int ox, int oy, int *row, int *col);
bool cell_in_sel(Terminal *t, int r, int c);

void term_select_all(Terminal *t);
void term_copy_selection(Terminal *t);
void term_copy_selection_html(Terminal *t);
void term_copy_selection_ansi(Terminal *t);
void term_paste(Terminal *t);

// ============================================================================
// RENDERING
// ============================================================================

void term_render(Terminal *t, int ox, int oy);

// ============================================================================
// KEYBOARD
// ============================================================================

// Forward: SDL types used in signature
#include <SDL2/SDL.h>
void handle_key(Terminal *t, SDL_Keysym ks, const char *text);

// ============================================================================
// CONTEXT MENU
// ============================================================================

#define MENU_ID_NEW_TERMINAL  0
#define MENU_ID_COPY          2
#define MENU_ID_COPY_HTML     3
#define MENU_ID_COPY_ANSI     4
#define MENU_ID_PASTE         5
#define MENU_ID_RESET         7
#define MENU_ID_THEMES        9
#define MENU_ID_OPACITY      10
#define MENU_ID_RENDER_MODE  11
#define MENU_ID_FIGHT_MODE   12
#define MENU_ID_BOUNCING_CIRCLE 13
#define MENU_ID_SELECT_ALL   14
#define MENU_ID_QUIT         15

struct MenuItem {
    const char *label;
    bool        separator;
};

struct ContextMenu {
    bool  visible;
    int   x, y;
    int   hovered;
    int   item_h, sep_h, pad_x, width;
    int   sub_open;
    int   sub_x, sub_y, sub_w, sub_h;
    int   sub_hovered;
};

extern const MenuItem MENU_ITEMS[];
extern const int MENU_COUNT;

extern ContextMenu g_menu;

void menu_open(ContextMenu *m, int x, int y, int win_w, int win_h);
int  menu_hit(ContextMenu *m, int px, int py);
int  submenu_hit(ContextMenu *m, int px, int py);
void menu_render(ContextMenu *m);

void action_new_terminal();

#include "fight_mode.h"

