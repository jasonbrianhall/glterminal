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

#include <string>
#include <cstring>


// ============================================================================
// CROSS-PLATFORM TEMP FILE
// ============================================================================

// Write data to a new temp file with the given extension. Returns the path on
// success or "" on failure. Caller must delete the file when done.
std::string iv_write_tempfile(const unsigned char *data, size_t len, const char *ext) {
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

void iv_delete_tempfile(const char *path) {
    if (!path || !path[0]) return;
#ifndef _WIN32
    unlink(path);
#else
    DeleteFileA(path);
#endif
}
