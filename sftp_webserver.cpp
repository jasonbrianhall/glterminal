#ifdef USESSH

#include "sftp_webserver.h"
#include "index.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define close closesocket
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

// ============================================================================
// SERVER STATE
// ============================================================================

static std::atomic<bool> s_server_running(false);
static std::atomic<bool> s_server_stop_requested(false);
static std::thread *s_server_thread = nullptr;
static std::mutex s_server_mutex;

#define WEBSERVER_PORT 53716
#define BACKLOG 5
#define RECV_BUF_SIZE 4096

// ============================================================================
// HTTP HELPERS
// ============================================================================

static void send_response(SOCKET client, const char *status, const char *content_type,
                         const uint8_t *body, int body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, body_len);

    send(client, header, header_len, 0);
    if (body_len > 0) {
        send(client, (const char *)body, body_len, 0);
    }
}

static void send_error(SOCKET client, const char *status, const char *message) {
    char body[256];
    int body_len = snprintf(body, sizeof(body), "<html><body><h1>%s</h1></body></html>", message);
    send_response(client, status, "text/html", (uint8_t *)body, body_len);
}

static void parse_http_request(const char *request, char *method, char *path, int path_max) {
    method[0] = path[0] = '\0';
    sscanf(request, "%31s %255s", method, path);
}

// ============================================================================
// CLIENT HANDLER
// ============================================================================

static void handle_client(SOCKET client) {
    char request[RECV_BUF_SIZE];
    int received = recv(client, request, RECV_BUF_SIZE - 1, 0);

    if (received <= 0) {
        close(client);
        return;
    }

    request[received] = '\0';

    char method[32], path[256];
    parse_http_request(request, method, path, sizeof(path));

    // Normalize path
    if (path[0] == '\0' || strcmp(path, "/") == 0) {
        send_response(client, "200 OK", "text/html", index_html, index_html_len);
    }
    else if (strcmp(path, "/api/status") == 0) {
        const char *status_json = "{\"status\": \"ok\"}";
        send_response(client, "200 OK", "application/json",
                     (uint8_t *)status_json, (int)strlen(status_json));
    }
    else {
        send_error(client, "404 Not Found", "Resource not found");
    }

    close(client);
}

// ============================================================================
// SERVER MAIN LOOP
// ============================================================================

static void server_main() {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "SFTP webserver: WSAStartup failed\n");
        return;
    }
#endif

    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        fprintf(stderr, "SFTP webserver: socket creation failed\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    // Allow reuse of address
    int reuse = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "SFTP webserver: setsockopt failed\n");
        close(listen_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(WEBSERVER_PORT);

    if (bind(listen_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "SFTP webserver: bind failed (port %d may be in use)\n", WEBSERVER_PORT);
        close(listen_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    if (listen(listen_socket, BACKLOG) == SOCKET_ERROR) {
        fprintf(stderr, "SFTP webserver: listen failed\n");
        close(listen_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    fprintf(stderr, "SFTP webserver: listening on localhost:%d\n", WEBSERVER_PORT);
    s_server_running.store(true, std::memory_order_release);

    while (!s_server_stop_requested.load(std::memory_order_acquire)) {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_socket, &readfds);

        int select_result = select((int)listen_socket + 1, &readfds, nullptr, nullptr, &tv);

        if (select_result < 0) {
            break;
        }

        if (select_result == 0) {
            // Timeout, check stop flag again
            continue;
        }

        if (!FD_ISSET(listen_socket, &readfds)) {
            continue;
        }

        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        SOCKET client_socket = accept(listen_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == INVALID_SOCKET) {
            continue;
        }

        // Handle client (blocking)
        handle_client(client_socket);
    }

    close(listen_socket);
    s_server_running.store(false, std::memory_order_release);

#ifdef _WIN32
    WSACleanup();
#endif

    fprintf(stderr, "SFTP webserver: stopped\n");
}

// ============================================================================
// PUBLIC API
// ============================================================================

void sftp_webserver_start(void) {
    std::lock_guard<std::mutex> lock(s_server_mutex);

    if (s_server_running.load(std::memory_order_acquire)) {
        return;  // Already running
    }

    s_server_stop_requested.store(false, std::memory_order_release);

    if (s_server_thread) {
        delete s_server_thread;
    }

    s_server_thread = new std::thread(server_main);
    fprintf(stderr, "SFTP webserver: starting...\n");
}

void sftp_webserver_stop(void) {
    std::lock_guard<std::mutex> lock(s_server_mutex);

    if (!s_server_running.load(std::memory_order_acquire)) {
        return;  // Not running
    }

    fprintf(stderr, "SFTP webserver: stopping...\n");
    s_server_stop_requested.store(true, std::memory_order_release);

    if (s_server_thread) {
        s_server_thread->join();
        delete s_server_thread;
        s_server_thread = nullptr;
    }
}

bool sftp_webserver_is_running(void) {
    return s_server_running.load(std::memory_order_acquire);
}

#endif // USESSH
