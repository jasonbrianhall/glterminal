/* supp.c -- support routines for dungeon */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef unix
#include <sys/types.h>
#endif

#ifdef BSD4_2
#include <sys/time.h>
#else
#include <time.h>
#endif

#include "../zork/funcs.h"

extern void exit(int);
extern int  rand(void);
extern time_t time(time_t *);
extern struct tm *localtime();

/* ============================================================================
 * ZORK I/O SHIM
 *
 * g_zork_out      ring buffer — dungeon writes here via more_output();
 *                 wopr_zork.cpp drains it each frame.
 * g_zork_in       line buffer — wopr_zork.cpp calls zork_shim_set_input()
 *                 to post a newline-terminated command; dungeon reads it
 *                 via zork_shim_fgets() (called from gdt.c in place of
 *                 fgets(stdin)).
 * g_zork_game_over set to 1 by exit_() so wopr_zork.cpp can return to menu.
 * ========================================================================== */

#define ZORK_OUT_SIZE 65536
#define ZORK_IN_SIZE  512

char g_zork_out[ZORK_OUT_SIZE];
int  g_zork_out_write = 0;
int  g_zork_out_read  = 0;

char g_zork_in[ZORK_IN_SIZE];
int  g_zork_in_ready  = 0;

int  g_zork_game_over = 0;

static void zork_out_push(const char *s)
{
    while (*s) {
        int next = (g_zork_out_write + 1) % ZORK_OUT_SIZE;
        if (next == g_zork_out_read) return; /* full — drop */
        g_zork_out[g_zork_out_write] = *s++;
        g_zork_out_write = next;
    }
}

void zork_shim_set_input(const char *line)
{
    strncpy(g_zork_in, line, ZORK_IN_SIZE - 1);
    g_zork_in[ZORK_IN_SIZE - 1] = '\0';
    g_zork_in_ready = 1;
}

/* Drop-in for fgets(buf, n, stdin) used in gdt.c */
char *zork_shim_fgets(char *buf, int n)
{
    if (!g_zork_in_ready) {
        if (n > 0) buf[0] = '\0';
        return buf;
    }
    strncpy(buf, g_zork_in, n - 1);
    buf[n - 1] = '\0';
    g_zork_in_ready = 0;
    return buf;
}

/* ============================================================================
 * more_output — signature UNCHANGED (const char *fmt, ...)
 * Body routes through the shim ring buffer instead of stdout.
 * ========================================================================== */

static int crows   = 24;
static int coutput = 0;

void more_init(void)
{
    crows = 24;
}

void more_output(const char *fmt, ...)
{
    char buf[4096];

    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        zork_out_push(buf);
    }

    coutput++;
}

void more_input(void)
{
    coutput = 0;
}

/* ============================================================================
 * Game lifecycle
 * exit_() sets a flag instead of calling exit() so the WOPR overlay can
 * return to the game menu cleanly rather than killing the process.
 * ========================================================================== */

void exit_(void)
{
    more_output("The game is over.\n");
    g_zork_game_over = 1;
}

void itime_(int *hrptr, int *minptr, int *secptr)
{
    time_t    timebuf;
    struct tm *tmptr;
    time(&timebuf);
    tmptr   = localtime(&timebuf);
    *hrptr  = tmptr->tm_hour;
    *minptr = tmptr->tm_min;
    *secptr = tmptr->tm_sec;
}

int rnd_(int maxval)
{
    return rand() % maxval;
}
