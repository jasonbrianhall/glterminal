// Standalone entry point — only compiled when -Dwebserver is passed, so this
// doesn't collide with the real app's main() when linked into the full build.
#ifdef webserver

#include "sftp_webserver.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <csignal>

static volatile bool g_running = true;
static void handle_signal(int) { g_running = false; }

int main(int argc, char **argv) {
    const char *root_dir  = ".";
    const char *bind_addr = "127.0.0.1";
    int port = 53716;

    // Usage: webserver [root_dir] [bind_addr] [port]
    if (argc > 1) root_dir  = argv[1];
    if (argc > 2) bind_addr = argv[2];
    if (argc > 3) port      = atoi(argv[3]);

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    if (!sftp_webserver_start_local(root_dir, bind_addr, port)) {
        fprintf(stderr, "Failed to start web server on %s:%d\n", bind_addr, port);
        return 1;
    }

    printf("Serving %s on http://%s:%d  (Ctrl+C to stop)\n",
           root_dir, bind_addr, sftp_webserver_get_port());

    while (g_running && sftp_webserver_running()) {
        SDL_Delay(200);
    }

    sftp_webserver_stop();
    return 0;
}

#endif // webserver
