// image_viewer.cpp — F5 image viewer overlay for Felix Terminal
// Supports local filesystem browsing.
// Image decoding via stb_image (no extra library dependency).

// GL must be included first — before stb_image and everything else
#include <GL/glew.h>
#include <GL/gl.h>

// image_viewer.h must come before stb/miniz so its types are visible everywhere
#include "image_viewer.h"

// ---- stb_image (header-only, embedded) ------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // we feed raw bytes, not filenames
// Note: do NOT use STBI_ONLY_* — the full decoder handles progressive JPEGs
// stb_image.h must be in the include path (https://github.com/nothings/stb)
#include "stb_image.h"

// ---- libwebp (auto-detected by Makefile, defines HAVE_WEBP when present) --
#ifdef HAVE_WEBP
#  include <webp/decode.h>
#endif

// ---- miniz (zip support) --------------------------------------------------
// Uses the split-header miniz distribution.
#include "miniz.h"
#include "miniz_zip.h"
// ---------------------------------------------------------------------------

#include "gl_renderer.h"
#include "sdl_renderer.h"
#include "ft_font.h"
#include "term_color.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctype.h>

#ifndef _WIN32
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <fcntl.h>
#else
#  include <windows.h>
#  include <shlobj.h>
#  include <io.h>
#  include <fcntl.h>
#endif

// ============================================================================
// CROSS-PLATFORM TEMP FILE
// ============================================================================

// Write data to a new temp file with the given extension. Returns the path on
// success or "" on failure. Caller must delete the file when done.
static std::string iv_write_tempfile(const unsigned char *data, size_t len,
                                      const char *ext) {
    char path[512];
#ifndef _WIN32
    snprintf(path, sizeof(path), "/tmp/iv_tmp_XXXXXX%s", ext ? ext : "");
    int fd = mkstemps(path, ext ? (int)strlen(ext) : 0);
    if (fd < 0) return "";
    ssize_t written = write(fd, data, len);
    close(fd);
    if (written != (ssize_t)len) { unlink(path); return ""; }
#else
    char tmp_dir[MAX_PATH];
    if (!GetTempPathA(sizeof(tmp_dir), tmp_dir)) return "";
    char base[MAX_PATH];
    if (!GetTempFileNameA(tmp_dir, "iv_", 0, base)) return "";
    // GetTempFileName creates a .tmp file — rename to include the right extension
    snprintf(path, sizeof(path), "%s%s", base, ext ? ext : "");
    // Write to the path directly (overwrite the placeholder)
    FILE *f = fopen(path, "wb");
    if (!f) { DeleteFileA(base); return ""; }
    bool ok = (fwrite(data, 1, len, f) == len);
    fclose(f);
    DeleteFileA(base);  // remove the placeholder .tmp
    if (!ok) { DeleteFileA(path); return ""; }
#endif
    return std::string(path);
}

static void iv_delete_tempfile(const char *path) {
    if (!path || !path[0]) return;
#ifndef _WIN32
    unlink(path);
#else
    DeleteFileA(path);
#endif
}

ImageViewer g_iv;
extern int  g_font_size;

static const int IV_PAD = 8;

// ============================================================================
// HELPERS
// ============================================================================

static bool is_image_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    const char *exts[] = IV_SUPPORTED_EXTS;
    for (auto *e : exts) if (strcmp(ext, e) == 0) return true;
    return false;
}

static bool is_zip_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    return strcmp(ext, ".zip") == 0;
}

static bool is_audio_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    const char *exts[] = IV_AUDIO_EXTS;
    for (auto *e : exts) if (strcmp(ext, e) == 0) return true;
    return false;
}

static bool is_cdg_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    return strcmp(ext, ".cdg") == 0;
}

// Given an audio filename, check if a matching .cdg exists in the same directory
static bool has_paired_cdg(const char *dir, const char *audio_name) {
    // Strip extension, append .cdg
    char base[512];
    strncpy(base, audio_name, sizeof(base)-1);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char cdg_path[4608];
#ifndef _WIN32
    snprintf(cdg_path, sizeof(cdg_path), "%s/%s.cdg", dir, base);
#else
    snprintf(cdg_path, sizeof(cdg_path), "%s\\%s.cdg", dir, base);
#endif
    // Case-insensitive check: try both .cdg and .CDG
    FILE *f = fopen(cdg_path, "rb");
    if (f) { fclose(f); return true; }
    // Try uppercase
    char *ext = strrchr(cdg_path, '.');
    if (ext) { ext[1]='C'; ext[2]='D'; ext[3]='G'; }
    f = fopen(cdg_path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// List images inside a local zip file into out.
// Each entry has is_zip_entry=true, zip_path set to zip_filepath, zip_entry set to the member name.
static void iv_list_zip(const char *zip_filepath, std::vector<IVEntry> &out) {
    out.clear();
    IVEntry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
    out.push_back(up);

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_file(&zip, zip_filepath, 0)) return;

    mz_uint n = mz_zip_reader_get_num_files(&zip);

    // First pass: collect all filenames so we can detect CDG pairs
    std::vector<std::string> all_names;
    for (mz_uint i = 0; i < n; i++) {
        char fname[512] = {};
        mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
        all_names.push_back(fname);
    }

    // Helper: check if a matching .cdg exists inside this zip for a given audio entry
    auto zip_has_cdg_pair = [&](const char *audio_fname) -> bool {
        char base[512]; strncpy(base, audio_fname, sizeof(base)-1);
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        for (auto &nm : all_names) {
            const char *ndot = strrchr(nm.c_str(), '.');
            if (!ndot) continue;
            // Compare base names case-insensitively
            size_t blen = strlen(base);
            if (nm.size() < blen + 4) continue;
            if (strncasecmp(nm.c_str(), base, blen) == 0) {
                char ext[8] = {};
                for (int j = 0; j < 7 && ndot[j]; j++) ext[j] = tolower((unsigned char)ndot[j]);
                if (strcmp(ext, ".cdg") == 0) return true;
            }
        }
        return false;
    };

    for (mz_uint i = 0; i < n; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char fname[512] = {};
        mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
        if (strncmp(fname, "__MACOSX", 8) == 0) continue;

        bool img   = is_image_ext(fname);
        bool audio = is_audio_ext(fname);
        bool cdg   = is_cdg_ext(fname);
        if (!img && !audio && !cdg) continue;

        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&zip, i, &st);

        IVEntry e{};
        const char *slash = strrchr(fname, '/');
        strncpy(e.name,      slash ? slash+1 : fname, sizeof(e.name)-1);
        strncpy(e.zip_path,  zip_filepath,             sizeof(e.zip_path)-1);
        strncpy(e.zip_entry, fname,                    sizeof(e.zip_entry)-1);
        e.is_zip_entry = true;
        e.size         = st.m_uncomp_size;
        if (audio) { e.is_audio = true; e.has_cdg_pair = zip_has_cdg_pair(fname); }
        if (cdg)   { e.is_cdg   = true; }
        out.push_back(e);
    }
    mz_zip_reader_end(&zip);

    std::stable_sort(out.begin()+1, out.end(), [](const IVEntry &a, const IVEntry &b){
        return strcmp(a.zip_entry, b.zip_entry) < 0;
    });
}

// Extract one image from a local zip into a heap buffer. Caller must free().
static unsigned char *iv_extract_zip_entry(const char *zip_filepath,
                                            const char *entry_name,
                                            size_t &out_size) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    out_size = 0;
    if (!mz_zip_reader_init_file(&zip, zip_filepath, 0)) return nullptr;
    void *buf = mz_zip_reader_extract_file_to_heap(&zip, entry_name, &out_size, 0);
    mz_zip_reader_end(&zip);
    return (unsigned char *)buf;
}

static std::string iv_home() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, buf);
    return buf[0] ? buf : "C:\\Users";
#else
    const char *h = getenv("HOME");
    return h ? h : "/home";
#endif
}

static void iv_draw_text(const char *t, float x, float y, float r, float g, float b, float a) {
    draw_text(t, x, y, g_font_size, g_font_size, r, g, b, a, 0);
}

static int iv_row_h() { return (int)(g_font_size * 1.8f); }

// Returns true if the zip at zip_path contains a CDG+audio pair
static bool zip_contains_cdg_pair(const char *zip_path) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) return false;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    std::vector<std::string> names;
    for (mz_uint i = 0; i < n; i++) {
        char fname[512] = {};
        mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
        names.push_back(fname);
    }
    mz_zip_reader_end(&zip);
    for (auto &nm : names) {
        if (!is_audio_ext(nm.c_str())) continue;
        char base[512]; strncpy(base, nm.c_str(), sizeof(base)-1);
        char *dot = strrchr(base, '.'); if (!dot) continue; *dot = '\0';
        for (auto &nm2 : names) {
            const char *d2 = strrchr(nm2.c_str(), '.');
            if (!d2) continue;
            char e2[8] = {};
            for (int i = 0; i < 7 && d2[i]; i++) e2[i] = tolower((unsigned char)d2[i]);
            if (strcmp(e2, ".cdg") != 0) continue;
            // Check base names match
            size_t blen = strlen(base);
            if (nm2.size() >= blen + 4 && strncasecmp(nm2.c_str(), base, blen) == 0)
                return true;
        }
    }
    return false;
}

// ============================================================================
// DIRECTORY LISTING
// ============================================================================

static void iv_list_local(const char *path, std::vector<IVEntry> &out) {
    out.clear();
#ifndef _WIN32
    if (strcmp(path, "/") != 0) {
        IVEntry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
        out.push_back(up);
    }
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        IVEntry e{};
        strncpy(e.name, de->d_name, sizeof(e.name)-1);
        char full[4096]; snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st{};
        if (stat(full, &st) == 0) {
            e.is_dir = S_ISDIR(st.st_mode);
            e.size   = (uint64_t)st.st_size;
        }
        bool img   = is_image_ext(e.name);
        bool zip   = is_zip_ext(e.name);
        bool audio = is_audio_ext(e.name);
        bool cdg   = is_cdg_ext(e.name);
        if (e.is_dir || img || zip || audio || cdg) {
            if (zip)   { e.is_zip = true; e.has_cdg_pair = zip_contains_cdg_pair(full); }
            if (audio) { e.is_audio = true; e.has_cdg_pair = has_paired_cdg(path, e.name); }
            if (cdg)   e.is_cdg = true;
            out.push_back(e);
        }
    }
    closedir(d);
