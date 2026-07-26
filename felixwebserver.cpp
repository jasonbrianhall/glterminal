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
#include <cctype>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <windows.h>
#endif

static volatile bool g_running = true;
static void handle_signal(int) { g_running = false; }

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-D] [--root PATH] [--listen [HOST]:PORT] [--pidfile PATH] [--log PATH]\n"
        "       %s --stop [--pidfile PATH]\n"
        "\n"
        "  --root PATH        directory to serve (default /)\n"
        "  --listen [HOST]:PORT\n"
        "                     address to bind. HOST may be a specific IPv4 or IPv6\n"
        "                     address, or omitted/'*'/':'-only to listen on all\n"
        "                     interfaces for BOTH IPv4 and IPv6 (dual-stack), e.g.\n"
        "                     --listen [::]:53716, --listen :::53716, or\n"
        "                     --listen :53716 (default 127.0.0.1:53716)\n"
        "  -D, --daemon       fork into the background\n"
        "  --pidfile PATH     PID file location (default /tmp/flt_webserver.pid)\n"
        "  --log PATH         stdout/stderr destination in daemon mode (default /tmp/flt_webserver.log)\n"
        "  --stop             send SIGTERM to the PID in --pidfile and exit\n",
        prog, prog);
}

// Parses "[HOST]:PORT" or "HOST:PORT" (e.g. "[::]:53716" — the unambiguous
// standard form — as well as looser forms like ":53716", "::53716",
// ":::53716", "0.0.0.0:53716", "192.168.1.5:53716", or a specific IPv6
// literal like "[2001:db8::1]:53716"). HOST may be empty, "*", or any run of
// ':' characters to mean "listen on all interfaces" (both IPv4 and IPv6 —
// the backend binds a dual-stack AF_INET6 socket with IPV6_V6ONLY off).
// Without brackets, splits on the LAST ':' so a leading "::" isn't mistaken
// for the host/port separator itself.
// Returns false (host/port untouched) if the spec can't be parsed.
static bool parse_listen_spec(const char *spec, std::string &host_out, int &port_out) {
    std::string s(spec);
    std::string host, port_str;

    if (!s.empty() && s[0] == '[') {
        size_t close = s.find(']');
        if (close == std::string::npos || close + 1 >= s.size() || s[close + 1] != ':') return false;
        host = s.substr(1, close - 1);
        port_str = s.substr(close + 2);
    } else {
        size_t colon = s.find_last_of(':');
        host = (colon == std::string::npos) ? "" : s.substr(0, colon);
        port_str = (colon == std::string::npos) ? s : s.substr(colon + 1);
    }

    if (port_str.empty()) return false;
    for (char c : port_str) if (!isdigit((unsigned char)c)) return false;
    int port = atoi(port_str.c_str());
    if (port <= 0 || port > 65535) return false;

    bool wildcard = host.empty() || host == "*";
    if (!wildcard) {
        wildcard = true;
        for (char c : host) if (c != ':') { wildcard = false; break; }
    }
    host_out = wildcard ? "::" : host;
    port_out = port;
    return true;
}

#ifndef _WIN32
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
#endif

// Runs the server loop in the calling process. Used directly in foreground
// mode, and inside the grandchild once daemonized.
static int run_server(const char *root_dir, const char *bind_addr, int port,
                      const char *pidfile /* nullptr in foreground mode */,
                      int report_fd /* -1 in foreground mode */) {
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    bool ok = sftp_webserver_start_local(root_dir, bind_addr, port);

#ifndef _WIN32
    if (report_fd >= 0) {
        char status = ok ? 1 : 0;
        write(report_fd, &status, 1);
        close(report_fd);
    }
#else
    (void)report_fd;
#endif

    if (!ok) {
        fprintf(stderr, "Failed to start web server on %s:%d\n", bind_addr, port);
        if (pidfile) remove(pidfile);
        return 1;
    }

    printf("Serving %s on http://%s:%d%s\n",
           root_dir, bind_addr, sftp_webserver_get_port(),
           pidfile ? "" : "  (Ctrl+C to stop)");
    fflush(stdout);

    while (g_running && sftp_webserver_running()) {
        SDL_Delay(200);
    }

    sftp_webserver_stop();
    return 0;
}

