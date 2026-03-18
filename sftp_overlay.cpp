// sftp_overlay.cpp — SFTP file transfer overlay for gl_terminal
#ifdef USESSH

#include "sftp_overlay.h"
#include "ssh_session.h"
#include "gl_renderer.h"
#include "ft_font.h"

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>

#ifndef _WIN32
#  include <sys/stat.h>
#  include <dirent.h>
#  include <unistd.h>
#  include <sys/select.h>
#else
#  include <windows.h>
#  include <shlobj.h>
#endif

// Wait for the socket to become readable or writable, with a timeout.
// Returns >0 on activity, 0 on timeout, <0 on error.
static int waitsocket(int sock, LIBSSH2_SESSION *sess) {
    struct timeval tv;
    tv.tv_sec  = 10;
    tv.tv_usec = 0;
    fd_set fd, *rfd = nullptr, *wfd = nullptr;
    FD_ZERO(&fd);
    FD_SET(sock, &fd);
    int dir = libssh2_session_block_directions(sess);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)  rfd = &fd;
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) wfd = &fd;
    return select(sock + 1, rfd, wfd, nullptr, &tv);
}

// ============================================================================
// EXTERNAL STATE
// ============================================================================

static LIBSSH2_SFTP *s_sftp  = nullptr;
SftpOverlay          g_sftp;
extern int           g_font_size;

// Transfer runs on a background thread. Main loop polls these atomics.
static std::thread        s_transfer_thread;
static std::atomic<float> s_progress{0.f};
static std::atomic<bool>  s_transferring{false};
// Status written by worker, read by main — protected by done flag ordering.
// Worker writes status then sets s_transferring=false; main reads after.
static char               s_status_buf[512] = {};
static bool               s_transfer_ok     = false;

// ============================================================================
// LAYOUT HELPERS
// ============================================================================

static const int PAD = 10;
static int row_h() { return (int)(g_font_size * 1.8f); }

static float sftp_draw_text(const char *text, float x, float y,
                             float r, float g, float b, float a) {
    return draw_text(text, x, y, g_font_size, g_font_size, r, g, b, a, 0);
}

// ============================================================================
// PLATFORM PATHS
// ============================================================================

std::string sftp_local_home_dir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, buf);
    return buf[0] ? buf : "C:\\Users";
#else
    const char *h = getenv("HOME");
    return h ? h : "/home";
#endif
}

std::string sftp_local_download_dir() {
    std::string home = sftp_local_home_dir();
#ifdef _WIN32
    std::string dl = home + "\\Downloads\\FelixTerminal";
    CreateDirectoryA((home + "\\Downloads").c_str(), nullptr);
    CreateDirectoryA(dl.c_str(), nullptr);
#else
    std::string dl = home + "/Downloads/FelixTerminal";
    mkdir((home + "/Downloads").c_str(), 0755);
    mkdir(dl.c_str(), 0755);
#endif
    return dl;
}

// ============================================================================
// SFTP INIT / SHUTDOWN
// ============================================================================

bool sftp_init() {
    if (s_sftp) return true;
    LIBSSH2_SESSION *sess = ssh_get_session();
    if (!sess) return false;
    libssh2_session_set_blocking(sess, 1);
    s_sftp = libssh2_sftp_init(sess);
    libssh2_session_set_blocking(sess, 0);
    if (!s_sftp) {
        SDL_Log("[SFTP] libssh2_sftp_init failed: %d\n", libssh2_session_last_errno(sess));
        return false;
    }
    SDL_Log("[SFTP] subsystem initialized\n");
    return true;
}

void sftp_transfer_join() {
    if (s_transfer_thread.joinable()) s_transfer_thread.join();
}

void sftp_shutdown() {
    sftp_transfer_join();
    if (s_sftp) { libssh2_sftp_shutdown(s_sftp); s_sftp = nullptr; }
}

// ============================================================================
// DIRECTORY LISTING
// ============================================================================