#else
    if (strcmp(path, "/") != 0 && strcmp(path, "\\") != 0) {
        IVEntry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
        out.push_back(up);
    }
    char pattern[4096]; snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        IVEntry e{};
        strncpy(e.name, fd.cFileName, sizeof(e.name)-1);
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size   = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (e.is_dir || is_image_ext(e.name) || is_zip_ext(e.name) ||
            is_audio_ext(e.name) || is_cdg_ext(e.name)) {
            if (is_zip_ext(e.name)) {
                e.is_zip = true;
                char full[4096]; snprintf(full, sizeof(full), "%s\\%s", path, e.name);
                e.has_cdg_pair = zip_contains_cdg_pair(full);
            }
            if (is_audio_ext(e.name)) { e.is_audio = true;
                e.has_cdg_pair = has_paired_cdg(path, e.name); }
            if (is_cdg_ext(e.name))   e.is_cdg = true;
            out.push_back(e);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    std::stable_sort(out.begin(), out.end(), [](const IVEntry &a, const IVEntry &b){
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return strcmp(a.name, b.name) < 0;
    });
}



// ============================================================================
// TEXTURE LOADING
// ============================================================================

static void iv_free_tex() {
    if (g_use_sdl_renderer) {
        if (g_iv.sdl_tex) { SDL_DestroyTexture(g_iv.sdl_tex); g_iv.sdl_tex = nullptr; }
    } else {
        if (g_iv.tex) { glDeleteTextures(1, &g_iv.tex); g_iv.tex = 0; }
    }
    g_iv.tex_w = g_iv.tex_h = 0;
}

// ============================================================================
// AUDIO STOP / CDG FREE
// ============================================================================

static void iv_stop_audio() {
    if (g_iv.music) {
        Mix_HaltMusic();
        Mix_FreeMusic(g_iv.music);
        g_iv.music = nullptr;
    }
    g_iv.audio_playing  = false;
    g_iv.audio_paused   = false;
    g_iv.audio_position = 0.0;
    g_iv.audio_label[0] = '\0';
}

static void iv_cdg_free() {
    if (g_use_sdl_renderer) {
        if (g_iv.sdl_cdg_tex) { SDL_DestroyTexture(g_iv.sdl_cdg_tex); g_iv.sdl_cdg_tex = nullptr; }
    } else {
        if (g_iv.cdg_tex) { glDeleteTextures(1, (GLuint*)&g_iv.cdg_tex); g_iv.cdg_tex = 0; }
    }
    if (g_iv.cdg_display) { cdg_display_free(g_iv.cdg_display); g_iv.cdg_display = nullptr; }
}

static void iv_cdg_upload_texture();  // forward declaration

static bool iv_cdg_load(const char *cdg_path) {
    iv_cdg_free();
    g_iv.cdg_display = cdg_display_new();
    if (!g_iv.cdg_display) return false;
    if (!cdg_load_file(g_iv.cdg_display, cdg_path)) {
        cdg_display_free(g_iv.cdg_display);
        g_iv.cdg_display = nullptr;
        return false;
    }
    // Upload an initial (blank) texture immediately so the display area
    // shows the CDG frame from frame 1 rather than falling through to
    // the audio-only UI while waiting for iv_tick to run.
    iv_cdg_upload_texture();
    return true;
}

