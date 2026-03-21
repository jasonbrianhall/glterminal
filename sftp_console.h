#pragma once

#ifdef USESSH

#include <SDL2/SDL.h>

// F4 — interactive SFTP console (cd, ls, get, mget, put, mput, pwd, mkdir,
// rmdir, rm, rename, chmod).  Shares the live libssh2 session opened by
// ssh_session.cpp and the SFTP subsystem from sftp_overlay.cpp.

// True while the console is visible (suppresses normal key-to-terminal routing).
extern bool g_sftp_console_visible;

// Called once when the SSH session is first established (safe to call again
// on re-connect — it is idempotent).
void sftp_console_open(int win_w, int win_h);
void sftp_console_close();

// Main-loop hooks — call every frame.
void sftp_console_render(int win_w, int win_h);

// Returns true if the key was consumed.
bool sftp_console_keydown(SDL_Keysym ks, const char *text_input);

// Mouse handlers — call from SDL_MOUSEBUTTONDOWN/UP/MOTION when console is visible.
// button is SDL_BUTTON_LEFT / SDL_BUTTON_RIGHT etc.
bool sftp_console_mousedown(int x, int y, int button);
bool sftp_console_mousemotion(int x, int y, bool lbutton);
bool sftp_console_mouseup(int x, int y);

// Must be called before sftp_shutdown() if a background transfer may be running.
void sftp_console_join();

#endif // USESSH
