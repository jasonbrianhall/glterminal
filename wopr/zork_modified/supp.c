/* supp.c -- support routines for dungeon */
/* Modified for WOPR overlay:
 *   more_output() buffers text and flushes complete lines on '\n'.
 *   more_output(NULL) flushes any partial line as a separator.
 *   more_input() flushes any remaining partial line before waiting for input.
 *   zork_shim_fgets() blocks on a semaphore until input is posted.
 *   exit_() sets a flag instead of calling exit().
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <SDL2/SDL.h>

#ifdef unix
#include <sys/types.h>
#endif

#ifdef BSD4_2
#include <sys/time.h>
#else
#include <time.h>
#endif

#include "../zork/funcs.h"

extern void exit P((int));
extern int  rand P((void));
extern time_t time P((time_t *));
extern struct tm *localtime ();

/* ============================================================================
 * Shared I/O surfaces
 * ========================================================================== */

void wopr_zork_push_line(const char *line);   /* implemented in wopr_zork.cpp */

char          zork_input_buf[512];
int           zork_input_ready = 0;
int           g_zork_game_over = 0;
SDL_sem      *zork_input_sem   = NULL;

void zork_shim_init(void)
{
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
    if (zork_input_sem)
        SDL_SemWait(zork_input_sem);
    if (!zork_input_ready) {
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
 *
 * Dungeon builds output in three patterns:
 *   1. more_output("full line\n")
 *   2. more_output(NULL) ... more_output("%d", x) ... more_output("\n")
 *   3. more_output("word ") ... more_output("word\n")
 *
 * more_output(NULL) is used as a line-start sentinel — flush any pending
 * partial line first, then start a new one.
 * Flush on '\n' and also on more_input() before blocking for input.
 * ========================================================================== */

static int  crows      = 24;
static int  coutput    = 0;
static char s_linebuf[4096];
static int  s_linelen  = 0;

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
    crows = 24;
}

void more_output(const char *fmt, ...)
{
    char buf[4096];
    int  i;

    if (fmt == NULL) {
        /* NULL is a line separator — flush whatever we have */
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
    /* Flush any partial line before blocking for input */
    flush_linebuf();
    coutput = 0;
}

/* ============================================================================
 * Lifecycle
 * ========================================================================== */

void exit_(void)
{
    flush_linebuf();
    more_output("The game is over.\n");
    g_zork_game_over = 1;
    if (zork_input_sem)
        SDL_SemPost(zork_input_sem);
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
