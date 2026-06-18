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

// Truncate a filename so it fits within max_chars, preserving the extension.
// Result is written into out (must be at least max_chars+1 bytes).
// Uses a UTF-8-safe approach by working in bytes (filenames are usually ASCII).
void iv_truncate_name(const char *name, int max_chars, char *out, int out_sz) {
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

std::string iv_home() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, buf);
    return buf[0] ? buf : "C:\\Users";
#else
    const char *h = getenv("HOME");
    return h ? h : "/home";
#endif
}

