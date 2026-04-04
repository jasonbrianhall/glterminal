/* supp.c -- support routines for dungeon */
/* Modified for WOPR overlay */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <SDL2/SDL.h>

#ifdef unix
#include <sys/types.h>
#endif

#ifdef BSD4_2
#include <sys/time.h>
#else
#include <time.h>
#endif

#include "funcs.h"

extern void exit P((int));
extern int  rand P((void));
extern time_t time P((time_t *));

/* ============================================================================
 * Shared I/O surfaces
 * ========================================================================== */

void wopr_zork_push_line(const char *line);
void wopr_zork_signal_done(void);

char          zork_input_buf[512];
int           zork_input_ready  = 0;
int           g_zork_game_over  = 0;
SDL_sem      *zork_input_sem    = NULL;

/* longjmp target set in zork_thread_fn before calling zork_main() */
jmp_buf       zork_exit_jmp;

void zork_shim_init(void)
{
    if (zork_input_sem) {
        SDL_DestroySemaphore(zork_input_sem);
        zork_input_sem = NULL;
    }
    zork_input_sem = SDL_CreateSemaphore(0);
}

void zork_shim_set_input(const char *line)
{
    strncpy(zork_input_buf, line, sizeof(zork_input_buf) - 1);
    zork_input_buf[sizeof(zork_input_buf) - 1] = '\0';
    zork_input_ready = 1;
    if (zork_input_sem)
        SDL_SemPost(zork_input_sem);
}

char *zork_shim_fgets(char *buf, int n)
{
    if (g_zork_game_over) {
        if (n > 0) buf[0] = '\0';
        return buf;
    }
    if (zork_input_sem) {
        SDL_SemWait(zork_input_sem);
    } else {
        /* Semaphore not yet initialised — poll slowly rather than busy-spin */
        while (!zork_input_ready && !g_zork_game_over)
            SDL_Delay(10);
    }
    if (!zork_input_ready || g_zork_game_over) {
        if (n > 0) buf[0] = '\0';
        return buf;
    }
    strncpy(buf, zork_input_buf, n - 1);
    buf[n - 1] = '\0';
    zork_input_ready = 0;
    return buf;
}

/* ============================================================================
 * more_output
 * ========================================================================== */

static int  crows     = 24;
static int  coutput   = 0;
static char s_linebuf[4096];
static int  s_linelen = 0;

static void flush_linebuf(void)
{
    if (s_linelen > 0) {
        s_linebuf[s_linelen] = '\0';
        wopr_zork_push_line(s_linebuf);
        s_linelen = 0;
    }
}

void more_init(void)
{
    crows     = 24;
    s_linelen = 0;
}

void more_output(const char *fmt, ...)
{
    char buf[4096];
    int  i;

    if (fmt == NULL) {
        flush_linebuf();
        coutput++;
        return;
    }

    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }

    for (i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '\n') {
            flush_linebuf();
        } else {
            if (s_linelen < (int)sizeof(s_linebuf) - 1)
                s_linebuf[s_linelen++] = buf[i];
        }
    }

    coutput++;
}

void more_input(void)
{
    flush_linebuf();
    coutput = 0;
}

/* ============================================================================
 * Lifecycle
 * ========================================================================== */

void exit_(void)
{
    flush_linebuf();
    g_zork_game_over = 1;
    /* Unblock any fgets waiting for input */
    if (zork_input_sem)
        SDL_SemPost(zork_input_sem);
    /* Jump all the way back to zork_thread_fn, unwinding the entire
     * dungeon call stack in one shot — no more dungeon code runs */
    longjmp(zork_exit_jmp, 1);
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
