/*
 * display_ansi.c — ANSI/VT100 terminal implementation of display.h
 *
 * Three runtime backends selected by compile flags:
 *   (default)       — raw ANSI/VT100 to stdout
 *   -DWOPR          — WOPR overlay shim (wopr_basic_push_line etc.)
 *   -DFELIX_BASIC   — F8 standalone console shim (felix_basic_push_line etc.)
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

#ifdef WOPR
#include <SDL2/SDL.h>
#elif defined(FELIX_BASIC)
#include <SDL2/SDL.h>
#include "felix_basic.h"
#elif defined(_WIN32)
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

BASIC_NS_BEGIN

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
    printf("\033[0m\033[?25h\n");
    fflush(stdout);
}

static void signal_handler(int sig) {
    cleanup_terminal();
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
#if !defined(WOPR) && !defined(FELIX_BASIC)
#  ifndef _WIN32
    printf("\033[0m\033[2J\033[H");
    fflush(stdout);
#  endif
#endif
}

void display_shutdown(void)
{
#if !defined(WOPR) && !defined(FELIX_BASIC)
    sound_shutdown();
#endif
}

void display_cls(void)
{
#ifdef WOPR
    wopr_basic_cls();
#elif defined(FELIX_BASIC)
    felix_basic_cls();
#else
    printf("\033[2J\033[H");
    fflush(stdout);
#endif
}

void display_locate(int row, int col)
{
#ifdef WOPR
    wopr_basic_locate(row, col);
#elif defined(FELIX_BASIC)
    (void)row; (void)col;
    felix_basic_flush_partial();
#else
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    printf("\033[%d;%dH", row, col);
    fflush(stdout);
#endif
}

void display_color(int fg, int bg)
{
#ifdef WOPR
    wopr_basic_color(fg, bg);
#elif defined(FELIX_BASIC)
    (void)bg;
    felix_basic_color(fg);
#else
    if (fg < 0 || fg > 15) fg = 7;
    if (bg < 0 || bg > 15) bg = 0;

    int bold   = (fg >= 8) ? 1 : 0;
    int fg_idx = cga_to_ansi[fg & 7];
    int bg_idx = cga_to_ansi[bg & 7];

    printf("\033[%d;%d;%dm", bold, 30 + fg_idx, 40 + bg_idx);
    fflush(stdout);
#endif
}

void display_width(int cols)
{
#ifdef WOPR
    (void)cols;
#elif defined(FELIX_BASIC)
    (void)cols;
#else
    g_width = cols;
    if (cols == 40)
        printf("\033[?3h");
    else
        printf("\033[?3l");
    fflush(stdout);
#endif
}

void display_print(char *s)
{
#ifdef WOPR
    if (strcmp(s, "Ok\n") == 0) return;
    if (strncmp(s, "WOPR BASIC", 10) == 0) return;
    if (strncmp(s, "Type NEW,", 9) == 0) return;
    g_basic_suppress_newline = 0;
    wopr_basic_push_line(s);
#elif defined(FELIX_BASIC)
    felix_basic_push_line(s);
#else
    fputs(s, stdout);
    fflush(stdout);
#endif
}

void display_putchar(int c)
{
#ifdef WOPR
    g_basic_suppress_newline = 0;
    char tmp[2] = { (char)c, 0 };
    wopr_basic_push_line(tmp);
#elif defined(FELIX_BASIC)
    char tmp[2] = { (char)c, 0 };
    felix_basic_push_line(tmp);
#else
    putchar(c);
    fflush(stdout);
#endif
}

void display_newline(void)
{
#ifdef WOPR
    if (g_basic_suppress_newline) {
        g_basic_suppress_newline = 0;
        return;
    }
    wopr_basic_push_line("\n");
#elif defined(FELIX_BASIC)
    felix_basic_push_line("\n");
#else
    putchar('\n');
    fflush(stdout);
#endif
}

void display_cursor(int visible)
{
#ifdef WOPR
    (void)visible;
#elif defined(FELIX_BASIC)
    (void)visible;
#else
    if (visible)
        printf("\033[?25h");
    else
        printf("\033[?25l");
    fflush(stdout);
#endif
}

void display_spc(int n)
{
#ifdef WOPR
    for (int i = 0; i < n; i++) wopr_basic_push_line(" ");
#elif defined(FELIX_BASIC)
    for (int i = 0; i < n; i++) felix_basic_push_line(" ");
#else
    for (int i = 0; i < n; i++) putchar(' ');
    fflush(stdout);
#endif
}

int display_get_width(void)
{
#if defined(WOPR) || defined(FELIX_BASIC)
    return g_width;
#elif !defined(_WIN32)
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return g_width;
#else
    return g_width;
#endif
}

/* INKEY$ — non-blocking */
int display_inkey(void)
{
#ifdef WOPR
    // Flush any pending partial line so it shows as the prompt while
    // the program spins on INKEY$ waiting for a keypress.
    wopr_basic_flush_partial();
    int c = wopr_basic_get_key();
    if (c >= 0) return c;
#  ifdef _WIN32
    SDL_Delay(1);
#  else
    usleep(1000);
#  endif
    return 0;
#elif defined(FELIX_BASIC)
    int c = felix_basic_get_key();
    if (c >= 0) return c;
    SDL_Delay(1);
    return 0;
#elif !defined(_WIN32)
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return (int)c;
    usleep(1000);
    return 0;
#else
    SDL_Delay(1);
    return 0;
#endif
}

/* Blocking line read */
int display_getline(char *buf, int bufsz)
{
#ifdef WOPR
    if (!basic_shim_fgets(buf, bufsz)) {
        buf[0] = '\0';
        return 0;
    }
    // Echo the typed line to the terminal buffer
    wopr_basic_push_line(buf);
    g_basic_suppress_newline = 1;
    SDL_Log("Returning Buffer %s\n", buf);
    return (int)strlen(buf);
#elif defined(FELIX_BASIC)
    felix_basic_shim_fgets(buf, bufsz);
    return (int)strlen(buf);
#elif !defined(_WIN32)
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

/* Single blocking getchar */
int display_getchar(void)
{
#ifdef WOPR
    char buf[2] = {0};
    if (!basic_shim_fgets(buf, sizeof(buf))) return 0;
    return (unsigned char)buf[0];
#elif defined(FELIX_BASIC)
    char buf[2] = {0};
    felix_basic_shim_fgets(buf, sizeof(buf));
    return (unsigned char)buf[0];
#elif !defined(_WIN32)
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

BASIC_NS_END