static void list_remote(const char *path, std::vector<SftpPanel::Entry> &out) {
    out.clear();
    if (!s_sftp) return;
    LIBSSH2_SESSION *sess = ssh_get_session();
    libssh2_session_set_blocking(sess, 1);

    LIBSSH2_SFTP_HANDLE *dh = nullptr;
    for (int i = 0; i < 200 && !dh; i++) {
        dh = libssh2_sftp_opendir(s_sftp, path);
        if (!dh && libssh2_session_last_errno(sess) == LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);
        else if (!dh) break;
    }
    if (!dh) {
        SDL_Log("[SFTP] opendir '%s' failed: sftp=%lu\n", path, libssh2_sftp_last_error(s_sftp));
        libssh2_session_set_blocking(sess, 0);
        return;
    }
    if (strcmp(path, "/") != 0) {
        SftpPanel::Entry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
        out.push_back(up);
    }
    char name[512], longentry[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    while (libssh2_sftp_readdir_ex(dh, name, sizeof(name), longentry, sizeof(longentry), &attrs) > 0) {
        if (!strcmp(name,".") || !strcmp(name,"..")) continue;
        SftpPanel::Entry e{};
        strncpy(e.name, name, sizeof(e.name)-1);
        e.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        e.size   = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
        out.push_back(e);
    }
    libssh2_sftp_closedir(dh);
    libssh2_session_set_blocking(sess, 0);
    std::stable_sort(out.begin(), out.end(), [](const SftpPanel::Entry &a, const SftpPanel::Entry &b){
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return strcmp(a.name, b.name) < 0;
    });
}

static void list_local(const char *path, std::vector<SftpPanel::Entry> &out) {
    out.clear();
#ifndef _WIN32
    if (strcmp(path, "/") != 0) {
        SftpPanel::Entry up{}; strncpy(up.name,"..",sizeof(up.name)-1); up.is_dir=true; out.push_back(up);
    }
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        SftpPanel::Entry e{};
        strncpy(e.name, de->d_name, sizeof(e.name)-1);
        char full[4096]; snprintf(full,sizeof(full),"%s/%s",path,de->d_name);
        struct stat st{};
        if (stat(full,&st)==0) { e.is_dir=S_ISDIR(st.st_mode); e.size=(uint64_t)st.st_size; }
        out.push_back(e);
    }
    closedir(d);
#else
    if (strcmp(path,"/")!=0 && strcmp(path,"\\")!=0) {
        SftpPanel::Entry up{}; strncpy(up.name,"..",sizeof(up.name)-1); up.is_dir=true; out.push_back(up);
    }
    char pattern[4096]; snprintf(pattern,sizeof(pattern),"%s\\*",path);
    WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA(pattern,&fd);
    if (h==INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
        SftpPanel::Entry e{};
        strncpy(e.name,fd.cFileName,sizeof(e.name)-1);
        e.is_dir=(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        e.size=((uint64_t)fd.nFileSizeHigh<<32)|fd.nFileSizeLow;
        out.push_back(e);
    } while(FindNextFileA(h,&fd));
    FindClose(h);
#endif
    std::stable_sort(out.begin(), out.end(), [](const SftpPanel::Entry &a, const SftpPanel::Entry &b){
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return strcmp(a.name, b.name) < 0;
    });
}

// ============================================================================
// PANEL OPS
// ============================================================================

void sftp_panel_refresh(SftpPanel &p) {
    p.selected = 0; p.scroll_top = 0;
    if (p.is_remote) list_remote(p.path, p.entries);
    else             list_local (p.path, p.entries);
}

static void panel_navigate(SftpPanel &p, const char *name) {
    if (!strcmp(name, "..")) {
#ifndef _WIN32
        char *slash = strrchr(p.path, '/');
        if (slash && slash != p.path) *slash = '\0';
        else strcpy(p.path, "/");
#else
        char sep = p.is_remote ? '/' : '\\';
        char *s = strrchr(p.path, sep);
        if (s && s != p.path) *s = '\0';
#endif
    } else {
#ifndef _WIN32
        char sep = '/';
#else
        char sep = p.is_remote ? '/' : '\\';
#endif
        size_t len = strlen(p.path);
        if (len > 1 && p.path[len-1] != sep) strncat(p.path, &sep, 1);
        strncat(p.path, name, sizeof(p.path) - strlen(p.path) - 1);
    }
    sftp_panel_refresh(p);
}

void sftp_panel_enter(SftpPanel &p) {
    if (p.entries.empty()) return;
    const auto &e = p.entries[p.selected];
    if (!e.is_dir) return;
    panel_navigate(p, e.name);
}

// ============================================================================
// OVERLAY OPEN
// ============================================================================

void sftp_overlay_open(SftpOverlayMode mode, const char *remote_cwd, int /*win_w*/, int win_h) {
    if (!sftp_init()) { SDL_Log("[SFTP] sftp_init failed\n"); return; }

    g_sftp.mode          = mode;
    g_sftp.visible       = true;
    g_sftp.status[0]     = '\0';
    g_sftp.transfer_ok   = false;
    g_sftp.progress      = 0.0f;
    g_sftp.transferring  = false;

    int rh = row_h();
    g_sftp.visible_rows = (win_h - PAD*4 - rh*3) / rh;

    // Left panel is always local
    g_sftp.left.is_remote = false;
    strncpy(g_sftp.left.path, sftp_local_home_dir().c_str(), sizeof(g_sftp.left.path)-1);
    sftp_panel_refresh(g_sftp.left);

    // Right panel is always remote
    g_sftp.right.is_remote = true;
    strncpy(g_sftp.right.path, remote_cwd, sizeof(g_sftp.right.path)-1);
    sftp_panel_refresh(g_sftp.right);

    if (mode == SftpOverlayMode::UPLOAD)
        g_sftp.focused_panel = 0;   // start on local so user picks source
    else
        g_sftp.focused_panel = 1;   // start on remote so user picks file to download
}

// ============================================================================
// PROGRESS RENDER  (called mid-transfer to keep UI alive)
// ============================================================================

static void fmt_size(uint64_t sz, char *buf, int bufsz) {
    if      (sz >= 1024*1024*1024ULL) snprintf(buf,bufsz,"%.1f GB",sz/(1024.0*1024*1024));
    else if (sz >= 1024*1024ULL)      snprintf(buf,bufsz,"%.1f MB",sz/(1024.0*1024));
    else if (sz >= 1024ULL)           snprintf(buf,bufsz,"%.1f KB",sz/1024.0);
    else                              snprintf(buf,bufsz,"%llu B",(unsigned long long)sz);
}

// ============================================================================
// TRANSFER  (runs on background thread — no GL calls allowed)
// ============================================================================

static bool do_download(const char *remote_path, const char *filename,
                        const char *local_dir, char *status, int stsz) {
    char remote_full[4096], local_full[4096];
    snprintf(remote_full, sizeof(remote_full), "%s/%s", remote_path, filename);
    snprintf(local_full,  sizeof(local_full),  "%s/%s", local_dir,   filename);

    LIBSSH2_SESSION *sess = ssh_get_session();
    int              sock = ssh_get_socket();
    libssh2_session_set_blocking(sess, 0);

    // Stat for size
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    uint64_t file_size = 0;
    { int rc; while ((rc = libssh2_sftp_stat(s_sftp, remote_full, &attrs)) == LIBSSH2_ERROR_EAGAIN) waitsocket(sock, sess);
      if (rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) file_size = attrs.filesize; }

    // Open remote file
    LIBSSH2_SFTP_HANDLE *fh = nullptr;
    while (!fh) {
        fh = libssh2_sftp_open(s_sftp, remote_full, LIBSSH2_FXF_READ, 0);
        if (!fh) {
            if (libssh2_session_last_errno(sess) == LIBSSH2_ERROR_EAGAIN) { waitsocket(sock, sess); continue; }
            snprintf(status, stsz, "Error: cannot open remote '%s' (sftp %lu)", filename, libssh2_sftp_last_error(s_sftp));
            libssh2_session_set_blocking(sess, 0); return false;
        }
    }

    FILE *out = fopen(local_full, "wb");
    if (!out) {
        snprintf(status, stsz, "Error: cannot create local '%s': %s", local_full, strerror(errno));
        while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket(sock, sess);
        libssh2_session_set_blocking(sess, 0); return false;
    }

    char     buf[32768];
    ssize_t  n;
    uint64_t total = 0;
    bool     ok    = true;
    s_progress.store(0.f);

    for (;;) {
        n = libssh2_sftp_read(fh, buf, sizeof(buf));
        if (n == LIBSSH2_ERROR_EAGAIN) { waitsocket(sock, sess); continue; }
        if (n < 0) { snprintf(status, stsz, "Error reading '%s' (sftp %lu)", filename, libssh2_sftp_last_error(s_sftp)); ok = false; break; }
        if (n == 0) break;
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            snprintf(status, stsz, "Error writing local '%s': %s", local_full, strerror(errno)); ok = false; break;
        }
        total += (uint64_t)n;
        if (file_size > 0) s_progress.store((float)total / (float)file_size);
        char done_sz[32], total_sz[32];
        fmt_size(total, done_sz, sizeof(done_sz));
        fmt_size(file_size > 0 ? file_size : total, total_sz, sizeof(total_sz));
        snprintf(g_sftp.status, sizeof(g_sftp.status), "Downloading '%s'  %s / %s", filename, done_sz, total_sz);
    }

    fclose(out);
    while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket(sock, sess);
    libssh2_session_set_blocking(sess, 0);

    if (!ok) return false;
    char sz[32]; fmt_size(total, sz, sizeof(sz));
    snprintf(status, stsz, "Downloaded '%s'  →  %s  (%s)", filename, local_dir, sz);
    s_progress.store(1.f);
    return true;
}

