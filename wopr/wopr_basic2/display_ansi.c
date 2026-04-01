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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

/* ----------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------- */
static struct termios g_orig_termios;
static int g_raw = 0;
static int g_width = 80;

/* CGA index → ANSI colour number (for fg: 30+n, bg: 40+n) */
static const int cga_to_ansi[16] = {
    0, 4, 2, 6, 1, 5, 3, 7,   /* 0-7  normal */
    0, 4, 2, 6, 1, 5, 3, 7    /* 8-15 bright (handled via bold) */
};

/* ----------------------------------------------------------------
 * Raw mode helpers
 * ---------------------------------------------------------------- */
static void enter_raw(void)
{
    if (g_raw) return;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;   /* non-blocking */
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
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
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    g_raw = 0;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void display_init(void)
{
    enter_raw();
    /* Reset attributes, clear screen */
    printf("\033[0m\033[2J\033[H");
    fflush(stdout);
}

void display_shutdown(void)
{
    leave_raw();
    printf("\033[0m\n");
    fflush(stdout);
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
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return g_width;
}

/* INKEY$ — non-blocking */
int display_inkey(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return (n == 1) ? (int)c : 0;
}

/* Blocking getchar for LINE INPUT */
int display_getchar(void)
{
    /* temporarily switch to blocking + echo */
    leave_raw();

    struct termios cooked = g_orig_termios;
    cooked.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &cooked);

    int c = getchar();

    /* go back to raw */
    enter_raw();
    return c;
}
