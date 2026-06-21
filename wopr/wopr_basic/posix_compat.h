#ifndef POSIX_COMPAT_H
#define POSIX_COMPAT_H

/* POSIX compatibility shim for DOS/DJGPP builds
 * Provides implementations of POSIX functions that might not be available
 */

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <signal.h>       /* Signal handling - always available */
#include <time.h>         /* time functions */
#include <stdint.h>       /* uint32_t and other integer types */
#include <unistd.h>       /* write, STDOUT_FILENO */

/* Try to include standard POSIX headers */
#ifdef __DJGPP__
#define _POSIX_SOURCE
#define _GNU_SOURCE
#endif

/* Include headers that might not be available */
#ifdef __cplusplus
extern "C" {
#endif

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __cplusplus
}
#endif

/* ========== Time structures and functions ========== */
#ifndef HAVE_CLOCK_GETTIME

/* Define timespec if not available */
#ifndef timespec
struct timespec {
    time_t tv_sec;      /* seconds */
    long   tv_nsec;     /* nanoseconds */
};
#endif

/* Define CLOCK_REALTIME if not available */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

/* Fallback clock_gettime using time() */
static inline int clock_gettime(int clock_id, struct timespec *ts) {
    (void)clock_id;  /* Ignore clock type on DOS */
    if (!ts) return -1;
    time_t now = time(NULL);
    ts->tv_sec = now;
    ts->tv_nsec = 0;
    return 0;
}
#endif

/* Fallback nanosleep - simplified for DOS */
#ifndef HAVE_NANOSLEEP
static inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) return -1;
    /* On DOS, just sleep whole seconds - nanosecond precision not available */
    if (req->tv_sec > 0) {
        #ifdef _WIN32
            Sleep(req->tv_sec * 1000);
        #else
            /* DOS/DJGPP: minimal sleep - just busy-wait is often used */
            volatile long i, j;
            for (i = 0; i < req->tv_sec * 10000000L; i++) {
                j = i;  /* Prevent optimization */
            }
        #endif
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}
#endif

/* Define STDOUT_FILENO if not available */
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

/* ========== Case-insensitive string comparison ========== */
#ifndef HAVE_STRNCASECMP
static inline int strncasecmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n && s1[i] && s2[i]; i++) {
        int c1 = tolower((unsigned char)s1[i]);
        int c2 = tolower((unsigned char)s2[i]);
        if (c1 != c2) return c1 - c2;
    }
    return 0;
}
#endif

#ifndef HAVE_STRCASECMP
static inline int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1++);
        int c2 = tolower((unsigned char)*s2++);
        if (c1 != c2) return c1 - c2;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}
#endif

#ifndef HAVE_STRCASESTR
static inline char *strcasestr(const char *haystack, const char *needle) {
    if (!needle || !*needle) return (char *)haystack;
    
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h = haystack, *n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                h++;
                n++;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}
#endif

/* ========== Directory operations fallback ========== */
/* DJGPP should have these, but provide stubs if needed */
#ifdef MSDOS_BUILD
/* Stub implementations - these should be in DJGPP's libc */
/* If compilation fails, these will need real implementations */
#endif

/* ========== Signal handling ========== */
/* DJGPP's sigaction might have issues; provide minimal wrapper if needed */
#ifdef MSDOS_BUILD

#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif

/* If sigaction isn't working, we might need to wrap it */
#if defined(__GNUC__) && !defined(HAVE_SIGACTION)
typedef struct {
    void (*sa_handler)(int);
    /* Simplified - DJGPP's sigaction should provide the rest */
} sigaction_compat;
#endif

#endif /* MSDOS_BUILD */

#endif /* POSIX_COMPAT_H */