static bool do_upload(const char *local_path, const char *filename,
                      const char *remote_dir, char *status, int stsz) {
    char local_full[4096], remote_full[4096];
    snprintf(local_full,  sizeof(local_full),  "%s/%s", local_path, filename);
    snprintf(remote_full, sizeof(remote_full), "%s/%s", remote_dir, filename);

    uint64_t file_size = 0;
#ifndef _WIN32
    struct stat st{};
    if (stat(local_full, &st) == 0) file_size = (uint64_t)st.st_size;
#else
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(local_full, GetFileExInfoStandard, &fa))
        file_size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
#endif

    FILE *in = fopen(local_full, "rb");
    if (!in) { snprintf(status, stsz, "Error: cannot open local '%s': %s", local_full, strerror(errno)); return false; }

    LIBSSH2_SESSION *sess = ssh_get_session();
    int              sock = ssh_get_socket();
    libssh2_session_set_blocking(sess, 0);

    // Open remote file
    LIBSSH2_SFTP_HANDLE *fh = nullptr;
    while (!fh) {
        fh = libssh2_sftp_open(s_sftp, remote_full,
            LIBSSH2_FXF_WRITE|LIBSSH2_FXF_CREAT|LIBSSH2_FXF_TRUNC,
            LIBSSH2_SFTP_S_IRUSR|LIBSSH2_SFTP_S_IWUSR|LIBSSH2_SFTP_S_IRGRP|LIBSSH2_SFTP_S_IROTH);
        if (!fh) {
            if (libssh2_session_last_errno(sess) == LIBSSH2_ERROR_EAGAIN) { waitsocket(sock, sess); continue; }
            snprintf(status, stsz, "Error: cannot create remote '%s' (sftp %lu)", remote_full, libssh2_sftp_last_error(s_sftp));
            fclose(in); libssh2_session_set_blocking(sess, 0); return false;
        }
    }

    // 32KB chunks — sweet spot for libssh2 SFTP window
    char     buf[32768];
    size_t   nread;
    uint64_t total = 0;
    bool     ok    = true;
    s_progress.store(0.f);

    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        size_t sent = 0;
        while (sent < nread) {
            ssize_t rc = libssh2_sftp_write(fh, buf + sent, nread - sent);
            if (rc == LIBSSH2_ERROR_EAGAIN) { waitsocket(sock, sess); continue; }
            if (rc < 0) {
                snprintf(status, stsz, "Error writing '%s' (sftp %lu)", filename, libssh2_sftp_last_error(s_sftp));
                ok = false; goto done;
            }
            sent  += (size_t)rc;
            total += (size_t)rc;
        }
        if (file_size > 0) s_progress.store((float)total / (float)file_size);
        char done_sz[32], total_sz[32];
        fmt_size(total, done_sz, sizeof(done_sz));
        fmt_size(file_size > 0 ? file_size : total, total_sz, sizeof(total_sz));
        snprintf(g_sftp.status, sizeof(g_sftp.status), "Uploading '%s'  %s / %s", filename, done_sz, total_sz);
    }
    if (!ok) goto done;
    if (ferror(in)) { snprintf(status, stsz, "Error reading local '%s'", local_full); ok = false; }

