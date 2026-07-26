// Standalone entry point — only compiled when -Dwebserver is passed, so this
// doesn't collide with the real app's main() when linked into the full build.
#ifdef webserver

#include "sftp_webserver.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

static volatile bool g_running = true;
static void handle_signal(int) { g_running = false; }

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-D] [--pidfile PATH] [--log PATH] [root_dir] [bind_addr] [port]\n"
        "       %s --stop [--pidfile PATH]\n"
        "\n"
        "  -D, --daemon     fork into the background\n"
        "  --pidfile PATH   PID file location (default /tmp/flt_webserver.pid)\n"
        "  --log PATH       stdout/stderr destination in daemon mode (default /tmp/flt_webserver.log)\n"
        "  --stop           send SIGTERM to the PID in --pidfile and exit\n",
        prog, prog);
}

// Double-fork daemonize. Must be called AFTER any relative paths (root_dir,
// pidfile, logfile) have been resolved to absolute paths, since this chdir()s
// to "/" and detaches stdio.
static void daemonize(const char *pidfile, const char *logfile) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) _exit(0);                 // first parent exits

    if (setsid() < 0) { perror("setsid"); exit(1); }
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) _exit(0);                 // second parent exits (can't reacquire a tty)

    umask(0027);
    if (chdir("/") != 0) { /* non-fatal */ }

    int devnull = open("/dev/null", O_RDONLY);
    int logfd   = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (devnull >= 0) { dup2(devnull, STDIN_FILENO);  close(devnull); }
    if (logfd   >= 0) { dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO); close(logfd); }

    FILE *f = fopen(pidfile, "w");
    if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
}

static int stop_daemon(const char *pidfile) {
    FILE *f = fopen(pidfile, "r");
    if (!f) { fprintf(stderr, "No pidfile at %s\n", pidfile); return 1; }
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 0) { fprintf(stderr, "Invalid pid in %s\n", pidfile); return 1; }
    if (kill(pid, SIGTERM) != 0) { perror("kill"); return 1; }
    printf("Sent SIGTERM to pid %d\n", pid);
    remove(pidfile);
    return 0;
}

int main(int argc, char **argv) {
    const char *root_dir  = "/";
    const char *bind_addr = "127.0.0.1";
    int  port         = 53716;
    bool daemon_mode  = false;
    bool stop_mode    = false;
    const char *pidfile = "/tmp/flt_webserver.pid";
    const char *logfile = "/tmp/flt_webserver.log";

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--daemon") || !strcmp(argv[i], "-D")) {
            daemon_mode = true;
        } else if (!strcmp(argv[i], "--stop")) {
            stop_mode = true;
        } else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) {
            pidfile = argv[++i];
        } else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            logfile = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else {
            switch (positional++) {
                case 0: root_dir  = argv[i]; break;
                case 1: bind_addr = argv[i]; break;
                case 2: port      = atoi(argv[i]); break;
                default: break;
            }
        }
    }

    if (stop_mode) {
        return stop_daemon(pidfile);
    }

    // Resolve root_dir to an absolute path BEFORE daemonizing — daemonize()
    // chdir()s to "/", so a relative path like "." would otherwise end up
    // serving the wrong directory.
    char resolved_root[PATH_MAX];
    if (realpath(root_dir, resolved_root)) {
        root_dir = resolved_root;
    } else {
        fprintf(stderr, "Warning: could not resolve '%s' (%s); using as-is\n",
                root_dir, strerror(errno));
    }

    if (daemon_mode) {
        daemonize(pidfile, logfile);
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    if (!sftp_webserver_start_local(root_dir, bind_addr, port)) {
        fprintf(stderr, "Failed to start web server on %s:%d\n", bind_addr, port);
        return 1;
    }

    printf("Serving %s on http://%s:%d%s\n",
           root_dir, bind_addr, sftp_webserver_get_port(),
           daemon_mode ? " (daemonized)" : "  (Ctrl+C to stop)");
    fflush(stdout);

    while (g_running && sftp_webserver_running()) {
        SDL_Delay(200);
    }

    sftp_webserver_stop();
    return 0;
}

#endif // webserver
