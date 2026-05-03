#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include "cdg.h"
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// EYE OF FELIX  (F5)
//
// Image formats : JPEG, PNG, BMP, GIF, TIFF, WEBP  (via stb_image)
// Audio formats : MP3, OGG, FLAC, OPUS, WAV, AIFF, VOC          (via SDL_mixer)
//                 MOD, XM, IT, S3M, 669, MED, MTM  (tracker, via SDL_mixer/libxmp)
//                 MID/MIDI                           (via SDL_mixer + timidity/fluidsynth)
// CD+G karaoke  : .cdg paired with any supported audio file
// ZIP browsing  : peek inside zip files for images/audio/cdg
// ============================================================================

#define IV_SUPPORTED_EXTS  { ".jpg",".jpeg",".png",".bmp",".gif",".webp",".tiff",".tif" }
#define IV_AUDIO_EXTS      { ".mp3",".ogg",".flac",".opus",".wav",".mid",".midi", \
                             ".aiff",".aif",".voc", \
                             ".mod",".xm",".it",".s3m",".669",".med",".mtm" }

struct IVEntry {
    char     name[512]      = {};
    bool     is_dir         = false;
    bool     is_zip         = false;
    bool     is_zip_entry   = false;
    bool     is_audio       = false;
    bool     is_cdg         = false;
    bool     has_cdg_pair   = false;
    char     zip_path[4096] = {};
    char     zip_entry[512] = {};
    uint64_t size           = 0;
};

struct ImageViewer {
    bool visible  = false;
    bool remote   = false;

    char path[4096]     = {};
    bool in_zip         = false;
    char zip_file[4096] = {};

    std::vector<IVEntry> entries;
    int  selected    = 0;
    int  scroll_top  = 0;

    // Current image (GL texture)
    unsigned int tex  = 0;
    SDL_Texture *sdl_tex = nullptr;   // SDL renderer path (replaces tex)
    int    tex_w      = 0;
    int    tex_h      = 0;
    char   img_label[512] = {};
    char   error[256]     = {};

    // Audio visualizer
    int    vis_mode   = 0;      // 0=symmetry cascade, 1=radial, 2=oscilloscope, 3=starfield

    // Zoom / pan / rotation (image view)
    float  zoom       = 1.0f;   // 1.0 = fit-to-area
    float  pan_x      = 0.0f;  // offset in pixels from centre
    float  pan_y      = 0.0f;
    int    img_rot    = 0;      // 0 / 90 / 180 / 270 degrees
    bool   drag_active = false;
    int    drag_start_x = 0;
    int    drag_start_y = 0;
    float  drag_pan_x0  = 0.0f;
    float  drag_pan_y0  = 0.0f;

    // Audio playback
    bool       audio_playing      = false;
    bool       audio_paused       = false;
    Mix_Music *music              = nullptr;
    char       audio_label[512]   = {};
    double     audio_start_ticks  = 0.0;
    double     audio_position     = 0.0;

    // CD+G — uses your CDGDisplay from cdg.h/cdg.cpp
    CDGDisplay  *cdg_display      = nullptr;  // non-null when CDG is active
    unsigned int cdg_tex          = 0;        // GL texture 300x216, updated each frame
    SDL_Texture *sdl_cdg_tex      = nullptr;  // SDL renderer path (replaces cdg_tex)
};

extern ImageViewer g_iv;

void iv_open(bool remote, int win_w, int win_h);
void iv_close();
void iv_tick(double dt);
void iv_render(int win_w, int win_h);
bool iv_keydown(SDL_Keycode sym);
bool iv_mousewheel(int x, int y, int delta_y, int win_w, int win_h);
bool iv_mousedown(int x, int y, int button, int win_w, int win_h);
bool iv_mousemotion(int x, int y, int win_w, int win_h);
bool iv_mouseup(int x, int y, int button);