done:
    fclose(in);
    while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket(sock, sess);
    libssh2_session_set_blocking(sess, 0);

    if (ok) {
        char sz[32]; fmt_size(total, sz, sizeof(sz));
        snprintf(status, stsz, "Uploaded '%s'  →  %s  (%s)", filename, remote_dir, sz);
        s_progress.store(1.f);
    }
    return ok;
}

void sftp_overlay_transfer() {
    // Don't start a second transfer while one is running
    if (s_transferring.load()) return;

    // Join any previously finished thread
    if (s_transfer_thread.joinable()) s_transfer_thread.join();

    if (g_sftp.mode == SftpOverlayMode::DOWNLOAD) {
        auto &rp = g_sftp.right;
        auto &lp = g_sftp.left;
        if (rp.entries.empty() || rp.entries[rp.selected].is_dir) return;

        // Snapshot what we need — thread must not touch g_sftp panel state
        std::string remote_path = rp.path;
        std::string filename    = rp.entries[rp.selected].name;
        std::string local_dir   = lp.path;

        g_sftp.status[0]    = '\0';
        g_sftp.transfer_ok  = false;
        g_sftp.transferring = true;
        s_transferring.store(true);
        s_progress.store(0.f);
        snprintf(g_sftp.status, sizeof(g_sftp.status), "Downloading '%s'...", filename.c_str());

        s_transfer_thread = std::thread([remote_path, filename, local_dir]() {
            bool ok = do_download(remote_path.c_str(), filename.c_str(),
                                  local_dir.c_str(), s_status_buf, sizeof(s_status_buf));
            s_transfer_ok = ok;
            s_transferring.store(false);  // signals main thread that we're done
        });

    } else {
        auto &lp = g_sftp.left;
        auto &rp = g_sftp.right;
        if (lp.entries.empty() || lp.entries[lp.selected].is_dir) {
            snprintf(g_sftp.status, sizeof(g_sftp.status),
                     "Select a local file in the left panel first");
            g_sftp.transfer_ok = false;
            return;
        }

        std::string local_path  = lp.path;
        std::string filename    = lp.entries[lp.selected].name;
        std::string remote_dir  = rp.path;

        g_sftp.status[0]    = '\0';
        g_sftp.transfer_ok  = false;
        g_sftp.transferring = true;
        s_transferring.store(true);
        s_progress.store(0.f);
        snprintf(g_sftp.status, sizeof(g_sftp.status), "Uploading '%s'...", filename.c_str());

        s_transfer_thread = std::thread([local_path, filename, remote_dir]() {
            bool ok = do_upload(local_path.c_str(), filename.c_str(),
                                remote_dir.c_str(), s_status_buf, sizeof(s_status_buf));
            s_transfer_ok = ok;
            s_transferring.store(false);
        });
    }
}

