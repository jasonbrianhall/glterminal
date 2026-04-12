#pragma once

#ifdef USESSH

#include "terminal.h"
#include <SDL2/SDL.h>
#include <string>
#include <vector>

enum class SftpOverlayMode { NONE, UPLOAD, DOWNLOAD };

struct SftpPanel {
    char path[4096] = {};
    struct Entry {
        char     name[512] = {};
        bool     is_dir    = false;
        uint64_t size      = 0;
    };
    std::vector<Entry> entries;
    int  selected   = 0;
    int  scroll_top = 0;
    bool is_remote  = false;
};

struct SftpOverlay {
    SftpOverlayMode mode    = SftpOverlayMode::NONE;
    bool            visible = false;

    SftpPanel left;    // always local
    SftpPanel right;   // always remote

    int  focused_panel = 0;   // 0 = left, 1 = right

    // Transfer state
    char  status[512]  = {};
    bool  transfer_ok  = false;
    float progress     = 0.f;  // 0..1
    bool  transferring = false;

    int visible_rows = 0;
};

extern SftpOverlay g_sftp;

// Open the overlay. remote_cwd = initial path for the remote panel.
void sftp_overlay_open(SftpOverlayMode mode, const char *remote_cwd, int win_w, int win_h);

// Refresh one panel's file list.
void sftp_panel_refresh(SftpPanel &p);

// Navigate into the selected directory of a panel.
void sftp_panel_enter(SftpPanel &p);

// Execute the transfer (blocking, renders progress mid-transfer).
void sftp_overlay_transfer();

// Render the overlay (also called mid-transfer for progress updates).
void sftp_overlay_render(int win_w, int win_h);

// Handle SDL_KEYDOWN. Returns true if consumed.
bool sftp_overlay_keydown(SDL_Keycode sym);

// Mouse wheel — call from SDL_MOUSEWHEEL when overlay is visible.
// x/y = cursor position, delta = ev.wheel.y. Returns true if consumed.
bool sftp_overlay_mousewheel(int x, int y, int delta, int win_w);

// Mouse button down — click to select entry, double-click to enter directory.
bool sftp_overlay_mousedown(int x, int y, int button, int win_w, int win_h);

std::string sftp_local_download_dir();
std::string sftp_local_home_dir();

bool sftp_init();
void sftp_shutdown();

// Call before sftp_shutdown if a transfer may be in progress (e.g. on quit).
void sftp_transfer_join();

// Returns current transfer progress 0..1 (thread-safe).
float sftp_progress();

#endif // USESSH
