#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// EYE OF FELIX  (F5)
//
// Local mode  : browses the local filesystem using dirent / FindFirstFile.
// Remote mode : browses remote filesystem via libssh2 SFTP (USESSH only).
//               Remote files are downloaded to a temp buffer before use.
//
// Image formats : JPEG, PNG, BMP, GIF, TIFF, WEBP  (via stb_image)
// Audio formats : MP3, OGG, FLAC, OPUS, WAV         (via SDL_mixer)
// CD+G karaoke  : .cdg paired with any supported audio file
// ZIP browsing  : peek inside zip files for images/audio/cdg
// ============================================================================

#define IV_SUPPORTED_EXTS  { ".jpg",".jpeg",".png",".bmp",".gif",".webp",".tiff",".tif" }
#define IV_AUDIO_EXTS      { ".mp3",".ogg",".flac",".opus",".wav" }

struct IVEntry {
    char     name[512]      = {};  // display name
    bool     is_dir         = false;
    bool     is_zip         = false;   // .zip file (browsable)
    bool     is_zip_entry   = false;   // entry inside a zip
    bool     is_audio       = false;   // audio file
    bool     is_cdg         = false;   // CD+G karaoke graphics file
    bool     has_cdg_pair   = false;   // audio file that has a matching .cdg
    char     zip_path[4096] = {};
    char     zip_entry[512] = {};
    uint64_t size           = 0;
};

// CD+G playback state (lives inside ImageViewer)
struct IVCdg {
    bool     active         = false;
    // Raw CDG packet data
    std::vector<uint8_t> data;
    int      packet_count   = 0;
    int      current_packet = 0;
    // Decoded screen buffer (palette indices)
    uint8_t  screen[216][300] = {};
    uint32_t palette[16]      = {};
    uint8_t  border_color     = 0;
    // GL texture updated each frame
    unsigned int tex          = 0;  // RGBA texture 300x216
};

struct ImageViewer {
    bool visible     = false;
    bool remote      = false;

    char path[4096]  = {};
    bool in_zip      = false;
    char zip_file[4096] = {};

    std::vector<IVEntry> entries;
    int  selected    = 0;
    int  scroll_top  = 0;

    // Current image (GL texture)
    unsigned int tex  = 0;
    int    tex_w      = 0;
    int    tex_h      = 0;
    char   img_label[512] = {};
    char   error[256]     = {};

    // Audio playback
    bool          audio_playing  = false;
    bool          audio_paused   = false;
    Mix_Music    *music          = nullptr;
    char          audio_label[512] = {};
    double        audio_start_ticks = 0.0; // SDL_GetTicks() when play started
    double        audio_position    = 0.0; // seconds (approximated)

    // CD+G
    IVCdg  cdg;
};

extern ImageViewer g_iv;

void iv_open(bool remote, int win_w, int win_h);
void iv_close();
void iv_tick(double dt);   // call every frame — advances CDG, updates audio pos
void iv_render(int win_w, int win_h);
bool iv_keydown(SDL_Keycode sym);