// ============================================================================
// KEYBOARD
// ============================================================================

bool sftp_overlay_keydown(SDL_Keycode sym) {
    if (!g_sftp.visible) return false;
    if (g_sftp.transferring) return true;   // block input during transfer

    SftpPanel &fp = (g_sftp.focused_panel == 0) ? g_sftp.left : g_sftp.right;
    int n = (int)fp.entries.size();

    switch (sym) {
    case SDLK_ESCAPE:
        g_sftp.visible = false;
        g_sftp.mode    = SftpOverlayMode::NONE;
        return true;

    case SDLK_TAB:
        g_sftp.focused_panel = 1 - g_sftp.focused_panel;
        return true;

    case SDLK_UP:
        if (fp.selected > 0) fp.selected--;
        if (fp.selected < fp.scroll_top) fp.scroll_top = fp.selected;
        return true;

    case SDLK_DOWN:
        if (fp.selected < n-1) fp.selected++;
        if (fp.selected >= fp.scroll_top + g_sftp.visible_rows)
            fp.scroll_top = fp.selected - g_sftp.visible_rows + 1;
        return true;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (!fp.entries.empty() && fp.entries[fp.selected].is_dir) {
            sftp_panel_enter(fp);
        } else if (g_sftp.mode == SftpOverlayMode::UPLOAD && g_sftp.focused_panel == 0) {
            // Enter on local file: switch focus to remote so user confirms destination
            if (!fp.entries.empty() && !fp.entries[fp.selected].is_dir)
                g_sftp.focused_panel = 1;
        } else {
            sftp_overlay_transfer();
        }
        return true;

    case SDLK_SPACE:
        sftp_overlay_transfer();
        return true;

    case SDLK_BACKSPACE:
        for (int i = 0; i < n; i++) {
            if (!strcmp(fp.entries[i].name, "..")) {
                fp.selected = i;
                sftp_panel_enter(fp);
                break;
            }
        }
        return true;

    default: return false;
    }
}

