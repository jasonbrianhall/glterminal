/*
 * display_ansi.cpp — ANSI terminal display for text mode
 * Minimal stub for DOS build
 */

#include "display.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

BASIC_NS_BEGIN

void display_init(void) {
    /* Initialize terminal */
}

void display_shutdown(void) {
    /* Clean up */
}

void display_cls(void) {
    printf("\033[2J\033[H");  /* ANSI clear screen */
    fflush(stdout);
}

void display_locate(int row, int col) {
    printf("\033[%d;%dH", row, col);  /* ANSI cursor position */
    fflush(stdout);
}

void display_color(int fg, int bg) {
    /* ANSI color codes - simplified */
    if (fg >= 0 && fg <= 15) {
        printf("\033[%dm", 30 + (fg % 8));  /* foreground */
    }
    if (bg >= 0 && bg <= 15) {
        printf("\033[%dm", 40 + (bg % 8));  /* background */
    }
    fflush(stdout);
}

void display_width(int cols) {
    /* Ignore - terminal width is fixed */
}

void display_print(char *s) {
    if (s) {
        printf("%s", s);
        fflush(stdout);
    }
}

void display_putchar(int c) {
    printf("%c", c);
    fflush(stdout);
}

void display_newline(void) {
    printf("\n");
    fflush(stdout);
}

int display_inkey(void) {
    /* Non-blocking input not easily implemented in text mode */
    return 0;
}

int display_getchar(void) {
    return getchar();
}

int display_getline(char *buf, int bufsz) {
    if (!fgets(buf, bufsz, stdin)) return -1;
    /* Remove trailing newline */
    int len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    return len;
}

void display_cursor(int visible) {
    /* Ignore - cursor visibility is not controllable in simple text mode */
}

void display_spc(int n) {
    for (int i = 0; i < n; i++) printf(" ");
    fflush(stdout);
}

int display_get_width(void) {
    return 80;  /* Standard DOS/ANSI terminal width */
}

BASIC_NS_END
