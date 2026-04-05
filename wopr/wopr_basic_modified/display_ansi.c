/*
 * display_ansi.c — ANSI/VT100 terminal implementation of display.h
 *
 * Replace this file with display_sdl.c or display_opengl.c to port the
 * interpreter to a graphical backend.  The interface in display.h stays
 * the same; this file is the only thing that changes.
 *
 * CGA colour mapping to ANSI:
 *   0=Black 1=Blue 2=Green 3=Cyan 4=Red 5=Magenta 6=Brown/Yellow 7=LightGrey
 *   8=DarkGrey 9=LightBlue 10=LightGreen 11=LightCyan 12=LightRed
 *   13=LightMagenta 14=Yellow 15=White
 */

#include "display.h"
#include "sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <SDL2/SDL.h>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#endif

#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

/* ----------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------- */
#ifndef _WIN32
static struct termios g_orig_termios;
static int g_raw = 0;
#endif
static int g_width = 80;

/* CGA index → ANSI colour number (for fg: 30+n, bg: 40+n) */
static const int cga_to_ansi[16] = {
    0, 4, 2, 6, 1, 5, 3, 7,   /* 0-7  normal */
    0, 4, 2, 6, 1, 5, 3, 7    /* 8-15 bright (handled via bold) */
};

/* ----------------------------------------------------------------
 * Raw mode helpers
 * ---------------------------------------------------------------- */
#ifndef _WIN32
static void enter_raw(void)
{
    if (g_raw) return;
    struct termios raw = g_orig_termios;  /* always base off the one-time snapshot */
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;   /* non-blocking */
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    /* make stdin non-blocking */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    g_raw = 1;
}

static void leave_raw(void)
{
    if (!g_raw) return;
    /* restore blocking */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    g_raw = 0;
}
#else
static void enter_raw(void) {}
static void leave_raw(void) {}
#endif

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

static void cleanup_terminal(void) {
    leave_raw();
    /* Reset all attributes, show cursor, move to bottom */
    printf("\033[0m\033[?25h\n");
    fflush(stdout);
}

static void signal_handler(int sig) {
    cleanup_terminal();
    /* Re-raise with default handler so shell gets correct exit status */
    signal(sig, SIG_DFL);
    raise(sig);
}

void display_init(void)
{
    /* basic_shim_init() is called by wopr_basic_enter() before the thread
     * starts — do NOT call it here. */

#ifndef _WIN32
    tcgetattr(STDIN_FILENO, &g_orig_termios);
#endif
    enter_raw();
    atexit(cleanup_terminal);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    /* sound_init() is called by wopr_basic_enter() on the main thread */
}

void display_shutdown(void)
{
    /* sound_shutdown() is called by wopr_basic_free() on the main thread */
}

void display_cls(void)
{
    wopr_basic_cls();
}

void display_locate(int row, int col)
{
    (void)row; (void)col;
    wopr_basic_flush_partial();
}

void display_color(int fg, int bg)
{
    (void)bg;
    wopr_basic_color(fg);
}

void display_width(int cols)
{
    (void)cols;
}

void display_print(const char *s)
{
    if (strcmp(s, "Ok\n") == 0) return;
    if (strncmp(s, "WOPR BASIC", 10) == 0) return;
    if (strncmp(s, "Type NEW,", 9) == 0) return;
    g_basic_suppress_newline = 0;
    wopr_basic_push_line(s);
}

void display_putchar(int c)
{
    g_basic_suppress_newline = 0;
    char tmp[2] = { (char)c, 0 };
    wopr_basic_push_line(tmp);
}

void display_newline(void)
{
    if (g_basic_suppress_newline) {
        g_basic_suppress_newline = 0;
        return;
    }
    wopr_basic_push_line("\n");
}

void display_cursor(int visible)
{
    (void)visible;
}

void display_spc(int n)
{
    for (int i = 0; i < n; i++) wopr_basic_push_line(" ");
}

int display_get_width(void)
{
    return g_width;
}

int display_inkey(void)
{
    int c = wopr_basic_get_key();
    if (c >= 0) return c;
#ifdef _WIN32
    SDL_Delay(1);
#else
    usleep(1000);
#endif
    return 0;
}

int display_getline(char *buf, int bufsz)
{
    basic_shim_fgets(buf, bufsz);
    g_basic_suppress_newline = 1;
    SDL_Log("Returning Buffer %s\n", buf);
    return (int)strlen(buf);
}

int display_getchar(void)
{
    char buf[2] = {0};
    basic_shim_fgets(buf, sizeof(buf));
    return (unsigned char)buf[0];
}