// ============================================================================
// RENDER
// ============================================================================

static void draw_panel(const SftpPanel &p, float px, float py, float pw, float ph,
                       bool focused, const char *label, int visible_rows) {
    int rh = row_h();

    draw_rect(px, py, pw, ph, 0.09f, 0.09f, 0.12f, 1.f);

    float br = focused ? 0.35f : 0.22f;
    float bg = focused ? 0.55f : 0.22f;
    float bb = focused ? 0.95f : 0.35f;
    draw_rect(px,      py,      pw, 1,  br, bg, bb, 1.f);
    draw_rect(px,      py+ph-1, pw, 1,  br, bg, bb, 1.f);
    draw_rect(px,      py,      1,  ph, br, bg, bb, 1.f);
    draw_rect(px+pw-1, py,      1,  ph, br, bg, bb, 1.f);

    // Header
    draw_rect(px, py, pw, (float)rh, 0.13f, 0.13f, 0.20f, 1.f);
    draw_rect(px, py+rh-1, pw, 1, br, bg, bb, 0.5f);
    float hcr = focused ? 0.7f : 0.55f;
    float hcg = focused ? 0.9f : 0.75f;
    float hcb = focused ? 1.0f : 0.85f;
    sftp_draw_text(label, px + PAD, py + rh * 0.72f, hcr, hcg, hcb, 1.f);

    // Path
    float path_y = py + rh;
    draw_rect(px, path_y, pw, (float)rh, 0.08f, 0.10f, 0.10f, 1.f);
    draw_rect(px, path_y+rh-1, pw, 1, 0.2f, 0.2f, 0.3f, 1.f);
    char path_disp[4096+4]; snprintf(path_disp, sizeof(path_disp), " %s", p.path);
    sftp_draw_text(path_disp, px + PAD, path_y + rh * 0.72f, 0.45f, 0.80f, 0.45f, 1.f);

    // File list
    float list_y = path_y + rh;
    int   n      = (int)p.entries.size();
    for (int i = 0; i < visible_rows; i++) {
        int idx = p.scroll_top + i;
        if (idx >= n) break;
        const auto &e  = p.entries[idx];
        float        ry = list_y + i * rh;
        bool         sel = (idx == p.selected);

        if      (sel && focused) draw_rect(px+1, ry, pw-2, (float)rh, 0.18f, 0.38f, 0.75f, 0.90f);
        else if (sel)            draw_rect(px+1, ry, pw-2, (float)rh, 0.20f, 0.22f, 0.35f, 0.90f);
        else if (i%2==0)         draw_rect(px,   ry, pw,   (float)rh, 1.f,   1.f,   1.f,   0.02f);

        float nr = sel ? 1.0f : (e.is_dir ? 0.90f : 0.82f);
        float ng = sel ? 1.0f : (e.is_dir ? 0.90f : 0.82f);
        float nb = sel ? 1.0f : (e.is_dir ? 0.50f : 0.92f);

        char name_buf[520];
        snprintf(name_buf, sizeof(name_buf), "%s%s", e.is_dir ? "[DIR] " : "      ", e.name);
        sftp_draw_text(name_buf, px + PAD, ry + rh * 0.72f, nr, ng, nb, 1.f);

        if (!e.is_dir && e.size > 0) {
            char sz[32]; fmt_size(e.size, sz, sizeof(sz));
            sftp_draw_text(sz, px + pw - PAD - 72, ry + rh * 0.72f, 0.45f, 0.55f, 0.65f, 1.f);
        }
    }

    // Scrollbar
    if (n > visible_rows) {
        float lh = (float)(visible_rows * rh);
        float th = lh * visible_rows / n;
        float ty = list_y + (lh - th) * p.scroll_top / (float)(n - visible_rows);
        draw_rect(px+pw-5, list_y, 5, lh, 0.12f, 0.12f, 0.20f, 1.f);
        draw_rect(px+pw-5, ty,     5, th, 0.35f, 0.50f, 0.90f, 1.f);
    }
}

