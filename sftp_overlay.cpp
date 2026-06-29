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
#include <ctype.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

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

// Transfer job tracking
struct TransferJob {
    int id;
    std::thread thread;
    std::atomic<bool> active{true};
    std::atomic<float> progress{0.f};
    std::atomic<bool> ok{false};
    char status[512] = {};
};

static std::mutex s_jobs_mutex;
static std::vector<TransferJob*> s_active_jobs;
static int s_next_job_id = 0;

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
// JOB MANAGEMENT
// ============================================================================

static TransferJob* create_job() {
    auto job = new TransferJob();
    job->id = s_next_job_id++;
    {
        std::lock_guard<std::mutex> lock(s_jobs_mutex);
        s_active_jobs.push_back(job);
    }
    return job;
}

static void cleanup_finished_jobs() {
    std::lock_guard<std::mutex> lock(s_jobs_mutex);
    auto it = s_active_jobs.begin();
    while (it != s_active_jobs.end()) {
        TransferJob *job = *it;
        if (!job->active.load()) {
            if (job->thread.joinable()) job->thread.join();
            delete job;
            it = s_active_jobs.erase(it);
        } else {
            ++it;
        }
    }
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
    std::lock_guard<std::mutex> lock(s_jobs_mutex);
    for (auto job : s_active_jobs) {
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
}

float sftp_progress() {
    std::lock_guard<std::mutex> lock(s_jobs_mutex);
    if (s_active_jobs.empty()) return 0.f;
    float total = 0.f;
    for (auto job : s_active_jobs) {
        if (job->active.load()) {
            total += job->progress.load();
        }
    }
    return total / (float)s_active_jobs.size();
}

void sftp_shutdown() {
    sftp_transfer_join();
    cleanup_finished_jobs();
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
// TRANSFER  (runs on background thread — each thread has its own SFTP channel)
// ============================================================================

static bool do_download(TransferJob *job, const char *remote_path, const char *filename,
                        const char *local_dir) {
    char remote_full[4096], local_full[4096];
    snprintf(remote_full, sizeof(remote_full), "%s/%s", remote_path, filename);
    snprintf(local_full,  sizeof(local_full),  "%s/%s", local_dir,   filename);

    LIBSSH2_SESSION *sess = ssh_get_session();
    int              sock = ssh_get_socket();

    // Lock ONLY for SFTP subsystem init, then release immediately
    ssh_session_lock();
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
            snprintf(job->status, sizeof(job->status), "Error: cannot open remote '%s' (sftp %lu)", filename, libssh2_sftp_last_error(s_sftp));
            ssh_session_unlock();
            return false;
        }
    }
    ssh_session_unlock();

    FILE *out = fopen(local_full, "wb");
    if (!out) {
        snprintf(job->status, sizeof(job->status), "Error: cannot create local '%s': %s", local_full, strerror(errno));
        ssh_session_lock();
        while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket(sock, sess);
        ssh_session_unlock();
        return false;
    }

    char     buf[32768];
    ssize_t  n;
    uint64_t total = 0;
    bool     ok    = true;
    job->progress.store(0.f);

    // Now do the bulk transfer without holding the lock
    for (;;) {
        n = libssh2_sftp_read(fh, buf, sizeof(buf));
        if (n == LIBSSH2_ERROR_EAGAIN) { waitsocket(sock, sess); continue; }
        if (n < 0) { snprintf(job->status, sizeof(job->status), "Error reading '%s' (sftp %lu)", filename, libssh2_sftp_last_error(s_sftp)); ok = false; break; }
        if (n == 0) break;
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            snprintf(job->status, sizeof(job->status), "Error writing local '%s': %s", local_full, strerror(errno)); ok = false; break;
        }
        total += (uint64_t)n;
        if (file_size > 0) job->progress.store((float)total / (float)file_size);
        char done_sz[32], total_sz[32];
        fmt_size(total, done_sz, sizeof(done_sz));
        fmt_size(file_size > 0 ? file_size : total, total_sz, sizeof(total_sz));
        snprintf(job->status, sizeof(job->status), "Downloading '%s'  %s / %s", filename, done_sz, total_sz);
    }

    fclose(out);
    ssh_session_lock();
    while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket(sock, sess);
    ssh_session_unlock();

    if (!ok) return false;
    char sz[32]; fmt_size(total, sz, sizeof(sz));
    snprintf(job->status, sizeof(job->status), "Downloaded '%s'  →  %s  (%s)", filename, local_dir, sz);
    job->progress.store(1.f);
    return true;
}