#ifndef _WIN32
// Double-fork daemonize that blocks the ORIGINAL invocation until the
// grandchild has actually attempted the bind, so failures (e.g. port already
// in use) are reported on the terminal immediately instead of only showing
// up in the log file.
static int daemonize_and_run(const char *pidfile, const char *logfile,
                              const char *root_dir, const char *bind_addr, int port) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid > 0) {
        // Original process: wait for the grandchild's one-byte status.
        close(pipefd[1]);
        char status = 0;
        ssize_t n = read(pipefd[0], &status, 1);
        close(pipefd[0]);
        if (n == 1 && status == 1) {
            printf("Serving %s on http://%s:%d (daemonized, pidfile %s, log %s)\n",
                   root_dir, bind_addr, port, pidfile, logfile);
            return 0;
        }
        fprintf(stderr, "flt_webserver failed to start — see %s\n", logfile);
        return 1;
    }

    // First child: detach from the controlling terminal.
    close(pipefd[0]);
    if (setsid() < 0) { _exit(1); }
    signal(SIGHUP, SIG_IGN);

    pid_t pid2 = fork();
    if (pid2 < 0) { _exit(1); }
    if (pid2 > 0) _exit(0);   // intermediate child exits; grandchild can't reacquire a tty

    // Grandchild: the actual daemon.
    umask(0027);
    if (chdir("/") != 0) { /* non-fatal */ }

    int devnull = open("/dev/null", O_RDONLY);
    int logfd   = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (devnull >= 0) { dup2(devnull, STDIN_FILENO);  close(devnull); }
    if (logfd   >= 0) { dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO); close(logfd); }

    FILE *f = fopen(pidfile, "w");
    if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }

    int rc = run_server(root_dir, bind_addr, port, pidfile, pipefd[1]);
    _exit(rc);
}
#endif // !_WIN32

int main(int argc, char **argv) {
    const char *root_dir  = "/";
    const char *bind_addr = "127.0.0.1";
    int  port         = 53716;
    bool daemon_mode  = false;
    bool stop_mode    = false;
    const char *pidfile = "/tmp/flt_webserver.pid";
    const char *logfile = "/tmp/flt_webserver.log";

    int positional = 0;
    bool have_root = false;
    bool have_listen = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--daemon") || !strcmp(argv[i], "-D")) {
            daemon_mode = true;
        } else if (!strcmp(argv[i], "--stop")) {
            stop_mode = true;
        } else if (!strcmp(argv[i], "--root")) {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", argv[i]); print_usage(argv[0]); return 1; }
            root_dir = argv[++i];
            have_root = true;
        } else if (!strcmp(argv[i], "--listen")) {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", argv[i]); print_usage(argv[0]); return 1; }
            std::string host; int p;
            if (!parse_listen_spec(argv[++i], host, p)) {
                fprintf(stderr, "Invalid --listen value '%s' (expected [HOST]:PORT)\n", argv[i]);
                return 1;
            }
            static std::string host_storage;   // outlives this scope for bind_addr to point to
            host_storage = host;
            bind_addr = host_storage.c_str();
            port = p;
            have_listen = true;
        } else if (!strcmp(argv[i], "--pidfile")) {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", argv[i]); print_usage(argv[0]); return 1; }
            pidfile = argv[++i];
        } else if (!strcmp(argv[i], "--log")) {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", argv[i]); print_usage(argv[0]); return 1; }
            logfile = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            // Old-style positional fallback: root_dir bind_addr port.
            // --root / --listen (if given) take precedence over these.
            switch (positional++) {
                case 0: if (!have_root) root_dir = argv[i]; break;
                case 1: if (!have_listen) bind_addr = argv[i]; break;
                case 2: if (!have_listen) port = atoi(argv[i]); break;
                default: break;
            }
        }
    }

    if (stop_mode) {
#ifndef _WIN32
        return stop_daemon(pidfile);
#else
        fprintf(stderr, "--stop is not supported on Windows.\n");
        return 1;
#endif
    }

    // Resolve root_dir to an absolute path BEFORE daemonizing — daemonizing
    // chdir()s to "/", so a relative path like "." would otherwise end up
    // serving the wrong directory.
#ifndef _WIN32
    char resolved_root[PATH_MAX];
    if (realpath(root_dir, resolved_root)) {
        root_dir = resolved_root;
    } else {
        fprintf(stderr, "Warning: could not resolve '%s' (%s); using as-is\n",
                root_dir, strerror(errno));
    }
#else
    static char resolved_root[MAX_PATH];
    if (_fullpath(resolved_root, root_dir, MAX_PATH)) {
        root_dir = resolved_root;
    } else {
        fprintf(stderr, "Warning: could not resolve '%s'; using as-is\n", root_dir);
    }
#endif

    if (daemon_mode) {
#ifndef _WIN32
        return daemonize_and_run(pidfile, logfile, root_dir, bind_addr, port);
#else
        fprintf(stderr, "-D/--daemon is not supported on Windows; run in the foreground "
                        "(e.g. via NSSM or a Scheduled Task if you need it backgrounded).\n");
        return 1;
#endif
    }

    return run_server(root_dir, bind_addr, port, nullptr, -1);
}

#endif // webserver