static void iv_cdg_upload_texture() {
    CDGDisplay *cdg = g_iv.cdg_display;
    if (!cdg) return;
    static uint8_t rgba[CDG_HEIGHT * CDG_WIDTH * 4];
    for (int y = 0; y < CDG_HEIGHT; y++) {
        for (int x = 0; x < CDG_WIDTH; x++) {
            uint8_t idx = cdg->screen[y][x] & 0x0F;
            uint32_t col = cdg->palette[idx];
            int off = (y * CDG_WIDTH + x) * 4;
            rgba[off+0] = (col >> 16) & 0xFF;
            rgba[off+1] = (col >>  8) & 0xFF;
            rgba[off+2] =  col        & 0xFF;
            rgba[off+3] = 0xFF;
        }
    }
    if (g_use_sdl_renderer) {
        if (g_iv.sdl_cdg_tex) SDL_DestroyTexture(g_iv.sdl_cdg_tex);
        g_iv.sdl_cdg_tex = SDL_CreateTexture(g_sdl_renderer,
            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
            CDG_WIDTH, CDG_HEIGHT);
        if (g_iv.sdl_cdg_tex)
            SDL_UpdateTexture(g_iv.sdl_cdg_tex, nullptr, rgba, CDG_WIDTH * 4);
    } else {
        if (!g_iv.cdg_tex) {
            glGenTextures(1, (GLuint*)&g_iv.cdg_tex);
            glBindTexture(GL_TEXTURE_2D, g_iv.cdg_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CDG_WIDTH, CDG_HEIGHT, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        } else {
            glBindTexture(GL_TEXTURE_2D, g_iv.cdg_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CDG_WIDTH, CDG_HEIGHT,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

// ============================================================================
// AUDIO PLAY
// ============================================================================

static bool s_mixer_ready = false;

static void iv_ensure_mixer() {
    if (s_mixer_ready) return;
    Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048);
    Mix_AllocateChannels(8);
    s_mixer_ready = true;
}

static void iv_play_audio(const char *audio_path, const char *label, bool load_cdg) {
    iv_stop_audio();
    iv_cdg_free();
    iv_free_tex();
    iv_ensure_mixer();

    g_iv.music = Mix_LoadMUS(audio_path);
    if (!g_iv.music) {
        snprintf(g_iv.error, sizeof(g_iv.error), "Cannot load: %s", Mix_GetError());
        return;
    }
    Mix_PlayMusic(g_iv.music, 1);
    g_iv.audio_playing     = true;
    g_iv.audio_paused      = false;
    g_iv.audio_position    = 0.0;
    g_iv.audio_start_ticks = (double)SDL_GetTicks();
    strncpy(g_iv.audio_label, label, sizeof(g_iv.audio_label)-1);
    g_iv.error[0] = '\0';

    if (load_cdg) {
        char cdg_path[4096];
        strncpy(cdg_path, audio_path, sizeof(cdg_path)-1);
        char *dot = strrchr(cdg_path, '.');
        if (dot) {
            strcpy(dot, ".cdg");
            if (!iv_cdg_load(cdg_path)) { strcpy(dot, ".CDG"); iv_cdg_load(cdg_path); }
        }
    }
}

static void iv_upload_texture(const unsigned char *rgba, int w, int h) {
    iv_free_tex();
    if (g_use_sdl_renderer) {
        g_iv.sdl_tex = SDL_CreateTexture(g_sdl_renderer,
            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (g_iv.sdl_tex) {
            SDL_SetTextureBlendMode(g_iv.sdl_tex, SDL_BLENDMODE_BLEND);
            SDL_UpdateTexture(g_iv.sdl_tex, nullptr, rgba, w * 4);
        }
    } else {
        glGenTextures(1, &g_iv.tex);
        glBindTexture(GL_TEXTURE_2D, g_iv.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    g_iv.tex_w = w;
    g_iv.tex_h = h;
}

static void iv_load_image_from_mem(const unsigned char *data, size_t len, const char *label) {
    int w = 0, h = 0;
    unsigned char *px = nullptr;
    bool webp_alloc   = false;

#ifdef HAVE_WEBP
    // Detect WebP by magic: bytes 0-3 == "RIFF", bytes 8-11 == "WEBP"
    if (len >= 12 &&
        memcmp(data,     "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0)
    {
        px = WebPDecodeRGBA(data, len, &w, &h);
        if (!px) {
            snprintf(g_iv.error, sizeof(g_iv.error), "WebP decode failed");
            iv_free_tex();
            return;
        }
        webp_alloc = true;
    }
#endif

    if (!px) {
        int ch = 0;
        px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
        if (!px) {
            snprintf(g_iv.error, sizeof(g_iv.error), "Decode failed: %s", stbi_failure_reason());
            iv_free_tex();
            return;
        }
    }

    g_iv.error[0] = '\0';
    iv_upload_texture(px, w, h);

    if (webp_alloc)
        WebPFree(px);
    else
        stbi_image_free(px);

    strncpy(g_iv.img_label, label, sizeof(g_iv.img_label)-1);
    g_iv.zoom    = 1.0f;
    g_iv.pan_x   = 0.0f;
    g_iv.pan_y   = 0.0f;
    g_iv.img_rot = 0;
}

static void iv_load_local(const char *filepath, const char *label) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        snprintf(g_iv.error, sizeof(g_iv.error), "Cannot open: %s", filepath);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 64 * 1024 * 1024) {
        snprintf(g_iv.error, sizeof(g_iv.error), "File too large or empty");
        fclose(f);
        return;
    }
    unsigned char *buf = (unsigned char *)malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    iv_load_image_from_mem(buf, (size_t)sz, label);
    free(buf);
}

// ============================================================================
// OPEN / CLOSE
// ============================================================================

void iv_open(bool remote, int /*win_w*/, int /*win_h*/) {
    g_iv = ImageViewer{};
    g_iv.visible = true;
    g_iv.remote  = false;

    strncpy(g_iv.path, iv_home().c_str(), sizeof(g_iv.path)-1);
    iv_list_local(g_iv.path, g_iv.entries);
}

void iv_close() {
    iv_stop_audio();
    iv_cdg_free();
    iv_free_tex();
    g_iv.visible = false;
}

// ============================================================================
// NAVIGATE
// ============================================================================

// ============================================================================
// TICK  (call every frame)
// ============================================================================

void iv_tick(double /*dt*/) {
    if (!g_iv.visible) return;

    // Update audio position from wall clock whenever music is loaded
    if (g_iv.music && g_iv.audio_playing && !g_iv.audio_paused) {
        g_iv.audio_position = ((double)SDL_GetTicks() - g_iv.audio_start_ticks) / 1000.0;
        if (!Mix_PlayingMusic()) {
            g_iv.audio_playing  = false;
            g_iv.audio_position = 0.0;
        }
    }

    // Advance CDG to current position using your cdg_update()
    if (g_iv.cdg_display) {
        cdg_update(g_iv.cdg_display, g_iv.audio_position);
        iv_cdg_upload_texture();
    }
}

static void iv_refresh() {
    g_iv.selected   = 0;
    g_iv.scroll_top = 0;
    if (g_iv.in_zip)
        iv_list_zip(g_iv.zip_file, g_iv.entries);
    else
        iv_list_local(g_iv.path, g_iv.entries);
}

static void iv_enter_selected() {
    if (g_iv.entries.empty()) return;
    const IVEntry &e = g_iv.entries[g_iv.selected];

    // ── Inside a zip: handle image, audio, or CDG entry ──────────────────
    if (e.is_zip_entry) {
        g_iv.error[0] = '\0';

        if (e.is_audio) {
            // Extract audio to a temp file (SDL_mixer needs a real file path)
            size_t sz = 0;
            unsigned char *buf = iv_extract_zip_entry(e.zip_path, e.zip_entry, sz);
            if (!buf) {
                snprintf(g_iv.error, sizeof(g_iv.error), "Failed to extract: %s", e.name);
                return;
            }
            const char *dot = strrchr(e.name, '.');
            std::string tmp_path = iv_write_tempfile(buf, sz, dot ? dot : ".tmp");
            free(buf);
            if (tmp_path.empty()) {
                snprintf(g_iv.error, sizeof(g_iv.error), "Cannot write temp file");
                return;
            }

            // If CDG pair exists in the zip, extract it too
            bool load_cdg = e.has_cdg_pair;
            if (load_cdg) {
                char cdg_entry[512]; strncpy(cdg_entry, e.zip_entry, sizeof(cdg_entry)-1);
                char *edot = strrchr(cdg_entry, '.'); if (edot) strcpy(edot, ".cdg");
                size_t csz = 0;
                unsigned char *cbuf = iv_extract_zip_entry(e.zip_path, cdg_entry, csz);
                if (!cbuf) {
                    if (edot) strcpy(edot, ".CDG");
                    cbuf = iv_extract_zip_entry(e.zip_path, cdg_entry, csz);
                }
                if (cbuf) {
                    std::string tmp_cdg = iv_write_tempfile(cbuf, csz, ".cdg");
                    free(cbuf);
                    iv_stop_audio(); iv_cdg_free(); iv_ensure_mixer();
                    g_iv.music = Mix_LoadMUS(tmp_path.c_str());
                    if (g_iv.music) {
                        Mix_PlayMusic(g_iv.music, 1);
                        g_iv.audio_playing     = true;
                        g_iv.audio_paused      = false;
                        g_iv.audio_position    = 0.0;
                        g_iv.audio_start_ticks = (double)SDL_GetTicks();
                        strncpy(g_iv.audio_label, e.name, sizeof(g_iv.audio_label)-1);
                        if (!tmp_cdg.empty()) iv_cdg_load(tmp_cdg.c_str());
                    }
                    if (!tmp_cdg.empty()) iv_delete_tempfile(tmp_cdg.c_str());
                    iv_delete_tempfile(tmp_path.c_str());
                    return;
                }
            }

            iv_play_audio(tmp_path.c_str(), e.name, false);
            iv_delete_tempfile(tmp_path.c_str());
            return;
        }

        // Image entry
        iv_free_tex();
        iv_stop_audio();
        iv_cdg_free();
        size_t sz = 0;
        unsigned char *buf = iv_extract_zip_entry(e.zip_path, e.zip_entry, sz);
        if (buf) {
            iv_load_image_from_mem(buf, sz, e.name);
            free(buf);
        } else {
            snprintf(g_iv.error, sizeof(g_iv.error), "Failed to extract: %s", e.name);
        }
        return;
    }

    if (e.is_dir) {
        // ".." from inside a zip goes back to the filesystem directory
        if (strcmp(e.name, "..") == 0 && g_iv.in_zip) {
            g_iv.in_zip = false;
            g_iv.zip_file[0] = '\0';
            iv_refresh();
            return;
        }
        // Navigate into directory
        char newpath[4096];
        if (strcmp(e.name, "..") == 0) {
            char tmp[4096];
            strncpy(tmp, g_iv.path, sizeof(tmp)-1);
            if (g_iv.remote || tmp[0] == '/') {
                char *slash = strrchr(tmp, '/');
                if (slash && slash != tmp) { *slash = '\0'; }
                else { tmp[0]='/'; tmp[1]='\0'; }
            } else {
                char *slash = strrchr(tmp, '\\');
                if (!slash) slash = strrchr(tmp, '/');
                if (slash && slash != tmp) { *slash = '\0'; }
                else { tmp[0]='\\'; tmp[1]='\0'; }
            }
            strncpy(g_iv.path, tmp, sizeof(g_iv.path)-1);
        } else {
            // Avoid double-slash when path is already "/"
            size_t plen = strlen(g_iv.path);
            bool ends_sep = plen > 0 &&
                (g_iv.path[plen-1] == '/' || g_iv.path[plen-1] == '\\');
            const char *sep = ends_sep ? "" :
                              (g_iv.path[0] == '/') ? "/" : "\\";
            snprintf(newpath, sizeof(newpath), "%s%s%s", g_iv.path, sep, e.name);
            strncpy(g_iv.path, newpath, sizeof(g_iv.path)-1);
        }
        iv_refresh();

    } else if (e.is_zip) {
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s%s%s",
                 g_iv.path,
                 (g_iv.path[0] == '/') ? "/" : "\\",
                 e.name);

        std::string tmp_zip_path;
        const char *zip_to_open = fullpath;

        // Peek inside the zip — if it contains a CDG+audio pair, auto-play it
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        bool auto_played = false;
        if (mz_zip_reader_init_file(&zip, zip_to_open, 0)) {
            mz_uint n = mz_zip_reader_get_num_files(&zip);

            std::vector<std::string> all_names;
            for (mz_uint i = 0; i < n; i++) {
                char fname[512] = {};
                mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
                all_names.push_back(fname);
            }

            for (mz_uint i = 0; i < n && !auto_played; i++) {
                if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
                char fname[512] = {};
                mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
                if (!is_audio_ext(fname)) continue;

                char cdg_entry[512]; strncpy(cdg_entry, fname, sizeof(cdg_entry)-1);
                char *dot = strrchr(cdg_entry, '.'); if (!dot) continue;
                strcpy(dot, ".cdg");
                bool found_cdg = false;
                for (auto &nm : all_names)
                    if (strcasecmp(nm.c_str(), cdg_entry) == 0) { found_cdg = true; break; }
                if (!found_cdg) { strcpy(dot, ".CDG");
                    for (auto &nm : all_names)
                        if (strcasecmp(nm.c_str(), cdg_entry) == 0) { found_cdg = true; break; }
                }
                if (!found_cdg) continue;

                mz_zip_reader_end(&zip);

                auto extract_to_tmp = [&](const char *entry, const char *ext) -> std::string {
                    size_t sz = 0;
                    unsigned char *buf = iv_extract_zip_entry(zip_to_open, entry, sz);
                    if (!buf) return "";
                    std::string path = iv_write_tempfile(buf, sz, ext);
                    free(buf);
                    return path;
                };

                std::string cdg_actual;
                for (auto &nm : all_names)
                    if (strcasecmp(nm.c_str(), cdg_entry) == 0) { cdg_actual = nm; break; }

                const char *audio_ext = strrchr(fname, '.');
                std::string tmp_audio = extract_to_tmp(fname, audio_ext ? audio_ext : ".mp3");
                std::string tmp_cdg   = extract_to_tmp(cdg_actual.c_str(), ".cdg");

                if (!tmp_audio.empty()) {
                    const char *display_name = strrchr(fname, '/');
                    display_name = display_name ? display_name+1 : fname;

                    iv_stop_audio(); iv_cdg_free(); iv_free_tex();
                    iv_ensure_mixer();
                    g_iv.music = Mix_LoadMUS(tmp_audio.c_str());
                    if (g_iv.music) {
                        Mix_PlayMusic(g_iv.music, 1);
                        g_iv.audio_playing     = true;
                        g_iv.audio_paused      = false;
                        g_iv.audio_position    = 0.0;
                        g_iv.audio_start_ticks = (double)SDL_GetTicks();
                        strncpy(g_iv.audio_label, display_name, sizeof(g_iv.audio_label)-1);
                        g_iv.error[0] = '\0';
                        if (!tmp_cdg.empty()) iv_cdg_load(tmp_cdg.c_str());
                    }
                    iv_delete_tempfile(tmp_audio.c_str());
                    if (!tmp_cdg.empty()) iv_delete_tempfile(tmp_cdg.c_str());
                    auto_played = true;
                }
                break;
            }

            if (!auto_played) mz_zip_reader_end(&zip);
        }

        if (!tmp_zip_path.empty()) iv_delete_tempfile(tmp_zip_path.c_str());

        if (!auto_played) {
            strncpy(g_iv.zip_file, fullpath, sizeof(g_iv.zip_file)-1);
            g_iv.in_zip = true; g_iv.selected = 0; g_iv.scroll_top = 0;
            iv_list_zip(fullpath, g_iv.entries);
        }

    } else {
        // Load plain image or play audio
        g_iv.error[0] = '\0';
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s%s%s",
                 g_iv.path,
                 (g_iv.path[0] == '/') ? "/" : "\\",
                 e.name);

        if (e.is_audio) {
            iv_play_audio(fullpath, e.name, e.has_cdg_pair);
        } else {
            // Image file
            iv_free_tex();
            iv_stop_audio();
            iv_cdg_free();
            iv_load_local(fullpath, e.name);
        }
    }
}

// ============================================================================
// SIZE HELPER
// ============================================================================

static void fmt_size_iv(uint64_t sz, char *buf, int n) {
    if      (sz >= 1024*1024*1024) snprintf(buf, n, "%.1fG", sz/(1024.0*1024*1024));
    else if (sz >= 1024*1024)      snprintf(buf, n, "%.1fM", sz/(1024.0*1024));
    else if (sz >= 1024)           snprintf(buf, n, "%.1fK", sz/1024.0);
    else                           snprintf(buf, n, "%lluB", (unsigned long long)sz);
}

// ============================================================================
// RENDER
// ============================================================================

static void iv_draw_image_tex(SDL_Texture *sdl_tex, unsigned int gl_tex,
                               float x, float y, float w, float h) {
    if (g_use_sdl_renderer) {
        if (!sdl_tex) return;
        gl_flush_verts();
        SDL_Vertex verts[6];
        auto mkv = [](float vx, float vy, float u, float v) {
            SDL_Vertex sv{};
            sv.position.x = vx; sv.position.y = vy;
            sv.color = {255, 255, 255, 255};
            sv.tex_coord.x = u; sv.tex_coord.y = v;
            return sv;
        };
        verts[0] = mkv(x,   y,   0.f, 0.f);
        verts[1] = mkv(x+w, y,   1.f, 0.f);
        verts[2] = mkv(x+w, y+h, 1.f, 1.f);
        verts[3] = mkv(x,   y,   0.f, 0.f);
        verts[4] = mkv(x+w, y+h, 1.f, 1.f);
        verts[5] = mkv(x,   y+h, 0.f, 1.f);
        SDL_RenderGeometry(g_sdl_renderer, sdl_tex, verts, 6, nullptr, 0);
        return;
    }
    if (!gl_tex) return;

    gl_flush_verts();

    GLint prev_prog;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);

    static GLuint s_tex_prog = 0;
    static GLuint s_tex_vao  = 0;
    static GLuint s_tex_vbo  = 0;
    if (!s_tex_prog) {
        const char *vs =
            "#version 330 core\n"
            "layout(location=0) in vec4 vtx;\n"
            "out vec2 uv;\n"
            "void main(){ gl_Position=vec4(vtx.xy,0,1); uv=vtx.zw; }\n";
        const char *fs =
            "#version 330 core\n"
            "in vec2 uv;\n"
            "out vec4 frag;\n"
            "uniform sampler2D tex;\n"
            "void main(){ frag=texture(tex,uv); }\n";
        auto compile = [](GLenum t, const char *src) {
            GLuint s = glCreateShader(t);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            return s;
        };
        GLuint v = compile(GL_VERTEX_SHADER, vs);
        GLuint f = compile(GL_FRAGMENT_SHADER, fs);
        s_tex_prog = glCreateProgram();
        glAttachShader(s_tex_prog, v); glAttachShader(s_tex_prog, f);
        glLinkProgram(s_tex_prog);
        glDeleteShader(v); glDeleteShader(f);
        glGenVertexArrays(1, &s_tex_vao);
        glGenBuffers(1, &s_tex_vbo);
        glBindVertexArray(s_tex_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_tex_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4*sizeof(float), nullptr);
        glBindVertexArray(0);
    }

    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    float ww = (float)vp[2], wh = (float)vp[3];
    auto px2ndc_x = [&](float px) { return (px / ww) * 2.f - 1.f; };
    auto px2ndc_y = [&](float py) { return 1.f - (py / wh) * 2.f; };

    float x0 = px2ndc_x(x),   y0 = px2ndc_y(y);
    float x1 = px2ndc_x(x+w), y1 = px2ndc_y(y+h);
    float verts[24] = {
        x0, y0, 0.f, 0.f,  x1, y0, 1.f, 0.f,  x1, y1, 1.f, 1.f,
        x0, y0, 0.f, 0.f,  x1, y1, 1.f, 1.f,  x0, y1, 0.f, 1.f,
    };

    glUseProgram(s_tex_prog);
    glBindVertexArray(s_tex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_tex_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glUniform1i(glGetUniformLocation(s_tex_prog, "tex"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(prev_prog);
}

static void iv_draw_image(float x, float y, float w, float h) {
    iv_draw_image_tex(g_iv.sdl_tex, g_iv.tex, x, y, w, h);
}

// Draw a texture as a rotated quad. cx/cy = centre in pixels, w/h = display
// dimensions before rotation, angle_deg = clockwise degrees (0/90/180/270).
// UV assignment rotates the texture coords to match so the image actually turns.
static void iv_draw_image_rotated(float cx, float cy, float w, float h, int angle_deg) {
    // Build 4 corners in pixel space, rotated around centre
    float rad  = (float)(angle_deg * M_PI / 180.0);
    float cosA = cosf(rad), sinA = sinf(rad);
    float hw = w * 0.5f, hh = h * 0.5f;

    // Local corner offsets (before rotation): TL, TR, BR, BL
    float lx[4] = { -hw,  hw,  hw, -hw };
    float ly[4] = { -hh, -hh,  hh,  hh };

    float px[4], py[4];
    for (int i = 0; i < 4; i++) {
        px[i] = cx + lx[i] * cosA - ly[i] * sinA;
        py[i] = cy + lx[i] * sinA + ly[i] * cosA;
    }

    // UV coords for each corner (TL, TR, BR, BL) after clockwise rotation.
    // At 0°:   TL=(0,0) TR=(1,0) BR=(1,1) BL=(0,1)
    // At 90°CW: the top of the image goes to the right, so:
    //   TL=(0,1) TR=(0,0) BR=(1,0) BL=(1,1)
    // At 180°: TL=(1,1) TR=(0,1) BR=(0,0) BL=(1,0)
    // At 270°CW: TL=(1,0) TR=(1,1) BR=(0,1) BL=(0,0)
    static const float uv_table[4][4][2] = {
        { {0,0},{1,0},{1,1},{0,1} },   //   0°
        { {0,1},{0,0},{1,0},{1,1} },   //  90° CW
        { {1,1},{0,1},{0,0},{1,0} },   // 180°
        { {1,0},{1,1},{0,1},{0,0} },   // 270° CW
    };
    int rot_idx = ((angle_deg / 90) % 4 + 4) % 4;
    float u[4], v[4];
    for (int i = 0; i < 4; i++) {
        u[i] = uv_table[rot_idx][i][0];
        v[i] = uv_table[rot_idx][i][1];
    }

    if (g_use_sdl_renderer) {
        if (!g_iv.sdl_tex) return;
        gl_flush_verts();
        SDL_Vertex verts[6];
        auto mkv = [](float vx, float vy, float tu, float tv) {
            SDL_Vertex sv{};
            sv.position.x = vx; sv.position.y = vy;
            sv.color = {255, 255, 255, 255};
            sv.tex_coord.x = tu; sv.tex_coord.y = tv;
            return sv;
        };
        // Two triangles: TL-TR-BR and TL-BR-BL
        verts[0] = mkv(px[0], py[0], u[0], v[0]);
        verts[1] = mkv(px[1], py[1], u[1], v[1]);
        verts[2] = mkv(px[2], py[2], u[2], v[2]);
        verts[3] = mkv(px[0], py[0], u[0], v[0]);
        verts[4] = mkv(px[2], py[2], u[2], v[2]);
        verts[5] = mkv(px[3], py[3], u[3], v[3]);
        SDL_RenderGeometry(g_sdl_renderer, g_iv.sdl_tex, verts, 6, nullptr, 0);
        return;
    }

    if (!g_iv.tex) return;
    gl_flush_verts();

    // Reuse the existing shader from iv_draw_image_tex (it's in a static)
    // We need to call it directly with rotated NDC verts.
    // Since the shader is static-local to iv_draw_image_tex, duplicate the
    // minimal GL draw here using the same VAO trick with a custom vert array.
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    float ww = (float)vp[2], wh = (float)vp[3];
    auto nx = [&](float x2) { return (x2 / ww) * 2.f - 1.f; };
    auto ny = [&](float y2) { return 1.f - (y2 / wh) * 2.f; };

    // Build 6-vert array: TL-TR-BR, TL-BR-BL (xyuv per vert)
    float verts[24] = {
        nx(px[0]),ny(py[0]),u[0],v[0],
        nx(px[1]),ny(py[1]),u[1],v[1],
        nx(px[2]),ny(py[2]),u[2],v[2],
        nx(px[0]),ny(py[0]),u[0],v[0],
        nx(px[2]),ny(py[2]),u[2],v[2],
        nx(px[3]),ny(py[3]),u[3],v[3],
    };

    // Use a simple self-contained program for the rotated draw
    static GLuint s_rot_prog = 0;
    static GLuint s_rot_vao  = 0;
    static GLuint s_rot_vbo  = 0;
    if (!s_rot_prog) {
        const char *vs =
            "#version 330 core\n"
            "layout(location=0) in vec4 vtx;\n"
            "out vec2 uv;\n"
            "void main(){ gl_Position=vec4(vtx.xy,0,1); uv=vtx.zw; }\n";
        const char *fs =
            "#version 330 core\n"
            "in vec2 uv; out vec4 frag;\n"
            "uniform sampler2D tex;\n"
            "void main(){ frag=texture(tex,uv); }\n";
        auto cmp = [](GLenum t, const char *src) {
            GLuint s = glCreateShader(t);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            return s;
        };
        GLuint vs2 = cmp(GL_VERTEX_SHADER, vs);
        GLuint fs2 = cmp(GL_FRAGMENT_SHADER, fs);
        s_rot_prog = glCreateProgram();
        glAttachShader(s_rot_prog, vs2); glAttachShader(s_rot_prog, fs2);
        glLinkProgram(s_rot_prog);
        glDeleteShader(vs2); glDeleteShader(fs2);
        glGenVertexArrays(1, &s_rot_vao);
        glGenBuffers(1, &s_rot_vbo);
        glBindVertexArray(s_rot_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_rot_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4*sizeof(float), nullptr);
        glBindVertexArray(0);
    }

    GLint prev_prog; glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glUseProgram(s_rot_prog);
    glBindVertexArray(s_rot_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_rot_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_iv.tex);
    glUniform1i(glGetUniformLocation(s_rot_prog, "tex"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(prev_prog);
}

// Truncate a filename so it fits within max_chars, preserving the extension.
// Result is written into out (must be at least max_chars+1 bytes).
// Uses a UTF-8-safe approach by working in bytes (filenames are usually ASCII).
static void iv_truncate_name(const char *name, int max_chars, char *out, int out_sz) {
    int len = (int)strlen(name);
    if (len <= max_chars) {
        strncpy(out, name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    // Find extension (last dot)
    const char *dot = strrchr(name, '.');
    int ext_len = dot ? (int)strlen(dot) : 0;  // includes the dot
    // We need: stem... + ellipsis(1 char '…' = 3 bytes) + ext
    // Keep at least "A…ext" = 1 stem char + ellipsis + ext
    int keep_stem = max_chars - 1 - ext_len;  // chars for stem before ellipsis
    if (keep_stem < 1) keep_stem = 1;
    // Build truncated string
    int pos = 0;
    for (int i = 0; i < keep_stem && i < len && pos < out_sz - 1; i++)
        out[pos++] = name[i];
    // UTF-8 ellipsis: 0xE2 0x80 0xA6
    if (pos + 3 < out_sz) { out[pos++]='\xe2'; out[pos++]='\x80'; out[pos++]='\xa6'; }
    if (dot && ext_len > 0 && pos + ext_len < out_sz) {
        strncpy(out + pos, dot, out_sz - pos - 1);
        pos += ext_len;
    }
    out[pos] = '\0';
}

void iv_render(int win_w, int win_h) {
    if (!g_iv.visible) return;

    int rh = iv_row_h();

    // ── Layout ─────────────────────────────────────────────────────────────
    // Left: file browser panel (~30% width)
    // Right: image display area (~70% width)
    // Bottom: status bar (1 row)
    int status_h = rh + IV_PAD;
    int panel_w  = (int)(win_w * 0.28f);
    int panel_w_min = rh * 14;
    if (panel_w < panel_w_min) panel_w = panel_w_min;
    if (panel_w > win_w / 2)   panel_w = win_w / 2;

    int title_h  = rh + IV_PAD;
    int content_y = title_h;
    int content_h = win_h - title_h - status_h;
    int img_x    = panel_w + 2;
    int img_w    = win_w - img_x;

    // ── Background ─────────────────────────────────────────────────────────
    draw_rect(0, 0, (float)win_w, (float)win_h, 0.06f, 0.06f, 0.08f, 1.f);

    // ── Title bar ──────────────────────────────────────────────────────────
    draw_rect(0, 0, (float)win_w, (float)title_h, 0.12f, 0.12f, 0.18f, 1.f);
    draw_rect(0, (float)(title_h-1), (float)win_w, 1, 0.25f, 0.35f, 0.60f, 1.f);
    const char *mode_str = g_iv.remote ? "Felix Chirp — Remote" : "Felix Chirp";
    char title[256];
    snprintf(title, sizeof(title),
             "%s (F5)   ↑↓: navigate   ←→: prev/next   Shift+←→: ±5s   Shift+↑↓: ±30s   Enter/DblClick: open   Space: pause   R: rotate   0: reset zoom   Esc: close",
             mode_str);
    iv_draw_text(title, (float)IV_PAD, (float)title_h * 0.72f, 0.75f, 0.88f, 1.0f, 1.f);

    // ── File browser panel ──────────────────────────────────────────────────
    {
        float px = 0, py = (float)content_y, pw = (float)panel_w, ph = (float)content_h;

        // Panel background + border
        draw_rect(px, py, pw, ph, 0.09f, 0.09f, 0.12f, 1.f);
        draw_rect(px, py, pw, 1,  0.35f, 0.55f, 0.95f, 1.f);
        draw_rect(px, py+ph-1, pw, 1, 0.35f, 0.55f, 0.95f, 1.f);
        draw_rect(px, py, 1, ph, 0.35f, 0.55f, 0.95f, 1.f);
        draw_rect(px+pw-1, py, 1, ph, 0.35f, 0.55f, 0.95f, 1.f);

        // Path row — show zip name when inside a zip
        draw_rect(px, py, pw, (float)rh, 0.08f, 0.10f, 0.10f, 1.f);
        draw_rect(px, py+rh-1, pw, 1, 0.20f, 0.20f, 0.30f, 1.f);
        char path_disp[4100];
        if (g_iv.in_zip) {
            const char *zname = strrchr(g_iv.zip_file, '/');
            if (!zname) zname = strrchr(g_iv.zip_file, '\\');
            snprintf(path_disp, sizeof(path_disp), " [ZIP] %s", zname ? zname+1 : g_iv.zip_file);
        } else {
            // Truncate path from the left if it's too long for the panel
            int char_w  = g_font_size > 0 ? g_font_size : 16;
            int avail   = (int)(pw - 2 * IV_PAD);
            int max_ch  = avail / char_w;
            if (max_ch < 6) max_ch = 6;
            int plen    = (int)strlen(g_iv.path);
            if (plen + 1 <= max_ch) {
                snprintf(path_disp, sizeof(path_disp), " %s", g_iv.path);
            } else {
                // Show ellipsis + tail: "…/rest/of/path"
                const char *tail = g_iv.path + plen - (max_ch - 2);
                // Align to next '/' boundary
                const char *sl = strchr(tail, '/');
                if (!sl) sl = tail;
                snprintf(path_disp, sizeof(path_disp), " \xe2\x80\xa6%s", sl);
            }
        }
        iv_draw_text(path_disp, px + IV_PAD, py + rh * 0.72f, 0.45f, 0.80f, 0.45f, 1.f);

        float list_y = py + rh;
        int visible_rows = (content_h - rh) / rh;
        int n = (int)g_iv.entries.size();

        // Clamp scroll
        if (g_iv.selected < g_iv.scroll_top)
            g_iv.scroll_top = g_iv.selected;
        if (g_iv.selected >= g_iv.scroll_top + visible_rows)
            g_iv.scroll_top = g_iv.selected - visible_rows + 1;

        for (int i = 0; i < visible_rows; i++) {
            int idx = g_iv.scroll_top + i;
            if (idx >= n) break;
            const IVEntry &e = g_iv.entries[idx];
            float ry = list_y + i * rh;
            bool sel = (idx == g_iv.selected);

            if      (sel)    draw_rect(px+1, ry, pw-2, (float)rh, 0.18f, 0.38f, 0.75f, 0.90f);
            else if (i%2==0) draw_rect(px,   ry, pw,   (float)rh, 1.f,   1.f,   1.f,   0.02f);

            float nr, ng, nb;
            if (sel) {
                nr = ng = nb = 1.f;
            } else if (e.is_dir) {
                nr = 0.90f; ng = 0.90f; nb = 0.55f;
            } else if (e.is_zip && e.has_cdg_pair) {
                nr = 0.40f; ng = 1.00f; nb = 0.60f; // green — CDG zip
            } else if (e.is_zip) {
                nr = 0.95f; ng = 0.75f; nb = 0.30f; // orange — plain zip
            } else if (e.is_audio && e.has_cdg_pair) {
                nr = 0.40f; ng = 1.00f; nb = 0.60f; // green — has CDG
            } else if (e.is_audio) {
                nr = 0.50f; ng = 0.85f; nb = 1.00f; // cyan — audio only
            } else if (e.is_cdg) {
                nr = 0.80f; ng = 0.60f; nb = 1.00f; // purple — CDG graphics
            } else {
                nr = 0.82f; ng = 0.82f; nb = 0.92f;
            }

            // Highlight currently playing track
            bool is_playing_this = g_iv.audio_playing &&
                strcmp(e.name, g_iv.audio_label) == 0;
            if (is_playing_this && !sel)
                draw_rect(px+1, ry, pw-2, (float)rh, 0.10f, 0.30f, 0.15f, 0.60f);

            char name_buf[520];
            const char *prefix = e.is_dir  ? "[DIR] " :
                                 (e.is_zip && e.has_cdg_pair) ? "[CDG] " :
                                 e.is_zip  ? "[ZIP] " :
                                 e.is_audio ? (e.has_cdg_pair ? "[CDG] " : "[AUD] ") :
                                 e.is_cdg  ? "[.CDG]" :
                                             "      ";
            // Calculate how many characters fit in the panel (approx, monospace)
            // Reserve ~7 chars on right for the size column + padding
            int prefix_len = (int)strlen(prefix);
            int avail_px   = (int)(pw - 2 * IV_PAD - 64);  // 64px reserved for size
            int char_w     = g_font_size > 0 ? g_font_size : 16;
            int max_name_chars = avail_px / char_w;
            if (max_name_chars < 8) max_name_chars = 8;
            int max_for_name = max_name_chars - prefix_len;
            if (max_for_name < 4) max_for_name = 4;

            char trunc_name[520];
            iv_truncate_name(e.name, max_for_name, trunc_name, sizeof(trunc_name));
            snprintf(name_buf, sizeof(name_buf), "%s%s", prefix, trunc_name);
            iv_draw_text(name_buf, px + IV_PAD, ry + rh * 0.72f, nr, ng, nb, 1.f);

            if (!e.is_dir && e.size > 0) {
                char sz[32]; fmt_size_iv(e.size, sz, sizeof(sz));
                iv_draw_text(sz, px + pw - IV_PAD - 56, ry + rh * 0.72f,
                             0.45f, 0.55f, 0.65f, 1.f);
            }
        }

        // Scrollbar
        if (n > visible_rows) {
            float lh = (float)(visible_rows * rh);
            float th = lh * visible_rows / n;
            float ty2 = list_y + (lh - th) * g_iv.scroll_top / (float)(n - visible_rows);
            draw_rect(px+pw-5, list_y, 5, lh, 0.12f, 0.12f, 0.20f, 1.f);
            draw_rect(px+pw-5, ty2,    5, th, 0.35f, 0.50f, 0.90f, 1.f);
        }
    }

    // Divider
    draw_rect((float)panel_w, (float)content_y, 2, (float)content_h, 0.20f, 0.20f, 0.30f, 1.f);

    // ── Display area (image / CDG / audio) ─────────────────────────────────
    {
        float ix = (float)img_x, iy = (float)content_y;
        float iw = (float)img_w,  ih = (float)content_h;

        draw_rect(ix, iy, iw, ih, 0.04f, 0.04f, 0.06f, 1.f);

        if (g_iv.cdg_display && (g_use_sdl_renderer ? (bool)g_iv.sdl_cdg_tex : (bool)g_iv.cdg_tex)) {
            // CD+G display — render palette texture directly
            float scale = std::min(iw / (float)CDG_WIDTH, ih / (float)CDG_HEIGHT);
            float dw = CDG_WIDTH * scale, dh = CDG_HEIGHT * scale;
            float dx = ix + (iw - dw) * 0.5f;
            float dy = iy + (ih - dh) * 0.5f;
            draw_rect(dx, dy, dw, dh, 0.f, 0.f, 0.f, 1.f);
            iv_draw_image_tex(g_iv.sdl_cdg_tex, g_iv.cdg_tex, dx, dy, dw, dh);

            // Playback progress bar at bottom of CDG area
            if (g_iv.audio_playing) {
                float bar_y  = dy + dh + 4.f;
                float bar_w  = dw;
                double total = Mix_MusicDuration(g_iv.music);
                float prog   = (total > 0) ? (float)(g_iv.audio_position / total) : 0.f;
                if (prog > 1.f) prog = 1.f;
                draw_rect(dx, bar_y, bar_w, 6.f, 0.15f, 0.15f, 0.20f, 1.f);
                draw_rect(dx, bar_y, bar_w * prog, 6.f, 0.30f, 0.80f, 0.40f, 1.f);
            }

        } else if (g_iv.audio_playing || g_iv.audio_paused) {
            // ── Audio-only display: multi-mode visualizer ─────────────────
            const int NUM_BANDS = 48;
            double bands[NUM_BANDS];
            double t = g_iv.audio_position;
            float  alpha_mul = g_iv.audio_paused ? 0.35f : 1.0f;

            // Sine-bank pseudo-spectrum (no FFT available here)
            for (int i = 0; i < NUM_BANDS; i++) {
                double freq  = 0.4 + i * 0.18;
                double phase = t * freq * 6.2832;
                double v = 0.5 + 0.5  * sin(phase + i * 0.4)
                               + 0.25 * sin(phase * 2.1 + i * 0.7)
                               + 0.10 * sin(phase * 3.3 + i * 1.1);
                bands[i] = v / 1.85;
            }

            // ── Helper: HSV→RGB ───────────────────────────────────────────
            auto hsv_rgb = [](float hue, float sat, float val,
                               float &r, float &g2, float &b) {
                int   hi = (int)(hue * 6.f) % 6;
                float f  = hue * 6.f - (int)(hue * 6.f);
                float p  = val * (1.f - sat);
                float q  = val * (1.f - f * sat);
                float tv = val * (1.f - (1.f - f) * sat);
                switch (hi) {
                    case 0: r=val; g2=tv;  b=p;   break;
                    case 1: r=q;   g2=val; b=p;   break;
                    case 2: r=p;   g2=val; b=tv;  break;
                    case 3: r=p;   g2=q;   b=val; break;
                    case 4: r=tv;  g2=p;   b=val; break;
                    default:r=val; g2=p;   b=q;   break;
                }
            };

            // ── Visualizer button (top-right of display area) ─────────────
            static const char *vis_names[] = {
                "Symmetry", "Radial", "Scope", "Starfield"
            };
            const int NUM_VIS = 4;
            float btn_w = 90.f, btn_h = (float)rh;
            float btn_x = ix + iw - btn_w - IV_PAD;
            float btn_y = iy + IV_PAD;
            draw_rect(btn_x, btn_y, btn_w, btn_h, 0.18f, 0.22f, 0.35f, 0.90f);
            draw_rect(btn_x, btn_y, btn_w, 1,     0.35f, 0.55f, 0.95f, 1.f);
            draw_rect(btn_x, btn_y+btn_h-1, btn_w, 1, 0.35f, 0.55f, 0.95f, 1.f);
            draw_rect(btn_x, btn_y, 1, btn_h,     0.35f, 0.55f, 0.95f, 1.f);
            draw_rect(btn_x+btn_w-1, btn_y, 1, btn_h, 0.35f, 0.55f, 0.95f, 1.f);
            char btn_label[64];
            snprintf(btn_label, sizeof(btn_label), "V: %s",
                     vis_names[g_iv.vis_mode % NUM_VIS]);
            iv_draw_text(btn_label,
                         btn_x + 6.f,
                         btn_y + btn_h * 0.72f,
                         0.75f, 0.88f, 1.00f, 1.f);

            // ── Shared progress / track UI at bottom ──────────────────────
            double total   = g_iv.music ? Mix_MusicDuration(g_iv.music) : 0.0;
            float  prog    = (total > 0) ? (float)(t / total) : 0.f;
            if (prog > 1.f) prog = 1.f;
            float bar_y  = iy + ih - (float)rh * 2.5f;
            float bar_x  = ix + iw * 0.05f;
            float bar_w  = iw * 0.90f;

            // Track label
            float lbl_y = iy + ih * 0.07f;
            iv_draw_text(g_iv.audio_label,
                         ix + iw*0.5f - strlen(g_iv.audio_label)*g_font_size*0.3f,
                         lbl_y, 0.90f, 0.95f, 1.00f, 1.f);
            if (g_iv.audio_paused) {
                const char *ps = "\xe2\x80\x96 PAUSED";
                iv_draw_text(ps,
                             ix + iw*0.5f - strlen(ps)*g_font_size*0.3f,
                             lbl_y + rh, 1.00f, 0.75f, 0.30f, 1.f);
            }

            // Progress bar
            draw_rect(bar_x, bar_y, bar_w, 6.f, 0.15f, 0.15f, 0.20f, 1.f);
            if (prog > 0.f)
                draw_rect(bar_x, bar_y, bar_w * prog, 6.f, 0.30f, 0.80f, 0.40f, 1.f);

            // Time
            int cur_s = (int)t, tot_s = (int)total;
            char timebuf[32];
            snprintf(timebuf, sizeof(timebuf), "%d:%02d / %d:%02d",
                     cur_s/60, cur_s%60, tot_s/60, tot_s%60);
            iv_draw_text(timebuf,
                         bar_x + bar_w*0.5f - strlen(timebuf)*g_font_size*0.3f,
                         bar_y + 14.f, 0.55f, 0.65f, 0.75f, 1.f);

            const char *ctrl = "Space: pause/resume   S: stop   ←→: prev/next   V: next viz";
            iv_draw_text(ctrl,
                         ix + iw*0.5f - strlen(ctrl)*g_font_size*0.3f,
                         bar_y + (float)rh + 4.f,
                         0.40f, 0.45f, 0.55f, 1.f);

            // ── Visualizer drawing area (between label and progress bar) ──
            float viz_top = lbl_y + rh * 1.8f;
            float viz_bot = bar_y - (float)rh * 0.5f;
            float viz_h   = viz_bot - viz_top;
            float viz_mid = viz_top + viz_h * 0.5f;
            float viz_w   = iw * 0.96f;
            float viz_x   = ix + (iw - viz_w) * 0.5f;

            int mode = g_iv.vis_mode % NUM_VIS;

            if (mode == 0) {
                // ── 0: Symmetry Cascade ──────────────────────────────────
                float band_pw = viz_w / NUM_BANDS;
                float max_half = viz_h * 0.48f;
                for (int i = 0; i < NUM_BANDS; i++) {
                    float amp = (float)bands[i];
                    float h   = amp * max_half;
                    float bx  = viz_x + i * band_pw;
                    float hue = (float)i / NUM_BANDS;
                    float r, g2, b;
                    hsv_rgb(hue, fminf(1.f, amp*2.f), 1.f, r, g2, b);
                    draw_rect(bx+1.f, viz_mid - h, band_pw-2.f, h,
                              r, g2, b, alpha_mul * 0.90f);
                    draw_rect(bx+1.f, viz_mid,     band_pw-2.f, h,
                              r, g2, b, alpha_mul * 0.50f);
                }

            } else if (mode == 1) {
                // ── 1: Radial Burst ──────────────────────────────────────
                float cx2  = viz_x + viz_w * 0.5f;
                float cy2  = viz_top + viz_h * 0.5f;
                float maxR = fminf(viz_w, viz_h) * 0.46f;
                float innerR = maxR * 0.18f;
                // Rotating spoke angle driven by time
                float rot = (float)(fmod(t * 0.4, 6.2832));
                for (int i = 0; i < NUM_BANDS; i++) {
                    float angle = rot + (float)i / NUM_BANDS * 6.2832f;
                    float amp   = (float)bands[i];
                    float outerR = innerR + amp * (maxR - innerR);
                    float x1 = cx2 + cosf(angle) * innerR;
                    float y1 = cy2 + sinf(angle) * innerR;
                    float x2 = cx2 + cosf(angle) * outerR;
                    float y2 = cy2 + sinf(angle) * outerR;
                    float hue = fmodf((float)i / NUM_BANDS + (float)t * 0.05f, 1.f);
                    float r, g2, b;
                    hsv_rgb(hue, 1.f, 1.f, r, g2, b);
                    // Draw as a thin rect along the spoke direction
                    float dx2 = x2 - x1, dy2 = y2 - y1;
                    float len = sqrtf(dx2*dx2 + dy2*dy2);
                    if (len < 1.f) continue;
                    float nx2 = -dy2/len * 2.f, ny2 = dx2/len * 2.f;
                    // 4-corner quad — draw as two axis-rects (approximation)
                    // Use screen-aligned rect at spoke position
                    float spoke_w = fmaxf(2.f, viz_w / NUM_BANDS * 0.7f);
                    float sx = x1 + (x2-x1)*0.5f - spoke_w*0.5f;
                    float sy = fminf(y1, y2);
                    float sw = spoke_w;
                    float sh = fmaxf(2.f, fabsf(y2-y1));
                    draw_rect(sx, sy, sw, sh, r, g2, b, alpha_mul * 0.85f);
                    (void)nx2; (void)ny2;
                }
                // Centre pulse circle (simulated with a filled rect square)
                float pulse = 0.4f + 0.6f * (float)bands[0];
                float pr = innerR * pulse;
                draw_rect(cx2 - pr, cy2 - pr*0.5f, pr*2.f, pr,
                          1.f, 0.8f, 0.3f, alpha_mul * 0.6f);

            } else if (mode == 2) {
                // ── 2: Oscilloscope ──────────────────────────────────────
                // Draw grid
                draw_rect(viz_x, viz_mid - 0.5f, viz_w, 1.f,
                          0.25f, 0.25f, 0.35f, 0.5f);
                for (int gi = 1; gi < 4; gi++) {
                    float gy = viz_top + viz_h * gi / 4.f;
                    draw_rect(viz_x, gy, viz_w, 1.f, 0.20f, 0.20f, 0.28f, 0.4f);
                }
                // Draw waveform as a series of thin vertical rects connecting samples
                const int SCOPE_PTS = 128;
                float prev_y = viz_mid;
                for (int i = 0; i < SCOPE_PTS; i++) {
                    float fi   = (float)i / (SCOPE_PTS - 1);
                    // Build a sample from overlapping sine bands
                    double phase = t * 6.2832 * (1.0 + fi * 4.0) + fi * 12.0;
                    float sample = 0.f;
                    for (int b = 0; b < 6; b++)
                        sample += (float)(bands[b * 8] * sin(phase * (b+1) * 0.7 + b));
                    sample /= 6.f;
                    float sy2 = viz_mid - sample * viz_h * 0.44f;
                    sy2 = fmaxf(viz_top, fminf(viz_bot, sy2));
                    float sx2 = viz_x + fi * viz_w;
                    float hue = fmodf(fi + (float)t * 0.08f, 1.f);
                    float r, g2, b;
                    hsv_rgb(hue, 0.7f, 1.f, r, g2, b);
                    float seg_h = fabsf(sy2 - prev_y) + 2.f;
                    float seg_y = fminf(sy2, prev_y);
                    draw_rect(sx2, seg_y, fmaxf(2.f, viz_w / SCOPE_PTS + 1.f),
                              seg_h, r, g2, b, alpha_mul * 0.90f);
                    prev_y = sy2;
                }

            } else {
                // ── 3: Starfield ─────────────────────────────────────────
                // Persistent star state
                static struct Star {
                    float x, y, speed, size, hue;
                    bool  alive;
                } stars[200] = {};
                static bool stars_init = false;
                if (!stars_init) {
                    for (auto &s : stars) {
                        s.x     = (float)rand() / RAND_MAX;
                        s.y     = (float)rand() / RAND_MAX;
                        s.speed = 0.01f + (float)rand()/RAND_MAX * 0.04f;
                        s.size  = 1.f + (float)rand()/RAND_MAX * 3.f;
                        s.hue   = (float)rand()/RAND_MAX;
                        s.alive = true;
                    }
                    stars_init = true;
                }
                // Update: move stars outward from centre, reset when off-screen
                float beat = (float)(0.5 + 0.5 * sin(t * 4.0));  // pulse
                for (auto &s : stars) {
                    float dx2 = s.x - 0.5f, dy2 = s.y - 0.5f;
                    float dist = sqrtf(dx2*dx2 + dy2*dy2) + 0.001f;
                    float speed = s.speed * (1.f + beat * (float)bands[(int)(s.hue*47)]);
                    s.x += dx2 / dist * speed;
                    s.y += dy2 / dist * speed;
                    s.hue = fmodf(s.hue + 0.002f, 1.f);
                    if (s.x < 0 || s.x > 1 || s.y < 0 || s.y > 1) {
                        // Reset near centre
                        s.x     = 0.5f + ((float)rand()/RAND_MAX - 0.5f) * 0.08f;
                        s.y     = 0.5f + ((float)rand()/RAND_MAX - 0.5f) * 0.08f;
                        s.speed = 0.005f + (float)rand()/RAND_MAX * 0.035f;
                        s.size  = 1.f + (float)rand()/RAND_MAX * 4.f;
                        s.hue   = (float)rand()/RAND_MAX;
                    }
                }
                // Draw stars
                for (auto &s : stars) {
                    float sx2 = viz_x + s.x * viz_w;
                    float sy2 = viz_top + s.y * viz_h;
                    float dx2 = s.x - 0.5f, dy2 = s.y - 0.5f;
                    float dist = sqrtf(dx2*dx2 + dy2*dy2);
                    float brightness = fminf(1.f, dist * 3.f);
                    float r, g2, b;
                    hsv_rgb(s.hue, 0.6f, brightness, r, g2, b);
                    float sz = s.size * (0.5f + dist * 2.f);
                    draw_rect(sx2 - sz*0.5f, sy2 - sz*0.5f, sz, sz,
                              r, g2, b, alpha_mul * brightness * 0.90f);
                }
                // Vignette hint text
                const char *sft = "Starfield";
                iv_draw_text(sft,
                             viz_x + viz_w - strlen(sft)*g_font_size*0.6f - IV_PAD,
                             viz_top + IV_PAD,
                             0.30f, 0.35f, 0.50f, 0.5f);
            }

        } else if (g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex) {
            // ── Image display with zoom / pan / rotation ──────────────────
            // tw/th = texture dimensions in the rotated orientation (for fit calc)
            float tw = (float)(g_iv.img_rot % 180 == 0 ? g_iv.tex_w : g_iv.tex_h);
            float th = (float)(g_iv.img_rot % 180 == 0 ? g_iv.tex_h : g_iv.tex_w);
            float base_scale = std::min(iw / tw, ih / th);
            float effective  = base_scale * g_iv.zoom;

            // dw/dh = display size of the rotated image footprint
            float dw = tw * effective;
            float dh = th * effective;

            // Centre + pan offset
            float cx  = ix + iw * 0.5f;
            float cy2 = iy + ih * 0.5f;
            float dx  = cx - dw * 0.5f + g_iv.pan_x;
            float dy  = cy2 - dh * 0.5f + g_iv.pan_y;
            float img_cx = dx + dw * 0.5f;
            float img_cy = dy + dh * 0.5f;

            // Background rect fits the rotated footprint
            draw_rect(dx, dy, dw, dh, 0.25f, 0.25f, 0.25f, 1.f);

            // Draw with original (unswapped) tex dimensions — the rotation
            // function rotates the quad, so we pass raw tex size * scale.
            float raw_w = (float)g_iv.tex_w * effective;
            float raw_h = (float)g_iv.tex_h * effective;
            iv_draw_image_rotated(img_cx, img_cy, raw_w, raw_h, g_iv.img_rot);

            // Zoom level hint (fade out quickly — only show when != 1.0)
            if (g_iv.zoom != 1.0f) {
                char zoom_buf[32];
                snprintf(zoom_buf, sizeof(zoom_buf), "%.0f%%", g_iv.zoom * 100.f);
                iv_draw_text(zoom_buf,
                             ix + iw - strlen(zoom_buf)*g_font_size*0.6f - IV_PAD,
                             iy + ih - rh,
                             0.80f, 0.85f, 1.00f, 0.75f);
            }
            if (g_iv.img_rot != 0) {
                char rot_buf[16];
                snprintf(rot_buf, sizeof(rot_buf), "%d\xc2\xb0", g_iv.img_rot);
                iv_draw_text(rot_buf,
                             ix + IV_PAD,
                             iy + ih - rh,
                             0.80f, 0.85f, 1.00f, 0.75f);
            }

        } else if (g_iv.error[0]) {
            iv_draw_text(g_iv.error,
                         ix + iw*0.5f - strlen(g_iv.error)*g_font_size*0.3f,
                         iy + ih*0.5f, 1.f, 0.4f, 0.4f, 1.f);
        } else {
            const char *hint = "Select an image, audio, or CD+G file";
            iv_draw_text(hint,
                         ix + iw*0.5f - strlen(hint)*g_font_size*0.3f,
                         iy + ih*0.5f, 0.35f, 0.40f, 0.50f, 1.f);
        }
    }

    // ── Status bar ─────────────────────────────────────────────────────────
    {
        float st_y = (float)(win_h - status_h);
        draw_rect(0, st_y, (float)win_w, 1, 0.20f, 0.20f, 0.30f, 1.f);
        draw_rect(0, st_y+1, (float)win_w, (float)status_h-1, 0.09f, 0.09f, 0.13f, 1.f);

        char status[512];
        if (g_iv.cdg_display != nullptr) {
            int pkt = g_iv.cdg_display->current_packet, tot = g_iv.cdg_display->packet_count;
            snprintf(status, sizeof(status), "  \xe2\x99\xab %s   CD+G: packet %d/%d  (%.1fs)",
                     g_iv.audio_label, pkt, tot, g_iv.audio_position);
        } else if (g_iv.audio_playing || g_iv.audio_paused) {
            snprintf(status, sizeof(status), "  \xe2\x99\xab %s   %.1fs%s",
                     g_iv.audio_label, g_iv.audio_position,
                     g_iv.audio_paused ? "  [PAUSED]" : "");
        } else if (g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex) {
            snprintf(status, sizeof(status), "  %s   %dx%d px",
                     g_iv.img_label, g_iv.tex_w, g_iv.tex_h);
        } else {
            int n = (int)g_iv.entries.size();
            int imgs = 0, audio = 0, cdgs = 0;
            for (auto &e : g_iv.entries) {
                if (!e.is_dir && !e.is_zip) {
                    if (e.is_audio) audio++;
                    else if (e.is_cdg) cdgs++;
                    else imgs++;
                }
            }
            snprintf(status, sizeof(status), "  %d items  (%d images, %d audio, %d cdg)",
                     n, imgs, audio, cdgs);
        }
        iv_draw_text(status, (float)IV_PAD, st_y + status_h * 0.72f,
                     0.60f, 0.70f, 0.85f, 1.f);
    }

    gl_flush_verts();
}

// ============================================================================
// INPUT
// ============================================================================

bool iv_keydown(SDL_Keycode sym) {
    if (!g_iv.visible) return false;

    int n = (int)g_iv.entries.size();

    switch (sym) {
    case SDLK_ESCAPE: case SDLK_F5:
        iv_close();
        return true;

    case SDLK_PAGEUP:
        g_iv.selected = std::max(0, g_iv.selected - 10);
        return true;

    case SDLK_PAGEDOWN:
        g_iv.selected = std::min(n - 1, g_iv.selected + 10);
        return true;

    case SDLK_HOME:
        g_iv.selected = 0;
        return true;

    case SDLK_END:
        g_iv.selected = n > 0 ? n - 1 : 0;
        return true;

    case SDLK_RETURN: case SDLK_KP_ENTER:
        iv_enter_selected();
        return true;

    case SDLK_SPACE:
        // If audio is playing/paused, toggle pause; otherwise open selected
        if (g_iv.audio_playing) {
            Mix_PauseMusic();
            g_iv.audio_paused  = true;
            g_iv.audio_playing = false;
        } else if (g_iv.audio_paused) {
            Mix_ResumeMusic();
            g_iv.audio_playing = true;
            g_iv.audio_paused  = false;
            // Adjust start_ticks to account for paused time
            g_iv.audio_start_ticks = (double)SDL_GetTicks() - g_iv.audio_position * 1000.0;
        } else {
            iv_enter_selected();
        }
        return true;

    case SDLK_s:
        if (g_iv.audio_playing || g_iv.audio_paused) {
            iv_stop_audio();
            iv_cdg_free();
            return true;
        }
        // No audio active — fall through to first-letter jump
        goto first_letter_jump;

    case SDLK_r:
        // Rotate image 90° clockwise (only when an image is displayed)
        if (g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex) {
            g_iv.img_rot = (g_iv.img_rot + 90) % 360;
            // Swap pan axes to keep image centred after rotation
            float tmp = g_iv.pan_x; g_iv.pan_x = -g_iv.pan_y; g_iv.pan_y = tmp;
            return true;
        }
        goto first_letter_jump;

    case SDLK_0: case SDLK_KP_0:
        // Reset zoom and pan
        if (g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex) {
            g_iv.zoom  = 1.0f;
            g_iv.pan_x = 0.0f;
            g_iv.pan_y = 0.0f;
            return true;
        }
        return true;

    case SDLK_v:
        // Cycle visualizer mode (only when audio is playing/paused, else letter-jump)
        if (g_iv.audio_playing || g_iv.audio_paused) {
            g_iv.vis_mode = (g_iv.vis_mode + 1) % 4;
            return true;
        }
        goto first_letter_jump;

    case SDLK_LEFT:
    case SDLK_RIGHT: {
        SDL_Keymod mod = SDL_GetModState();
        // Shift+Left/Right = seek ±5 seconds when audio is playing
        if ((mod & KMOD_SHIFT) && (g_iv.audio_playing || g_iv.audio_paused)) {
            double delta = (sym == SDLK_RIGHT) ? 5.0 : -5.0;
            double newpos = g_iv.audio_position + delta;
            if (newpos < 0.0) newpos = 0.0;
            if (Mix_SetMusicPosition(newpos) == 0) {
                g_iv.audio_position    = newpos;
                g_iv.audio_start_ticks = (double)SDL_GetTicks() - newpos * 1000.0;
                // CDG must replay from start to catch up to new position
                if (g_iv.cdg_display != nullptr) {
                    cdg_reset(g_iv.cdg_display);
                }
            }
            return true;
        }
        // No shift — navigate to prev/next playable file
        std::vector<int> playable;
        for (int i = 0; i < n; i++) {
            const IVEntry &ei = g_iv.entries[i];
            if (ei.is_dir) continue;
            if (ei.is_cdg && !ei.is_zip) continue;  // skip bare .cdg sidecars
            if (ei.is_zip && !ei.has_cdg_pair) continue;  // skip non-CDG zips in nav
            playable.push_back(i);
        }
        if (playable.empty()) return true;

        int pos = -1;
        for (int i = 0; i < (int)playable.size(); i++)
            if (playable[i] == g_iv.selected) { pos = i; break; }

        if (pos < 0) pos = (sym == SDLK_RIGHT) ? 0 : (int)playable.size() - 1;
        else if (sym == SDLK_RIGHT)
            pos = (pos + 1) % (int)playable.size();
        else
            pos = (pos - 1 + (int)playable.size()) % (int)playable.size();

        g_iv.selected = playable[pos];
        iv_enter_selected();
        return true;
    }

    case SDLK_UP:
    case SDLK_DOWN: {
        SDL_Keymod mod = SDL_GetModState();
        // Shift+Up/Down = seek ±30 seconds when audio is playing
        if ((mod & KMOD_SHIFT) && (g_iv.audio_playing || g_iv.audio_paused)) {
            double delta = (sym == SDLK_UP) ? 30.0 : -30.0;
            double newpos = g_iv.audio_position + delta;
            if (newpos < 0.0) newpos = 0.0;
            if (Mix_SetMusicPosition(newpos) == 0) {
                g_iv.audio_position    = newpos;
                g_iv.audio_start_ticks = (double)SDL_GetTicks() - newpos * 1000.0;
                if (g_iv.cdg_display != nullptr) {
                    cdg_reset(g_iv.cdg_display);
                }
            }
            return true;
        }
        // No shift — normal list navigation
        if (sym == SDLK_UP) { if (g_iv.selected > 0) g_iv.selected--; }
        else                 { if (g_iv.selected < n-1) g_iv.selected++; }
        return true;
    }

    case SDLK_BACKSPACE:
        if (g_iv.audio_playing || g_iv.audio_paused) {
            iv_stop_audio(); iv_cdg_free();
        } else if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (strcmp(g_iv.entries[i].name, "..") == 0) {
                    g_iv.selected = i;
                    iv_enter_selected();
                    break;
                }
            }
        }
        return true;

    default: {
        first_letter_jump:
        // First-letter jump: press a letter/digit to jump to the first entry
        // whose name starts with that character (case-insensitive). Pressing
        // the same key again cycles through all matches, wrapping around.
        if (sym < 32 || sym > 126) return true;
        char ch = (char)tolower((unsigned char)sym);
        if (!isalpha((unsigned char)ch) && !isdigit((unsigned char)ch)) return true;

        std::vector<int> matches;
        for (int i = 0; i < n; i++) {
            const char *nm = g_iv.entries[i].name;
            if (strcmp(nm, "..") == 0) continue;
            if (tolower((unsigned char)nm[0]) == (unsigned char)ch)
                matches.push_back(i);
        }
        if (matches.empty()) return true;

        // If already on a match advance to next, otherwise jump to first.
        int next = matches[0];
        for (int i = 0; i < (int)matches.size(); i++) {
            if (matches[i] == g_iv.selected) {
                next = matches[(i + 1) % (int)matches.size()];
                break;
            }
        }
        g_iv.selected = next;
        return true;
    }
    } // switch
}

// ============================================================================
// MOUSE SUPPORT — zoom, pan, file list scroll, click to select
// ============================================================================

// Returns the image display rect (ix, iy, iw, ih) for the current window size.
static void iv_image_rect(int win_w, int win_h,
                          float &ix, float &iy, float &iw, float &ih) {
    int rh       = iv_row_h();
    int status_h = rh + IV_PAD;
    int title_h  = rh + IV_PAD;
    int panel_w  = (int)(win_w * 0.28f);
    int panel_w_min = rh * 14;
    if (panel_w < panel_w_min) panel_w = panel_w_min;
    if (panel_w > win_w / 2)   panel_w = win_w / 2;
    ix = (float)(panel_w + 2);
    iy = (float)title_h;
    iw = (float)(win_w - (panel_w + 2));
    ih = (float)(win_h - title_h - status_h);
}

// Returns the panel width for hit-testing the file list.
static int iv_panel_width(int win_w) {
    int rh = iv_row_h();
    int pw = (int)(win_w * 0.28f);
    int mn = rh * 14;
    if (pw < mn)        pw = mn;
    if (pw > win_w / 2) pw = win_w / 2;
    return pw;
}

bool iv_mousewheel(int x, int y, int delta_y, int win_w, int win_h) {
    if (!g_iv.visible) return false;

    float ix, iy, iw, ih;
    iv_image_rect(win_w, win_h, ix, iy, iw, ih);

    int panel_w = iv_panel_width(win_w);

    if (x < panel_w) {
        // Scroll the file list — move both scroll_top and selected together
        // so the render clamp (which keeps selected visible) doesn't fight us.
        int rh2 = iv_row_h();
        int title_h2  = rh2 + IV_PAD;
        int status_h2 = rh2 + IV_PAD;
        int content_h2 = win_h - title_h2 - status_h2;
        int visible_rows = (content_h2 - rh2) / rh2;
        int n = (int)g_iv.entries.size();
        int step = (delta_y > 0) ? -3 : 3;  // wheel up = step toward top (lower index)
        g_iv.scroll_top = std::max(0, std::min(g_iv.scroll_top + step, std::max(0, n - visible_rows)));
        // Keep selected within the visible window
        if (g_iv.selected < g_iv.scroll_top)
            g_iv.selected = g_iv.scroll_top;
        if (g_iv.selected >= g_iv.scroll_top + visible_rows)
            g_iv.selected = g_iv.scroll_top + visible_rows - 1;
        g_iv.selected = std::max(0, std::min(g_iv.selected, n - 1));
        return true;
    }

    // Zoom the image around the cursor position
    bool has_image = g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex;
    if (!has_image) return true;

    float old_zoom = g_iv.zoom;
    float factor   = (delta_y > 0) ? 1.15f : (1.0f / 1.15f);
    g_iv.zoom = std::max(0.1f, std::min(g_iv.zoom * factor, 16.0f));

    // Zoom towards cursor: adjust pan so the point under cursor stays fixed.
    float cx = ix + iw * 0.5f;
    float cy = iy + ih * 0.5f;
    float mouse_rel_x = (float)x - cx;
    float mouse_rel_y = (float)y - cy;
    float zoom_ratio  = g_iv.zoom / old_zoom;
    g_iv.pan_x = mouse_rel_x + (g_iv.pan_x - mouse_rel_x) * zoom_ratio;
    g_iv.pan_y = mouse_rel_y + (g_iv.pan_y - mouse_rel_y) * zoom_ratio;

    return true;
}

bool iv_mousedown(int x, int y, int button, int win_w, int win_h) {
    if (!g_iv.visible) return false;

    int rh      = iv_row_h();
    int title_h = rh + IV_PAD;
    int panel_w = iv_panel_width(win_w);

    if (button == SDL_BUTTON_LEFT) {
        if (x < panel_w && y >= title_h) {
            // Click in file list: select entry, double-click opens it
            int list_y    = title_h + rh;  // path row inside panel
            int status_h  = rh + IV_PAD;
            int content_h = win_h - title_h - status_h;
            int visible_rows = (content_h - rh) / rh;
            int row = (y - list_y) / rh;
            int idx = g_iv.scroll_top + row;
            int n   = (int)g_iv.entries.size();
            if (row >= 0 && idx < n) {
                static uint32_t s_last_click_time = 0;
                static int      s_last_click_idx  = -1;
                uint32_t now = SDL_GetTicks();
                if (idx == s_last_click_idx && (now - s_last_click_time) < 400) {
                    // Double-click
                    g_iv.selected = idx;
                    iv_enter_selected();
                } else {
                    g_iv.selected = idx;
                }
                s_last_click_time = now;
                s_last_click_idx  = idx;
            }
            return true;
        }

        // Check visualizer button (top-right of image area) when audio is playing
        if (x >= panel_w && (g_iv.audio_playing || g_iv.audio_paused)) {
            float ix2, iy2, iw2, ih2;
            iv_image_rect(win_w, win_h, ix2, iy2, iw2, ih2);
            float btn_w = 90.f, btn_h = (float)rh;
            float btn_x = ix2 + iw2 - btn_w - IV_PAD;
            float btn_y = iy2 + IV_PAD;
            if ((float)x >= btn_x && (float)x <= btn_x + btn_w &&
                (float)y >= btn_y && (float)y <= btn_y + btn_h) {
                g_iv.vis_mode = (g_iv.vis_mode + 1) % 4;
                return true;
            }
        }

        // Start pan drag in image area
        bool has_image = g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex;
        if (has_image && x >= panel_w) {
            g_iv.drag_active  = true;
            g_iv.drag_start_x = x;
            g_iv.drag_start_y = y;
            g_iv.drag_pan_x0  = g_iv.pan_x;
            g_iv.drag_pan_y0  = g_iv.pan_y;
            return true;
        }
    }

    if (button == SDL_BUTTON_RIGHT) {
        // Right-click in image area resets zoom and pan
        bool has_image = g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex;
        if (has_image && x >= panel_w) {
            g_iv.zoom  = 1.0f;
            g_iv.pan_x = 0.0f;
            g_iv.pan_y = 0.0f;
            return true;
        }
    }

    return true;
}

bool iv_mousemotion(int x, int y, int /*win_w*/, int /*win_h*/) {
    if (!g_iv.visible) return false;
    if (g_iv.drag_active) {
        g_iv.pan_x = g_iv.drag_pan_x0 + (float)(x - g_iv.drag_start_x);
        g_iv.pan_y = g_iv.drag_pan_y0 + (float)(y - g_iv.drag_start_y);
        return true;
    }
    return false;
}

bool iv_mouseup(int /*x*/, int /*y*/, int button) {
    if (!g_iv.visible) return false;
    if (button == SDL_BUTTON_LEFT) {
        g_iv.drag_active = false;
    }
    return true;
}
