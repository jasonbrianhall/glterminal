#pragma once
// Minimal POSIX compatibility shims for MSVC builds only. mingw, gcc, and
// clang already ship real dirent.h/unistd.h/pid_t, so this whole file is a
// no-op for them -- it only kicks in for _MSC_VER (and not __MINGW32__,
// which also defines _MSC_VER-like macros in some configs but has its own
// headers).
#if defined(_MSC_VER) && !defined(__MINGW32__)

#include <io.h>
#include <sys/types.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifndef NOMINMAX
#define NOMINMAX  // otherwise windows.h's min/max macros mangle std::min/std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

/* ---- POSIX case-insensitive string compare ---------------------------- */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

/* ---- struct stat / stat() ------------------------------------------------
 * MSVC's <sys/stat.h> only provides the underscore-prefixed struct _stat
 * and _stat() -- no non-prefixed alias exists by default. One macro covers
 * both the type name (`struct stat`) and the function call (`stat(...)`).
 */
#ifndef stat
#define stat _stat
#endif

/* ---- <sys/stat.h> S_ISDIR ----------------------------------------------
 * MSVC's sys/stat.h always defines the underscore-prefixed _S_IFMT/_S_IFDIR;
 * the non-prefixed POSIX aliases (S_IFMT/S_IFDIR) are only exposed under
 * some CRT configs, so define them explicitly rather than assume they're
 * already there.
 */
#ifndef S_IFMT
#define S_IFMT _S_IFMT
#endif
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

/* ---- mkdir/rmdir --------------------------------------------------------
 * MSVC's <direct.h> only has the single-argument _mkdir (no POSIX mode
 * bits on Windows). Wrap it under the POSIX name so callers can use either
 * mkdir(name) (as guarded by #ifdef _WIN32 at call sites) or, if some call
 * site isn't guarded, mkdir(name, mode) with the mode silently ignored.
 */
#ifndef mkdir
static __inline int mkdir(const char *path) { return _mkdir(path); }
#endif
#ifndef rmdir
#define rmdir _rmdir
#endif

/* ---- clock_gettime(CLOCK_REALTIME, ...) --------------------------------
 * MSVC's <time.h> already defines struct timespec (since VS2015 UCRT), just
 * not clock_gettime() or CLOCK_REALTIME itself.
 */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
static __inline int clock_gettime(int clk_id, struct timespec *spec) {
    (void)clk_id;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;  // Windows epoch (1601) -> Unix epoch (1970)
    spec->tv_sec  = (long)(t / 10000000ULL);
    spec->tv_nsec = (long)((t % 10000000ULL) * 100);
    return 0;
}
#endif

/* ---- <pid_t> --------------------------------------------------------- */
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

/* ---- <unistd.h> subset ------------------------------------------------
 * Only the bits this project actually reaches for; extend as needed.
 */
#define access _access
#define chmod  _chmod
#define unlink _unlink
#define getcwd _getcwd
#define chdir  _chdir
#define write  _write
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef F_OK
#define F_OK 0
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

static __inline unsigned int sleep(unsigned int seconds) {
    Sleep(seconds * 1000);
    return 0;
}
static __inline int usleep(unsigned int usec) {
    Sleep(usec < 1000 ? 1 : usec / 1000);
    return 0;
}
static __inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    Sleep(ms);
    return 0;
}

/* ---- <dirent.h> subset -------------------------------------------------
 * Just enough opendir/readdir/closedir to walk a directory, backed by
 * FindFirstFileA/FindNextFileA.
 */
struct dirent {
    char d_name[MAX_PATH];
};

typedef struct DIR {
    HANDLE handle;
    WIN32_FIND_DATAA find_data;
    int first;
    struct dirent entry;
} DIR;

static __inline DIR *opendir(const char *path) {
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", path);

    DIR *d = (DIR*)malloc(sizeof(DIR));
    if (!d) return NULL;
    d->handle = FindFirstFileA(pattern, &d->find_data);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static __inline struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (!d->first) {
        if (!FindNextFileA(d->handle, &d->find_data)) return NULL;
    } else {
        d->first = 0;
    }
    strncpy(d->entry.d_name, d->find_data.cFileName, MAX_PATH - 1);
    d->entry.d_name[MAX_PATH - 1] = '\0';
    return &d->entry;
}

static __inline int closedir(DIR *d) {
    if (!d) return -1;
    FindClose(d->handle);
    free(d);
    return 0;
}

#endif /* _MSC_VER && !__MINGW32__ */
