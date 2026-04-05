/* supp.c -- support routines for dungeon */
/* Modified for WOPR overlay */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <SDL2/SDL.h>
#include "basic_print.h"

#ifdef unix
#include <sys/types.h>
#endif

#ifdef BSD4_2
#include <sys/time.h>
#else
#include <time.h>
#endif
#include <SDL2/SDL.h>
/* ============================================================================
 * Shared I/O surfaces
 * ========================================================================== */

void wopr_basic_push_line(const char *line);
void wopr_basic_signal_done(void);

char          basic_input_buf[512];
int           basic_input_ready         = 0;
int           g_basic_game_over         = 0;
int           g_basic_waiting_input     = 0;
int           g_basic_suppress_newline  = 0;
SDL_sem      *basic_input_sem           = NULL;

/* longjmp target set in basic_thread_fn before calling basic_main() */
jmp_buf       basic_exit_jmp;

void basic_shim_init(void)
{
    if (basic_input_sem) {
        SDL_DestroySemaphore(basic_input_sem);
        basic_input_sem = NULL;
    }
    basic_input_sem = SDL_CreateSemaphore(0);
}

void basic_shim_set_input(const char *line)
{
    SDL_Log("basic_shim_set_input called with '%s'", line);
    strncpy(basic_input_buf, line, sizeof(basic_input_buf) - 1);
    basic_input_buf[sizeof(basic_input_buf) - 1] = '\0';
    basic_input_ready = 1;
    if (basic_input_sem) {
        SDL_Log("Posting semaphore %p", (void*)basic_input_sem);
        SDL_SemPost(basic_input_sem);
    } else {
        SDL_Log("NO semaphore to post");
    }
}


char *basic_shim_fgets(char *buf, int n)
{
    SDL_Log("In Fgets: sem=%p ready=%d game_over=%d",
            (void*)basic_input_sem, basic_input_ready, g_basic_game_over);

    if (g_basic_game_over) {
        SDL_Log("Game over (early)");
        if (n > 0) buf[0] = '\0';
        return buf;
    }

    if (basic_input_sem) {
        SDL_Log("Waiting on semaphore");
        wopr_basic_flush_partial();  /* flush "YOUR CHOICE?" etc. before showing cursor */
        g_basic_waiting_input = 1;
        SDL_SemWait(basic_input_sem);
        g_basic_waiting_input = 0;
        SDL_Log("Woke from semaphore: ready=%d game_over=%d",
                basic_input_ready, g_basic_game_over);
    } else {
        SDL_Log("No semaphore, polling");
        wopr_basic_flush_partial();
        g_basic_waiting_input = 1;
        while (!basic_input_ready && !g_basic_game_over) {
            SDL_Delay(10);
        }
        g_basic_waiting_input = 0;
        SDL_Log("Exited poll: ready=%d game_over=%d",
                basic_input_ready, g_basic_game_over);
    }

    if (!basic_input_ready || g_basic_game_over) {
        SDL_Log("No input ready after wait: ready=%d game_over=%d",
                basic_input_ready, g_basic_game_over);
        if (n > 0) buf[0] = '\0';
        return buf;
    }

    strncpy(buf, basic_input_buf, n - 1);
    buf[n - 1] = '\0';
    basic_input_ready = 0;
    SDL_Log("Returned Buffer is '%s'", buf);
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
        wopr_basic_push_line(s_linebuf);
        s_linelen = 0;
    }
}

void basic_more_init(void)
{
    crows     = 24;
    s_linelen = 0;
}

void basic_more_output(const char *fmt, ...)
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

void basic_more_input(void)
{
    flush_linebuf();
    coutput = 0;
}

/* ============================================================================
 * Lifecycle
 * ========================================================================== */

void basic_exit_(void)
{
    flush_linebuf();
    g_basic_game_over = 1;
    /* Unblock any fgets waiting for input */
    if (basic_input_sem)
        SDL_SemPost(basic_input_sem);
    /* Jump all the way back to basic_thread_fn, unwinding the entire
     * dungeon call stack in one shot — no more dungeon code runs */
    longjmp(basic_exit_jmp, 1);
}

void basic_itime_(int *hrptr, int *minptr, int *secptr)
{
    time_t    timebuf;
    struct tm *tmptr;
    time(&timebuf);
    tmptr   = localtime(&timebuf);
    *hrptr  = tmptr->tm_hour;
    *minptr = tmptr->tm_min;
    *secptr = tmptr->tm_sec;
}

int basic_rnd_(int maxval)
{
    return rand() % maxval;
}

int basic_printf(const char *fmt, ...)
{
    char buf[4096];

    if (fmt == NULL) {
        return 0;  /* was fflush(stdout) — no-op in WOPR mode */
    }

    // Format into buf
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // Push the entire formatted string directly to WOPR
    wopr_basic_push_line(buf);

    return 0;
}


