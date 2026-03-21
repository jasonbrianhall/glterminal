#pragma once
#include <GL/glew.h>
#include <GL/gl.h>
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// IMAGE VIEWER OVERLAY  (F5)
//
// Local mode  : browses the local filesystem using dirent / FindFirstFile.
// Remote mode : browses remote filesystem via libssh2 SFTP (USESSH only).
//               Remote images are downloaded to a temp buffer before display.
//
// Supported formats: JPEG, PNG, BMP, GIF (static), TIFF, WEBP
// Decoding uses stb_image (header-only, no extra dep) embedded in image_viewer.cpp.
// ============================================================================

#define IV_SUPPORTED_EXTS { ".jpg",".jpeg",".png",".bmp",".gif",".webp",".tiff",".tif" }

struct IVEntry {
    char     name[512] = {};
    bool     is_dir    = false;
    uint64_t size      = 0;
};

struct ImageViewer {
    bool visible     = false;
    bool remote      = false;   // true = browsing via SFTP

    char path[4096]  = {};      // current directory

    std::vector<IVEntry> entries;
    int  selected    = 0;
    int  scroll_top  = 0;

    // Current image (GL texture)
    GLuint tex       = 0;
    int    tex_w     = 0;
    int    tex_h     = 0;
    char   img_label[512] = {};  // filename shown in status bar
    bool   loading   = false;
    char   error[256]= {};
};

extern ImageViewer g_iv;

// Open the viewer.  remote=true requires USESSH + active SSH session.
// Starts in the home directory (local) or remote $HOME (SSH).
void iv_open(bool remote, int win_w, int win_h);
void iv_close();

void iv_render(int win_w, int win_h);

// Returns true if the key was consumed.
bool iv_keydown(SDL_Keycode sym);
