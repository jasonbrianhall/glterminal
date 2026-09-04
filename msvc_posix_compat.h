#pragma once
// Minimal POSIX compatibility shims for MSVC builds only. mingw, gcc, and
// clang already ship real dirent.h/unistd.h/pid_t, so this whole file is a
// no-op for them -- it only kicks in for _MSC_VER (and not __MINGW32__,
// which also defines _MSC_VER-like macros in some configs but has its own
// headers).
#if defined(_MSC_VER) && !defined(__MINGW32__)

#include <io.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

/* ---- POSIX case-insensitive string compare ---------------------------- */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

/* ---- <pid_t> --------------------------------------------------------- */
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

/* ---- <unistd.h> subset ------------------------------------------------
 * Only the bits this project actually reaches for; extend as needed.
 */
#define access _access
#define unlink _unlink
#define getcwd _getcwd
#define chdir  _chdir
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