static bool do_upload(TransferJob *job, const char *local_path, const char *filename,
                      const char *remote_dir) {
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
    if (!in) { snprintf(job->status, sizeof(job->status), "Error: cannot open local '%s': %s", local_full, strerror(errno)); return false; }

    LIBSSH2_SESSION *sess = ssh_get_session();
    int              sock = ssh_get_socket();
    
    // Lock only for file open
    ssh_session_lock();
    LIBSSH2_SFTP_HANDLE *fh = nullptr;
    while (!fh) {
        fh = libssh2_sftp_open(s_sftp, remote_full,
            LIBSSH2_FXF_WRITE|LIBSSH2_FXF_CREAT|LIBSSH2_FXF_TRUNC,
            LIBSSH2_SFTP_S_IRUSR|LIBSSH2_SFTP_S_IWUSR|LIBSSH2_SFTP_S_IRGRP|LIBSSH2_SFTP_S_IROTH);
        if (!fh) {
            if (libssh2_session_last_errno(sess) == LIBSSH2_ERROR_EAGAIN) { waitsocket(sock, sess); continue; }
            snprintf(job->status, sizeof(job->status), "Error: cannot create remote '%s' (sftp %lu)", remote_full, libssh2_sftp_last_error(s_sftp));
            fclose(in);
            ssh_session_unlock();
            return false;
        }
    }
    ssh_session_unlock();

    // 32KB chunks — sweet spot for libssh2 SFTP window
    char     buf[32768];
    size_t   nread;
    uint64_t total = 0;
    bool     ok    = true;
    job->progress.store(0.f);

    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        size_t sent = 0;
        while (sent < nread) {
            ssize_t rc = libssh2_sftp_write(fh, buf + sent, nread - sent);
            if (rc == LIBSSH2_ERROR_EAGAIN) { waitsocket(sock, sess); continue; }
            if (rc < 0) {
                snprintf(job->status, sizeof(job->status), "Error writing '%s' (sftp %lu)", filename, libssh2_sftp_last_error(s_sftp));
                ok = false; goto done;
            }
            sent  += (size_t)rc;
            total += (size_t)rc;
        }
        if (file_size > 0) job->progress.store((float)total / (float)file_size);
        char done_sz[32], total_sz[32];
        fmt_size(total, done_sz, sizeof(done_sz));
        fmt_size(file_size > 0 ? file_size : total, total_sz, sizeof(total_sz));
        snprintf(job->status, sizeof(job->status), "Uploading '%s'  %s / %s", filename, done_sz, total_sz);
    }
    if (!ok) goto done;
    if (ferror(in)) { snprintf(job->status, sizeof(job->status), "Error reading local '%s'", local_full); ok = false; }

done:
    fclose(in);
    ssh_session_lock();
    while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket(sock, sess);
    ssh_session_unlock();

    if (ok) {
        char sz[32]; fmt_size(total, sz, sizeof(sz));
        snprintf(job->status, sizeof(job->status), "Uploaded '%s'  →  %s  (%s)", filename, remote_dir, sz);
        job->progress.store(1.f);
    }
    return ok;
}

