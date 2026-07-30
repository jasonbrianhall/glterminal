#pragma once

#ifdef USESSH

#include <SDL2/SDL.h>

// F4 — interactive SFTP console (cd, ls, get, mget, put, mput, pwd, mkdir,
// rmdir, rm, rename, chmod).  Shares the live libssh2 session opened by
// ssh_session.cpp and the SFTP subsystem from sftp_overlay.cpp.
//
// Transfers run on background threads; render() polls progress; the console
// remains responsive to input (ls, pwd, etc) while transfers complete.
// Press Escape to cancel an in-progress transfer.

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

// Mouse wheel scroll — delta_y > 0 = wheel up.
void sftp_console_scroll(int delta_y);

// Must be called before sftp_shutdown() to wait for any background transfer.
void sftp_console_join();

// Reset console state after fork().
// Call in child process before any console operations.
void sftp_console_reset_after_fork();

// Returns true if a transfer is currently in progress.
bool sftp_console_transfer_in_progress();

// Cancel any running transfer.
void sftp_console_cancel_transfer();

#endif // USESSH
