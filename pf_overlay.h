#pragma once

#ifdef USESSH

#include <SDL2/SDL.h>

// F6 overlay — view and manage dynamic port forwards.
// Shows all active local (-L) and remote (-R) forwards with live
// connection counts, and lets the user add or remove them at runtime.

struct PfOverlay {
    bool visible  = false;
    int  selected = 0;    // highlighted row in the list

    // Input field for adding a new forward
    bool  input_active = false;
    bool  skip_next_textinput = false;  // swallow the SDL_TEXTINPUT paired with L/R keydown
    char  input_buf[256] = {};
    int   input_len      = 0;
    // 'L' or 'R' — which type is being added
    char  input_type     = 'L';

    char  status[256]    = {};   // feedback line
    bool  status_ok      = true;
};

extern PfOverlay g_pf_overlay;

void pf_overlay_open();
void pf_overlay_render(int win_w, int win_h);
bool pf_overlay_keydown(SDL_Keycode sym);
void pf_overlay_textinput(const char *text);

#endif // USESSH
