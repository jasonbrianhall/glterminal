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
#include "gfx.h"

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
    if (gfx_is_open()) { gfx_cls(0); return; }
    printf("\033[2J\033[H");
    fflush(stdout);
}

void display_locate(int row, int col)
{
    if (gfx_is_open()) { gfx_locate(row, col); return; }
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    printf("\033[%d;%dH", row, col);
    fflush(stdout);
}

void display_color(int fg, int bg)
{
    if (fg < 0 || fg > 15) fg = 7;
    if (bg < 0 || bg > 15) bg = 0;
    if (gfx_is_open()) { gfx_color(fg, bg); return; }
    int bold   = (fg >= 8) ? 1 : 0;
    int fg_idx = cga_to_ansi[fg & 7];
    int bg_idx = cga_to_ansi[bg & 7];
    printf("\033[%d;%d;%dm", bold, 30 + fg_idx, 40 + bg_idx);
    fflush(stdout);
}

void display_width(int cols)
{
    g_width = cols;
    if (gfx_is_open()) return;
    /* Modern terminals don't support \033[?3h (40-column mode).
     * Just record the width; the program manages its own layout. */
    fflush(stdout);
}

void display_print(const char *s)
{
    if (gfx_is_open()) {
        for (const char *p = s; *p; p++)
            gfx_print_char((unsigned char)*p, -1, -1);
        gfx_flush();
        return;
    }
    fputs(s, stdout);
    fflush(stdout);
}

void display_putchar(int c)
{
    if (gfx_is_open()) { gfx_print_char((unsigned char)c, -1, -1); gfx_flush(); return; }
    putchar(c);
    fflush(stdout);
}

void display_newline(void)
{
    if (gfx_is_open()) { gfx_print_char('\n', -1, -1); gfx_flush(); return; }
    putchar('\n');
    fflush(stdout);
}

void display_cursor(int visible)
{
    if (gfx_is_open()) { gfx_cursor(visible); return; }
    if (visible)
        printf("\033[?25h");
    else
        printf("\033[?25l");
    fflush(stdout);
}

void display_spc(int n)
{
    if (gfx_is_open()) {
        for (int i = 0; i < n; i++) gfx_print_char(' ', -1, -1);
        gfx_flush();
        return;
    }
    for (int i = 0; i < n; i++) putchar(' ');
    fflush(stdout);
}

int display_get_width(void)
{
    if (gfx_is_open()) return gfx_get_cols();
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
#endif
    return g_width;
}


/* Blocking line read */
int display_getline(char *buf, int bufsz)
{
    if (gfx_is_open()) return gfx_getline(buf, bufsz);
#ifndef _WIN32
    /* Unconditionally ensure stdin is blocking + canonical for line input */
    int fd_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fd_flags & ~O_NONBLOCK);

    struct termios cooked = g_orig_termios;
    cooked.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &cooked);

    int len = 0;
    while (len < bufsz - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || c == '\n') break;
        if (c == '\r') continue;
        buf[len++] = c;
    }
    buf[len] = '\0';

    /* Restore raw+nonblocking if that's what we had before */
    if (g_raw) {
        struct termios raw = g_orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        fcntl(STDIN_FILENO, F_SETFL, fd_flags | O_NONBLOCK);
    }
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
    if (gfx_is_open()) return gfx_getchar();
#ifndef _WIN32
    int fd_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fd_flags & ~O_NONBLOCK);

    struct termios cooked = g_orig_termios;
    cooked.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &cooked);

    char c = 0;
    ssize_t nr;
    do { nr = read(STDIN_FILENO, &c, 1); } while (nr < 0 && errno == EINTR);

    if (g_raw) {
        struct termios raw = g_orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        fcntl(STDIN_FILENO, F_SETFL, fd_flags | O_NONBLOCK);
    }
    return (unsigned char)c;
#else
    int c = getchar();
    return (c == EOF) ? 0 : (unsigned char)c;
#endif
}

/* ================================================================
 * Graphics dispatch — delegate to gfx_sdl when a screen is open,
 * stub out silently otherwise.  display_ansi.c stays text-only.
 * ================================================================ */

void display_set_screen(int mode)
{
    if (mode == 0) {
        if (gfx_is_open()) gfx_close();
        /* Return terminal to normal */
#ifndef _WIN32
        printf("\033[0m\033[2J\033[H");
        fflush(stdout);
#endif
    } else {
        gfx_open(mode);
    }
}

int display_get_screen(void) { return gfx_is_open() ? gfx_get_mode() : 0; }

/* For INKEY$: when a gfx window is open, poll it; else poll the terminal */
int display_inkey(void)
{
    if (gfx_is_open()) return gfx_inkey();
    /* original ANSI non-blocking read */
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

/* Graphics primitives: forward to gfx layer */
void display_pset(int x, int y, int c)                          { gfx_pset(x,y,c); }
int  display_point(int x, int y)                                { return gfx_point(x,y); }
void display_line(int x1,int y1,int x2,int y2,int c,int s)     { gfx_line(x1,y1,x2,y2,c,s); }
void display_circle(int cx,int cy,int r,int c,
                    double asp,double sa,double ea,int fill)
{
    gfx_circle(cx,cy,r,c,asp,sa,ea);
    if (fill) gfx_paint(cx,cy,c<0?1:c,c<0?1:c);
}
void display_paint(int x,int y,int pc,int bc)                   { gfx_paint(x,y,pc,bc); }
void display_draw(const char *s)                                 { gfx_draw(s); }
void display_palette(int i,int r,int g,int b)                   { gfx_palette(i,r,g,b); }
void display_get_pen(int *x,int *y)                             { gfx_get_pen(x,y); }
void display_set_pen(int x,int y,int c)                         { gfx_set_pen(x,y,c); }
int  display_get_rect(int x1,int y1,int x2,int y2,int *buf)    { return gfx_get_rect(x1,y1,x2,y2,buf); }
void display_put_rect(int x,int y,int w,int h,const int *buf,int mode) { gfx_put_rect(x,y,w,h,buf,mode); }
