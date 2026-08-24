#ifndef _WIN32
#include "term_pty.h"

#include <SDL2/SDL.h>
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

// Optional write override (set by SSH layer; nullptr = use pty_fd)
void (*g_term_write_override)(Terminal *t, const char *s, int n) = nullptr;

bool term_spawn(Terminal *t, const char *cmd) {
    // See term_resize() for why: report one fewer column/row than we
    // actually render, so a full-width line from the child never reaches
    // our real autowrap boundary.
    int report_cols = t->cols > 1 ? t->cols - 1 : t->cols;
    int report_rows = t->rows > 1 ? t->rows - 1 : t->rows;
    struct winsize ws = {
        .ws_row    = (unsigned short)report_rows,
        .ws_col    = (unsigned short)report_cols,
        .ws_xpixel = (unsigned short)(report_cols*(int)t->cell_w),
        .ws_ypixel = (unsigned short)(report_rows*(int)t->cell_h)
    };

    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        return false;
    }

    if (pid == 0) {
        // NOTE: deliberately NOT setting $COLUMNS/$LINES here. ncurses
        // prioritizes those env vars over the real ioctl(TIOCGWINSZ) size,
        // and since they'd only ever be set once (at spawn), they go stale
        // after any resize — causing apps like top to wrap/pad at the old
        // width forever. The pty winsize (set below, and kept in sync by
        // term_resize's TIOCSWINSZ call) is the single source of truth.
        setenv("TERM",      g_term_type,   1);
        setenv("COLORTERM", "truecolor",   1);

        // --- X11-related env: preserve if present ---
        const char *disp = getenv("DISPLAY");
        if (disp && *disp) {
            setenv("DISPLAY", disp, 1);
        }

        const char *xauth = getenv("XAUTHORITY");
        if (xauth && *xauth) {
            setenv("XAUTHORITY", xauth, 1);
        } else {
            // Optional: try a sane default if not set
            const char *home = getenv("HOME");
            if (home && *home) {
                char xauth_path[PATH_MAX];
                snprintf(xauth_path, sizeof(xauth_path),
                         "%s/.Xauthority", home);
                setenv("XAUTHORITY", xauth_path, 1);
            }
        }

        const char *argv[] = { cmd, NULL };
        execvp(argv[0], (char * const *)argv);
        _exit(1);
    }

    t->pty_fd = master;
    t->child  = pid;

    int fl = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);

    return true;
}

bool term_read(Terminal *t) {
    char buf[4096];
    bool got_data = false;
    for (;;) {
        ssize_t n = read(t->pty_fd, buf, sizeof(buf));
        if (n > 0) { term_feed(t, buf, (int)n); got_data = true; }
        else break;
    }
    return got_data;
}

void term_write(Terminal *t, const char *s, int n) {
    if (g_term_write_override) { g_term_write_override(t, s, n); return; }
    if (t->pty_fd >= 0) { ssize_t r = write(t->pty_fd, s, n); (void)r; }
}
#endif