static void draw_progress_bar(float px, float py, float pw, float ph, float t) {
    // Track
    draw_rect(px, py, pw, ph, 0.12f, 0.12f, 0.18f, 1.f);
    draw_rect(px, py, pw, 1,  0.20f, 0.20f, 0.30f, 1.f);
    draw_rect(px, py+ph-1, pw, 1, 0.20f, 0.20f, 0.30f, 1.f);
    // Fill — green gradient feel: dark at left, bright at progress point
    float fill_w = pw * (t < 0.f ? 0.f : t > 1.f ? 1.f : t);
    if (fill_w > 0) {
        draw_rect(px, py+1, fill_w, ph-2, 0.15f, 0.55f, 0.25f, 1.f);
        float edge = 3.f;
        if (fill_w > edge)
            draw_rect(px + fill_w - edge, py+1, edge, ph-2, 0.30f, 0.90f, 0.45f, 1.f);
    }
    // Percentage text
    char pct[16]; snprintf(pct, sizeof(pct), "%d%%", (int)(t * 100));
    sftp_draw_text(pct, px + pw * 0.5f - 16, py + ph * 0.72f, 1.f, 1.f, 1.f, 1.f);
}

void sftp_overlay_render(int win_w, int win_h) {
    if (!g_sftp.visible) return;

    // Poll transfer thread completion
    if (g_sftp.transferring && !s_transferring.load()) {
        // Thread just finished — copy results to main state
        g_sftp.transferring = false;
        g_sftp.transfer_ok  = s_transfer_ok;
        g_sftp.progress     = s_progress.load();
        strncpy(g_sftp.status, s_status_buf, sizeof(g_sftp.status)-1);
        // Refresh the destination panel so the new file shows up
        if (g_sftp.transfer_ok) {
            if (g_sftp.mode == SftpOverlayMode::DOWNLOAD)
                sftp_panel_refresh(g_sftp.left);   // local panel got a new file
            else
                sftp_panel_refresh(g_sftp.right);  // remote panel got a new file
        }
        if (s_transfer_thread.joinable()) s_transfer_thread.join();
    }

    // Copy atomic progress into overlay struct for rendering
    if (g_sftp.transferring)
        g_sftp.progress = s_progress.load();

    int   rh       = row_h();
    float title_h  = (float)(rh + PAD);
    float status_h = (float)(rh * 2 + PAD);  // taller to fit progress bar
    float panels_y = title_h;
    float panels_h = win_h - title_h - status_h;
    g_sftp.visible_rows = ((int)panels_h - rh*2) / rh;

    // Background
    draw_rect(0, 0, (float)win_w, (float)win_h, 0.07f, 0.07f, 0.09f, 1.f);

    // Title
    draw_rect(0, 0, (float)win_w, title_h, 0.12f, 0.12f, 0.18f, 1.f);
    draw_rect(0, title_h-1, (float)win_w, 1, 0.25f, 0.35f, 0.60f, 1.f);
    const char *title = (g_sftp.mode == SftpOverlayMode::DOWNLOAD)
        ? "SFTP Download (F3)   Tab: switch panel   Up/Down: navigate   Enter/Space: download   Backspace: up   Esc: close"
        : "SFTP Upload (F2)     Tab: switch panel   Up/Down: navigate   Enter/Space: upload     Backspace: up   Esc: close";
    sftp_draw_text(title, (float)PAD, title_h * 0.72f, 0.75f, 0.88f, 1.0f, 1.f);

    // Two panels — left always local, right always remote
    float half    = win_w * 0.5f;
    float divider = 2.f;
    bool  ul      = (g_sftp.mode == SftpOverlayMode::UPLOAD);
    draw_panel(g_sftp.left,  0,    panels_y, half-divider, panels_h,
               g_sftp.focused_panel==0,
               ul ? "Local  (source)"      : "Local  (download destination)",
               g_sftp.visible_rows);
    draw_rect(half-divider*0.5f, panels_y, divider, panels_h, 0.20f, 0.20f, 0.30f, 1.f);
    draw_panel(g_sftp.right, half, panels_y, win_w-half,   panels_h,
               g_sftp.focused_panel==1,
               ul ? "Remote (destination)" : "Remote (source)",
               g_sftp.visible_rows);

    // Status / progress area
    float st_y = win_h - status_h;
    draw_rect(0, st_y, (float)win_w, 1, 0.20f, 0.20f, 0.30f, 1.f);
    draw_rect(0, st_y+1, (float)win_w, status_h-1, 0.09f, 0.09f, 0.13f, 1.f);

    float bar_pad  = PAD * 2.f;
    float bar_h    = (float)(rh - 4);
    float bar_y    = st_y + (float)PAD * 0.5f;
    float bar_x    = bar_pad;
    float bar_w    = win_w - bar_pad * 2;
    float text_y   = st_y + (float)rh + PAD * 0.5f;

    if (g_sftp.transferring || g_sftp.progress > 0.f) {
        draw_progress_bar(bar_x, bar_y, bar_w, bar_h, g_sftp.progress);
    }

    if (g_sftp.status[0]) {
        float sr = g_sftp.transfer_ok ? 0.30f : (g_sftp.transferring ? 0.75f : 1.0f);
        float sg = g_sftp.transfer_ok ? 1.00f : (g_sftp.transferring ? 0.85f : 0.40f);
        float sb = g_sftp.transfer_ok ? 0.30f : (g_sftp.transferring ? 0.75f : 0.40f);
        sftp_draw_text(g_sftp.status, (float)PAD, text_y, sr, sg, sb, 1.f);
    } else {
        // Idle hint
        auto &lp = g_sftp.left;
        auto &rp = g_sftp.right;
        char hint[1024];
        if (g_sftp.mode == SftpOverlayMode::UPLOAD) {
            bool have_src = !lp.entries.empty() && !lp.entries[lp.selected].is_dir;
            if (have_src)
                snprintf(hint, sizeof(hint), "Ready: upload '%s'  →  %s   (Space to confirm)",
                         lp.entries[lp.selected].name, rp.path);
            else
                snprintf(hint, sizeof(hint), "Left panel: pick local file.  Right panel: pick remote destination.  Space: transfer.");
        } else {
            bool have_src = !rp.entries.empty() && !rp.entries[rp.selected].is_dir;
            if (have_src)
                snprintf(hint, sizeof(hint), "Ready: download '%s'  →  %s   (Space to confirm)",
                         rp.entries[rp.selected].name, lp.path);
            else
                snprintf(hint, sizeof(hint), "Right panel: pick remote file.  Left panel: pick local destination.  Space: transfer.");
        }
        sftp_draw_text(hint, (float)PAD, text_y, 0.50f, 0.55f, 0.65f, 1.f);
    }

    gl_flush_verts();
}

#endif // USESSH