void sftp_overlay_transfer() {
    if (g_sftp.mode == SftpOverlayMode::DOWNLOAD) {
        auto &rp = g_sftp.right;
        auto &lp = g_sftp.left;
        if (rp.entries.empty() || rp.entries[rp.selected].is_dir) return;

        // Snapshot what we need
        std::string remote_path = rp.path;
        std::string filename    = rp.entries[rp.selected].name;
        std::string local_dir   = lp.path;

        g_sftp.status[0]    = '\0';
        g_sftp.transfer_ok  = false;
        g_sftp.transferring = true;
        snprintf(g_sftp.status, sizeof(g_sftp.status), "Downloading '%s'...", filename.c_str());

        TransferJob *job = create_job();
        job->thread = std::thread([job, remote_path, filename, local_dir]() {
            bool ok = do_download(job, remote_path.c_str(), filename.c_str(), local_dir.c_str());
            job->ok.store(ok);
            job->active.store(false);
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
        snprintf(g_sftp.status, sizeof(g_sftp.status), "Uploading '%s'...", filename.c_str());

        TransferJob *job = create_job();
        job->thread = std::thread([job, local_path, filename, remote_dir]() {
            bool ok = do_upload(job, local_path.c_str(), filename.c_str(), remote_dir.c_str());
            job->ok.store(ok);
            job->active.store(false);
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

    default: {
        // First-letter jump: press a letter/digit to jump to the first entry
        // starting with that character. Press again to cycle through matches.
        if (sym < 32 || sym > 126) return false;
        char ch = (char)tolower((unsigned char)sym);
        if (!isalpha((unsigned char)ch) && !isdigit((unsigned char)ch)) return false;

        std::vector<int> matches;
        for (int i = 0; i < n; i++) {
            const char *nm = fp.entries[i].name;
            if (strcmp(nm, "..") == 0) continue;
            if (tolower((unsigned char)nm[0]) == (unsigned char)ch)
                matches.push_back(i);
        }
        if (matches.empty()) return false;

        int next = matches[0];
        for (size_t i = 0; i < matches.size(); i++) {
            if (matches[i] == fp.selected) {
                next = matches[(i + 1) % matches.size()];
                break;
            }
        }
        fp.selected = next;
        if (fp.selected < fp.scroll_top) fp.scroll_top = fp.selected;
        if (fp.selected >= fp.scroll_top + g_sftp.visible_rows)
            fp.scroll_top = fp.selected - g_sftp.visible_rows + 1;
        return true;
    }
    }

    return false;
}

// ============================================================================
// RENDERING
// ============================================================================

static void draw_progress_bar(float px, float py, float pw, float ph, float t) {
    if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f;
    // Background
    draw_rect(px, py, pw, ph, 0.15f, 0.15f, 0.20f, 1.f);
    if (t > 0.f) {
        float fill_w = t * pw;
        draw_rect(px, py+1, fill_w, ph-2, 0.15f, 0.55f, 0.25f, 1.f);
        float edge = 3.f;
        if (fill_w > edge)
            draw_rect(px + fill_w - edge, py+1, edge, ph-2, 0.30f, 0.90f, 0.45f, 1.f);
    }
    // Percentage text
    char pct[16]; snprintf(pct, sizeof(pct), "%d%%", (int)(t * 100));
    sftp_draw_text(pct, px + pw * 0.5f - 16, py + ph * 0.72f, 1.f, 1.f, 1.f, 1.f);
}

static void draw_panel(const SftpPanel &p, float px, float py, float pw, float ph,
                       bool focus, const char *label, int visible_rows) {
    // Border highlight
    float border_h = 2.f;
    draw_rect(px, py, pw, border_h, focus ? 0.4f : 0.2f, focus ? 0.6f : 0.3f, 1.f, 1.f);

    // Path bar
    int   rh      = row_h();
    float path_y  = py + (float)rh;
    draw_rect(px, path_y-border_h, pw, (float)rh, 0.10f, 0.10f, 0.15f, 1.f);
    sftp_draw_text(p.path, px + (float)PAD, path_y + 2, 0.5f, 0.8f, 1.0f, 1.f);

    // Files list
    float list_y = path_y + (float)rh;
    int   n      = (int)p.entries.size();
    for (int i = 0; i < visible_rows && p.scroll_top + i < n; i++) {
        float row_y     = list_y + (float)(i * rh);
        const auto &e   = p.entries[p.scroll_top + i];
        bool  sel       = (p.scroll_top + i == p.selected);
        float bg_r      = sel ? 0.25f : 0.09f;
        float bg_g      = sel ? 0.40f : 0.09f;
        float bg_b      = sel ? 0.65f : 0.11f;
        draw_rect(px, row_y, pw, (float)rh, bg_r, bg_g, bg_b, 1.f);

        char label[600];
        if (e.is_dir)
            snprintf(label, sizeof(label), "%-40s <DIR>", e.name);
        else {
            char sz[32]; fmt_size(e.size, sz, sizeof(sz));
            snprintf(label, sizeof(label), "%-40s %s", e.name, sz);
        }
        sftp_draw_text(label, px + (float)PAD, row_y + 2, 0.9f, 0.9f, 0.9f, 1.f);
    }
}

void sftp_overlay_render(int win_w, int win_h) {
    if (!g_sftp.visible) return;

    // Poll jobs and update UI state
    cleanup_finished_jobs();
    {
        std::lock_guard<std::mutex> lock(s_jobs_mutex);
        if (!s_active_jobs.empty()) {
            TransferJob *job = s_active_jobs.back();
            g_sftp.progress = job->progress.load();
            strncpy(g_sftp.status, job->status, sizeof(g_sftp.status)-1);
            
            if (!job->active.load()) {
                // Job finished
                g_sftp.transferring = false;
                g_sftp.transfer_ok  = job->ok.load();
                // Refresh the destination panel so the new file shows up
                if (g_sftp.transfer_ok) {
                    if (g_sftp.mode == SftpOverlayMode::DOWNLOAD)
                        sftp_panel_refresh(g_sftp.left);   // local panel got a new file
                    else
                        sftp_panel_refresh(g_sftp.right);  // remote panel got a new file
                }
            }
        } else {
            g_sftp.transferring = false;
        }
    }

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
        ? "SFTP Download (F3)   Tab: switch panel   Up/Down: navigate   Space: download   Backspace: up   Esc: close"
        : "SFTP Upload (F2)     Tab: switch panel   Up/Down: navigate   Space: upload     Backspace: up   Esc: close";
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

// ============================================================================
// MOUSE
// ============================================================================

// Returns which panel (0=left, 1=right) the pixel x falls in, and computes
// the layout constants needed by both mouse handlers.
static int sftp_hit_panel(int x, int win_w, float &panels_y, int &rh_out, int &list_top_row) {
    rh_out     = row_h();
    panels_y   = (float)(rh_out + PAD);   // title_h
    list_top_row = 2;                       // header row + path row above the list
    float half = win_w * 0.5f;
    return (x < (int)half) ? 0 : 1;
}

bool sftp_overlay_mousewheel(int x, int /*y*/, int delta, int win_w) {
    if (!g_sftp.visible || g_sftp.transferring) return false;

    float panels_y; int rh, list_top_row;
    int side = sftp_hit_panel(x, win_w, panels_y, rh, list_top_row);

    SftpPanel &p = (side == 0) ? g_sftp.left : g_sftp.right;
    // Focus the panel the mouse is over
    g_sftp.focused_panel = side;

    int n    = (int)p.entries.size();
    int step = (delta > 0) ? -3 : 3;   // wheel up → scroll toward top
    p.scroll_top = std::max(0, std::min(p.scroll_top + step,
                                        std::max(0, n - g_sftp.visible_rows)));
    // Keep selected within the visible window
    if (p.selected < p.scroll_top)
        p.selected = p.scroll_top;
    if (p.selected >= p.scroll_top + g_sftp.visible_rows)
        p.selected = p.scroll_top + g_sftp.visible_rows - 1;
    p.selected = std::max(0, std::min(p.selected, n - 1));
    return true;
}

bool sftp_overlay_mousedown(int x, int y, int button, int win_w, int win_h) {
    if (!g_sftp.visible || g_sftp.transferring) return false;
    if (button != SDL_BUTTON_LEFT) return false;

    float panels_y; int rh, list_top_row;
    int side = sftp_hit_panel(x, win_w, panels_y, rh, list_top_row);

    // Switch focus to whichever panel was clicked
    g_sftp.focused_panel = side;
    SftpPanel &p = (side == 0) ? g_sftp.left : g_sftp.right;

    // Compute click row inside the file list
    // Layout: panels_y + rh (header) + rh (path) = start of file list
    float list_y = panels_y + rh * list_top_row;
    int   row    = (int)((y - list_y) / rh);
    int   idx    = p.scroll_top + row;
    int   n      = (int)p.entries.size();

    if (row < 0 || idx >= n) return true;  // clicked header/path row — just switch focus

    static uint32_t s_last_time[2] = {};
    static int      s_last_idx[2]  = { -1, -1 };
    uint32_t now = SDL_GetTicks();
    bool dbl = (idx == s_last_idx[side] && (now - s_last_time[side]) < 400);
    s_last_time[side] = now;
    s_last_idx[side]  = idx;

    p.selected = idx;
    // Keep selection visible (shouldn't need adjustment but be safe)
    if (p.selected < p.scroll_top) p.scroll_top = p.selected;
    if (p.selected >= p.scroll_top + g_sftp.visible_rows)
        p.scroll_top = p.selected - g_sftp.visible_rows + 1;

    if (dbl) {
        if (p.entries[idx].is_dir)
            sftp_panel_enter(p);
        else
            sftp_overlay_transfer();
    }

    (void)win_h;
    return true;
}


void sftp_reset_after_fork() {
    // Clean up inherited SFTP state from parent process.
    sftp_transfer_join();
    cleanup_finished_jobs();
    s_sftp = nullptr;
}

#endif // USESSH
