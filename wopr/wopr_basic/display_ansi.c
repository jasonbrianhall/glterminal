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
#ifdef _WIN32
#include <SDL2/SDL.h>
#endif

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
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    g_raw = 1;
}

static void leave_raw(void)
{
    if (!g_raw) return;
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
#ifndef _WIN32
    tcgetattr(STDIN_FILENO, &g_orig_termios);
#endif
    enter_raw();
    atexit(cleanup_terminal);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
#ifndef _WIN32
    printf("\033[0m\033[2J\033[H");
    fflush(stdout);
#endif
    sound_init();
}

void display_shutdown(void)
{
    sound_shutdown();
}

void display_cls(void)
{
    printf("\033[2J\033[H");
    fflush(stdout);
}

void display_locate(int row, int col)
{
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    printf("\033[%d;%dH", row, col);
    fflush(stdout);
}

void display_color(int fg, int bg)
{
    /* clamp */
    if (fg < 0 || fg > 15) fg = 7;
    if (bg < 0 || bg > 15) bg = 0;

    int bold   = (fg >= 8) ? 1 : 0;
    int fg_idx = cga_to_ansi[fg & 7];
    int bg_idx = cga_to_ansi[bg & 7];

    printf("\033[%d;%d;%dm", bold, 30 + fg_idx, 40 + bg_idx);
    fflush(stdout);
}

void display_width(int cols)
{
    g_width = cols;
    /* Ask terminal to switch column mode if supported */
    if (cols == 40)
        printf("\033[?3h");   /* DECCOLM 40-col — works on some terminals */
    else
        printf("\033[?3l");   /* DECCOLM 80-col */
    fflush(stdout);
}

void display_print(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

void display_putchar(int c)
{
    putchar(c);
    fflush(stdout);
}

void display_newline(void)
{
    putchar('\n');
    fflush(stdout);
}

void display_cursor(int visible)
{
    if (visible)
        printf("\033[?25h");
    else
        printf("\033[?25l");
    fflush(stdout);
}

void display_spc(int n)
{
    for (int i = 0; i < n; i++) putchar(' ');
    fflush(stdout);
}

int display_get_width(void)
{
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
#endif
    return g_width;
}

/* INKEY$ — non-blocking */
int display_inkey(void)
{
#ifndef _WIN32
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return (int)c;
    usleep(1000);
#else
    SDL_Delay(1);
#endif
    return 0;
}

/* Blocking line read for LINE INPUT — reads a full line into buf, up to bufsz-1 chars */
int display_getline(char *buf, int bufsz)
{
#ifndef _WIN32
    leave_raw();

    struct termios cooked = g_orig_termios;
    cooked.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &cooked);

    int len = 0;
    while (len < bufsz - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) break;
        if (n <= 0 || c == '\n') break;
        if (c == '\r') continue;
        buf[len++] = c;
    }
    buf[len] = '\0';

    enter_raw();
    return len;
#else
    if (!fgets(buf, bufsz, stdin)) { buf[0] = '\0'; return 0; }
    int len = (int)strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    return len;
#endif
}

/* Single blocking getchar — kept for compatibility */
int display_getchar(void)
{
#ifndef _WIN32
    leave_raw();

    struct termios cooked = g_orig_termios;
    cooked.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &cooked);

    char c = 0;
    ssize_t nr = read(STDIN_FILENO, &c, 1);
    (void)nr;

    enter_raw();
    return (unsigned char)c;
#else
    int c = getchar();
    return (c == EOF) ? 0 : (unsigned char)c;
#endif
}
