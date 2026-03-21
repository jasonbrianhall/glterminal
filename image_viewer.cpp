// image_viewer.cpp — F5 image viewer overlay for Felix Terminal
// Supports local filesystem and (when USESSH) remote SFTP browsing.
// Image decoding via stb_image (no extra library dependency).

// GL must be included first — before stb_image and everything else
#include <GL/glew.h>
#include <GL/gl.h>

// image_viewer.h must come before stb/miniz so its types are visible everywhere
#include "image_viewer.h"

// ---- stb_image (header-only, embedded) ------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // we feed raw bytes, not filenames
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#define STBI_ONLY_PIC
// stb_image.h must be in the include path (https://github.com/nothings/stb)
#include "stb_image.h"

// ---- miniz (zip support) --------------------------------------------------
// Uses the split-header miniz distribution.
#include "miniz.h"
#include "miniz_zip.h"
// ---------------------------------------------------------------------------

#include "gl_renderer.h"
#include "ft_font.h"
#include "term_color.h"

#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctype.h>

#ifdef USESSH
#  include "ssh_session.h"
#  include "sftp_overlay.h"
#  include <libssh2_sftp.h>
#endif

#ifndef _WIN32
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#else
#  include <windows.h>
#  include <shlobj.h>
#endif

// ============================================================================
// GLOBALS
// ============================================================================

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
    for (mz_uint i = 0; i < n; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char fname[512] = {};
        mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
        if (strncmp(fname, "__MACOSX", 8) == 0) continue;
        if (!is_image_ext(fname)) continue;

        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&zip, i, &st);

        IVEntry e{};
        const char *slash = strrchr(fname, '/');
        strncpy(e.name, slash ? slash+1 : fname, sizeof(e.name)-1);
        e.is_zip_entry = true;
        strncpy(e.zip_path,  zip_filepath, sizeof(e.zip_path)-1);
        strncpy(e.zip_entry, fname,        sizeof(e.zip_entry)-1);
        e.size = st.m_uncomp_size;
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
        if (e.is_dir || is_image_ext(e.name) || is_zip_ext(e.name)) {
            if (is_zip_ext(e.name)) e.is_zip = true;
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
        if (e.is_dir || is_image_ext(e.name) || is_zip_ext(e.name)) {
            if (is_zip_ext(e.name)) e.is_zip = true;
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

#ifdef USESSH
// Reuse the SFTP subsystem from sftp_overlay (s_sftp is extern there).
// We call the public sftp_panel helpers indirectly by duplicating the
// list logic here so we don't need to expose s_sftp directly.
static void iv_list_remote(const char *path, std::vector<IVEntry> &out) {
    out.clear();
    // Access the SFTP handle via the public SSH session API
    LIBSSH2_SESSION *sess = ssh_get_session();
    if (!sess) return;

    // We need the sftp handle. sftp_overlay.cpp owns it but doesn't expose it.
    // Use SftpPanel as a vehicle: create a temporary panel, call sftp_panel_refresh.
    // Instead, we open our own SFTP channel for listing (read-only, quick).
    // Note: sftp_init() in sftp_overlay already initialised the subsystem;
    // we open a second handle here for the directory listing to avoid
    // touching sftp_overlay's internal state.
    ssh_session_lock();
    libssh2_session_set_blocking(sess, 1);
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(sess);
    if (!sftp) { libssh2_session_set_blocking(sess, 0); ssh_session_unlock(); return; }

    if (strcmp(path, "/") != 0) {
        IVEntry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
        out.push_back(up);
    }

    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(sftp, path);
    if (dh) {
        char name[512], longentry[1024];
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        while (libssh2_sftp_readdir_ex(dh, name, sizeof(name), longentry, sizeof(longentry), &attrs) > 0) {
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            bool is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                          LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            if (!is_dir && !is_image_ext(name)) continue;
            IVEntry e{};
            strncpy(e.name, name, sizeof(e.name)-1);
            e.is_dir = is_dir;
            e.size   = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
            out.push_back(e);
        }
        libssh2_sftp_closedir(dh);
    }
    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(sess, 0);
    ssh_session_unlock();

    std::stable_sort(out.begin(), out.end(), [](const IVEntry &a, const IVEntry &b){
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return strcmp(a.name, b.name) < 0;
    });
}

// Download a remote file into a heap buffer. Returns nullptr on failure.
static unsigned char *iv_download_remote(const char *path, size_t &out_size) {
    LIBSSH2_SESSION *sess = ssh_get_session();
    if (!sess) return nullptr;

    ssh_session_lock();
    libssh2_session_set_blocking(sess, 1);
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(sess);
    if (!sftp) { libssh2_session_set_blocking(sess, 0); ssh_session_unlock(); return nullptr; }

    LIBSSH2_SFTP_HANDLE *fh = libssh2_sftp_open(sftp, path, LIBSSH2_FXF_READ, 0);
    unsigned char *buf = nullptr;
    out_size = 0;
    if (fh) {
        // Read in chunks, grow buffer
        size_t cap = 256 * 1024;
        buf = (unsigned char *)malloc(cap);
        char tmp[32768];
        ssize_t n;
        while ((n = libssh2_sftp_read(fh, tmp, sizeof(tmp))) > 0) {
            if (out_size + (size_t)n > cap) {
                cap = (cap + (size_t)n) * 2;
                buf = (unsigned char *)realloc(buf, cap);
            }
            memcpy(buf + out_size, tmp, n);
            out_size += n;
        }
        libssh2_sftp_close(fh);
        if (out_size == 0) { free(buf); buf = nullptr; }
    }
    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(sess, 0);
    ssh_session_unlock();
    return buf;
}
#endif // USESSH

// ============================================================================
// TEXTURE LOADING
// ============================================================================

static void iv_free_tex() {
    if (g_iv.tex) { glDeleteTextures(1, &g_iv.tex); g_iv.tex = 0; }
    g_iv.tex_w = g_iv.tex_h = 0;
}

static void iv_upload_texture(const unsigned char *rgba, int w, int h) {
    iv_free_tex();
    glGenTextures(1, &g_iv.tex);
    glBindTexture(GL_TEXTURE_2D, g_iv.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_iv.tex_w = w;
    g_iv.tex_h = h;
}

static void iv_load_image_from_mem(const unsigned char *data, size_t len, const char *label) {
    int w, h, ch;
    unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
    if (!px) {
        snprintf(g_iv.error, sizeof(g_iv.error), "Decode failed: %s", stbi_failure_reason());
        iv_free_tex();
        return;
    }
    g_iv.error[0] = '\0';
    iv_upload_texture(px, w, h);
    stbi_image_free(px);
    strncpy(g_iv.img_label, label, sizeof(g_iv.img_label)-1);
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
    g_iv.remote  = remote;

    if (!remote) {
        strncpy(g_iv.path, iv_home().c_str(), sizeof(g_iv.path)-1);
        iv_list_local(g_iv.path, g_iv.entries);
    } else {
#ifdef USESSH
        // Try to get the remote home dir via a quick pwd-like approach
        strncpy(g_iv.path, "/", sizeof(g_iv.path)-1);
        // If sftp is available, resolve ~ by reading a well-known path
        const char *rh = getenv("HOME");  // fallback — server may differ
        if (rh) strncpy(g_iv.path, rh, sizeof(g_iv.path)-1);
        iv_list_remote(g_iv.path, g_iv.entries);
#else
        g_iv.remote = false;
        strncpy(g_iv.path, iv_home().c_str(), sizeof(g_iv.path)-1);
        iv_list_local(g_iv.path, g_iv.entries);
#endif
    }
}

void iv_close() {
    iv_free_tex();
    g_iv.visible = false;
}

// ============================================================================
// NAVIGATE
// ============================================================================

static void iv_refresh() {
    g_iv.selected   = 0;
    g_iv.scroll_top = 0;
    if (g_iv.in_zip) {
        iv_list_zip(g_iv.zip_file, g_iv.entries);
    } else if (g_iv.remote) {
#ifdef USESSH
        iv_list_remote(g_iv.path, g_iv.entries);
#endif
    } else {
        iv_list_local(g_iv.path, g_iv.entries);
    }
}

static void iv_enter_selected() {
    if (g_iv.entries.empty()) return;
    const IVEntry &e = g_iv.entries[g_iv.selected];

    // ── Inside a zip: load image from zip or go back up ───────────────────
    if (e.is_zip_entry) {
        iv_free_tex();
        g_iv.error[0] = '\0';
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
#ifndef _WIN32
            char *slash = strrchr(tmp, '/');
            if (slash && slash != tmp) { *slash = '\0'; }
            else { tmp[0]='/'; tmp[1]='\0'; }
#else
            char *slash = strrchr(tmp, '\\');
            if (!slash) slash = strrchr(tmp, '/');
            if (slash && slash != tmp) { *slash = '\0'; }
            else { tmp[0]='\\'; tmp[1]='\0'; }
#endif
            strncpy(g_iv.path, tmp, sizeof(g_iv.path)-1);
        } else {
#ifndef _WIN32
            snprintf(newpath, sizeof(newpath), "%s/%s", g_iv.path, e.name);
#else
            snprintf(newpath, sizeof(newpath), "%s\\%s", g_iv.path, e.name);
#endif
            strncpy(g_iv.path, newpath, sizeof(g_iv.path)-1);
        }
        iv_refresh();

    } else if (e.is_zip) {
        // Enter the zip — list its images
        char fullpath[4096];
#ifndef _WIN32
        snprintf(fullpath, sizeof(fullpath), "%s/%s", g_iv.path, e.name);
#else
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", g_iv.path, e.name);
#endif
        strncpy(g_iv.zip_file, fullpath, sizeof(g_iv.zip_file)-1);
        g_iv.in_zip = true;
        g_iv.selected   = 0;
        g_iv.scroll_top = 0;
        iv_list_zip(fullpath, g_iv.entries);

    } else {
        // Load plain image
        iv_free_tex();
        g_iv.error[0] = '\0';
        if (g_iv.remote) {
#ifdef USESSH
            char fullpath[4096];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", g_iv.path, e.name);
            size_t sz = 0;
            unsigned char *buf = iv_download_remote(fullpath, sz);
            if (buf) {
                iv_load_image_from_mem(buf, sz, e.name);
                free(buf);
            } else {
                snprintf(g_iv.error, sizeof(g_iv.error), "Failed to download: %s", e.name);
            }
#endif
        } else {
            char fullpath[4096];
#ifndef _WIN32
            snprintf(fullpath, sizeof(fullpath), "%s/%s", g_iv.path, e.name);
#else
            snprintf(fullpath, sizeof(fullpath), "%s\\%s", g_iv.path, e.name);
#endif
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

// Draw a textured quad using immediate GL (compatible with the existing GL3 setup).
// We temporarily switch to a simple textured draw outside the vertex accumulator.
static void iv_draw_image(float x, float y, float w, float h) {
    if (!g_iv.tex) return;

    // Flush any pending geometry first
    gl_flush_verts();

    // Save GL state
    GLint prev_prog;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);

    // Build and use a minimal textured quad shader (compiled once)
    static GLuint s_tex_prog = 0;
    static GLuint s_tex_vao  = 0;
    static GLuint s_tex_vbo  = 0;
    if (!s_tex_prog) {
        const char *vs =
            "#version 330 core\n"
            "layout(location=0) in vec4 vtx;\n"  // xy + uv
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

    // Get window size from GL viewport
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    float ww = (float)vp[2], wh = (float)vp[3];

    // Convert pixel coords to NDC
    auto px2ndc_x = [&](float px) { return (px / ww) * 2.f - 1.f; };
    auto px2ndc_y = [&](float py) { return 1.f - (py / wh) * 2.f; };  // flip Y

    float x0 = px2ndc_x(x),   y0 = px2ndc_y(y);
    float x1 = px2ndc_x(x+w), y1 = px2ndc_y(y+h);

    float verts[24] = {
        x0, y0, 0.f, 0.f,
        x1, y0, 1.f, 0.f,
        x1, y1, 1.f, 1.f,
        x0, y0, 0.f, 0.f,
        x1, y1, 1.f, 1.f,
        x0, y1, 0.f, 1.f,
    };

    glUseProgram(s_tex_prog);
    glBindVertexArray(s_tex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_tex_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_iv.tex);
    glUniform1i(glGetUniformLocation(s_tex_prog, "tex"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);

    // Restore renderer program
    glUseProgram(prev_prog);
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
    const char *mode_str = g_iv.remote ? "Eye of Felix — Remote" : "Eye of Felix";
    char title[256];
    snprintf(title, sizeof(title),
             "%s (F5)   ↑↓: navigate   ←→: prev/next image   Enter/Space: open   Backspace: up   Esc: close",
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
            snprintf(path_disp, sizeof(path_disp), " %s", g_iv.path);
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

            float nr = sel ? 1.f : (e.is_dir ? 0.90f : (e.is_zip ? 0.95f : 0.82f));
            float ng = sel ? 1.f : (e.is_dir ? 0.90f : (e.is_zip ? 0.75f : 0.82f));
            float nb = sel ? 1.f : (e.is_dir ? 0.55f : (e.is_zip ? 0.30f : 0.92f));

            char name_buf[520];
            snprintf(name_buf, sizeof(name_buf), "%s%s",
                     e.is_dir ? "[DIR] " : (e.is_zip ? "[ZIP] " : "      "), e.name);
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

    // ── Image area ─────────────────────────────────────────────────────────
    {
        float ix = (float)img_x, iy = (float)content_y;
        float iw = (float)img_w,  ih = (float)content_h;

        draw_rect(ix, iy, iw, ih, 0.04f, 0.04f, 0.06f, 1.f);

        if (g_iv.tex) {
            // Fit image into area, preserving aspect ratio
            float scale = std::min(iw / g_iv.tex_w, ih / g_iv.tex_h);
            float dw = g_iv.tex_w * scale;
            float dh = g_iv.tex_h * scale;
            float dx = ix + (iw - dw) * 0.5f;
            float dy = iy + (ih - dh) * 0.5f;

            // Draw checkerboard background for transparent images
            draw_rect(dx, dy, dw, dh, 0.25f, 0.25f, 0.25f, 1.f);
            iv_draw_image(dx, dy, dw, dh);
        } else if (g_iv.error[0]) {
            iv_draw_text(g_iv.error,
                         ix + iw * 0.5f - strlen(g_iv.error) * g_font_size * 0.3f,
                         iy + ih * 0.5f,
                         1.f, 0.4f, 0.4f, 1.f);
        } else {
            const char *hint = "Select an image file to preview";
            iv_draw_text(hint,
                         ix + iw * 0.5f - strlen(hint) * g_font_size * 0.3f,
                         iy + ih * 0.5f,
                         0.35f, 0.40f, 0.50f, 1.f);
        }
    }

    // ── Status bar ─────────────────────────────────────────────────────────
    {
        float st_y = (float)(win_h - status_h);
        draw_rect(0, st_y, (float)win_w, 1, 0.20f, 0.20f, 0.30f, 1.f);
        draw_rect(0, st_y+1, (float)win_w, (float)status_h-1, 0.09f, 0.09f, 0.13f, 1.f);

        char status[512];
        if (g_iv.tex) {
            snprintf(status, sizeof(status), "  %s   %dx%d px",
                     g_iv.img_label, g_iv.tex_w, g_iv.tex_h);
        } else {
            int n = (int)g_iv.entries.size();
            int imgs = 0;
            for (auto &e : g_iv.entries) if (!e.is_dir) imgs++;
            snprintf(status, sizeof(status), "  %d items (%d images)", n, imgs);
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

    case SDLK_UP:
        if (g_iv.selected > 0) g_iv.selected--;
        return true;

    case SDLK_DOWN:
        if (g_iv.selected < n - 1) g_iv.selected++;
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

    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
        iv_enter_selected();
        return true;

    case SDLK_LEFT:
    case SDLK_RIGHT: {
        // Find the previous/next image file (non-directory), wrapping around.
        // Collect indices of all image entries.
        std::vector<int> img_indices;
        for (int i = 0; i < n; i++)
            if (!g_iv.entries[i].is_dir && !g_iv.entries[i].is_zip)
                img_indices.push_back(i);
        if (img_indices.empty()) return true;

        // Find where the current selection sits in that list
        int pos = -1;
        for (int i = 0; i < (int)img_indices.size(); i++)
            if (img_indices[i] == g_iv.selected) { pos = i; break; }

        // If current selection is a dir, find the nearest image instead
        if (pos < 0) pos = (sym == SDLK_RIGHT) ? 0 : (int)img_indices.size() - 1;
        else if (sym == SDLK_RIGHT)
            pos = (pos + 1) % (int)img_indices.size();
        else
            pos = (pos - 1 + (int)img_indices.size()) % (int)img_indices.size();

        g_iv.selected = img_indices[pos];
        iv_enter_selected();
        return true;
    }

    case SDLK_BACKSPACE:
        // Go up one directory
        if (n > 0) {
            // Simulate selecting ".." and entering
            for (int i = 0; i < n; i++) {
                if (strcmp(g_iv.entries[i].name, "..") == 0) {
                    g_iv.selected = i;
                    iv_enter_selected();
                    break;
                }
            }
        }
        return true;

    default:
        return true;  // swallow all keys while open
    }
}
