/* supp.c -- support routines for dungeon */
/* Modified for WOPR overlay:
 *   more_output() formats and calls wopr_zork_push_line() directly.
 *   exit_() sets a flag instead of calling exit().
 *   zork_input_buf / zork_input_ready replace stdin for gdt.c.
 */

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

extern void exit P((int));
extern int  rand P((void));
extern time_t time P((time_t *));
extern struct tm *localtime ();

/* ============================================================================
 * Shared I/O surfaces
 * ========================================================================== */

void wopr_zork_push_line(const char *line);   /* implemented in wopr_zork.cpp */

char zork_input_buf[512];
int  zork_input_ready = 0;
int  g_zork_game_over = 0;

void zork_shim_set_input(const char *line)
{
    strncpy(zork_input_buf, line, sizeof(zork_input_buf) - 1);
    zork_input_buf[sizeof(zork_input_buf) - 1] = '\0';
    zork_input_ready = 1;
}

char *zork_shim_fgets(char *buf, int n)
{
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
 * more_output — format and push directly, no buffering
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
        printf("Buffer is %s\n", buf);
        wopr_zork_push_line(buf);
    }

    coutput++;
}

void more_input(void)
{
    coutput = 0;
}

/* ============================================================================
 * Lifecycle
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
