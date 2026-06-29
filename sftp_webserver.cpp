#ifdef USESSH

#include "sftp_overlay.h"
#include "ssh_session.h"
#include "index.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>

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
    #include <signal.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

#include <libssh2.h>
#include <libssh2_sftp.h>

// ============================================================================
// SERVER STATE
// ============================================================================

static std::atomic<bool> s_server_running(false);
static std::atomic<bool> s_server_stop_requested(false);
static std::thread *s_server_thread = nullptr;
static std::mutex s_server_mutex;
static SOCKET s_listen_socket_global = INVALID_SOCKET;

#ifndef _WIN32
// Signal handler for CTRL+C
static void sigint_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\nSFTP webserver: SIGINT received, forcing shutdown\n");
    s_server_stop_requested.store(true, std::memory_order_release);
    
    // Force close the listen socket to interrupt select()
    if (s_listen_socket_global != INVALID_SOCKET) {
        close(s_listen_socket_global);
        s_listen_socket_global = INVALID_SOCKET;
    }
    
    exit(0);
}
#endif

#define WEBSERVER_PORT 53716
#define BACKLOG 5
#define RECV_BUF_SIZE 4096

// ============================================================================
// HELPER: Non-blocking SFTP wait
// ============================================================================

static int waitsocket_sftp(int sock, LIBSSH2_SESSION *sess) {
    struct timeval tv = { 1, 0 };
    fd_set fd, *rfd = nullptr, *wfd = nullptr;
    FD_ZERO(&fd);
    FD_SET(sock, &fd);
    int dir = libssh2_session_block_directions(sess);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)  rfd = &fd;
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) wfd = &fd;
    return select(sock + 1, rfd, wfd, nullptr, &tv);
}

// ============================================================================
// REQUEST RATE LIMITING
// ============================================================================

static std::atomic<int> s_active_requests{0};
static const int MAX_CONCURRENT_REQUESTS = 10;

struct RequestGuard {
    bool acquired = false;
    RequestGuard() {
        int current = s_active_requests.load();
        while (current < MAX_CONCURRENT_REQUESTS) {
            if (s_active_requests.compare_exchange_weak(current, current + 1)) {
                acquired = true;
                return;
            }
        }
    }
    ~RequestGuard() {
        if (acquired) s_active_requests--;
    }
};

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

static void parse_http_request(const char *request, char *method, char *path, int /*path_max*/, char *body, int body_max) {
    method[0] = path[0] = body[0] = '\0';
    sscanf(request, "%31s %255s", method, path);
    
    const char *body_start = strstr(request, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        strncpy(body, body_start, body_max - 1);
        body[body_max - 1] = '\0';
    }
}

// Extract JSON string value for a key
static bool json_get_string(const char *json, const char *key, char *value, int value_max) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    
    const char *pos = strstr(json, search);
    if (!pos) return false;
    
    pos = strchr(pos, ':');
    if (!pos) return false;
    
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t')) pos++;
    
    if (*pos != '"') return false;
    pos++;
    
    int i = 0;
    while (*pos && *pos != '"' && i < value_max - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
            if (*pos == 'n') value[i++] = '\n';
            else if (*pos == 't') value[i++] = '\t';
            else if (*pos == 'r') value[i++] = '\r';
            else if (*pos == '"') value[i++] = '"';
            else if (*pos == '\\') value[i++] = '\\';
            else value[i++] = *pos;
        } else {
            value[i++] = *pos;
        }
        pos++;
    }
    value[i] = '\0';
    return true;
}

// ============================================================================
// SFTP HANDLE MANAGEMENT (thread-local with proper cleanup)
// ============================================================================

static thread_local LIBSSH2_SFTP *tls_sftp_handle = nullptr;

// Close the current thread's SFTP handle (call after each transaction)
static void close_thread_sftp_handle() {
    if (tls_sftp_handle) {
        std::hash<std::thread::id> hash_fn;
        fprintf(stderr, "[SFTP] Channel CLOSED (tid=%zu, handle=%p)\n", 
                hash_fn(std::this_thread::get_id()), (void*)tls_sftp_handle);
        libssh2_sftp_shutdown(tls_sftp_handle);
        tls_sftp_handle = nullptr;
    }
}

// Initialize SFTP for this thread if needed
static bool ensure_sftp_init_thread() {
    if (tls_sftp_handle) {
        std::hash<std::thread::id> hash_fn;
        fprintf(stderr, "[SFTP] Channel reused (tid=%zu)\n", hash_fn(std::this_thread::get_id()));
        return true;
    }
    
    LIBSSH2_SESSION *sess = ssh_get_session();
    if (!sess) return false;
    
    ssh_session_lock();
    libssh2_session_set_blocking(sess, 1);
    tls_sftp_handle = libssh2_sftp_init(sess);
    libssh2_session_set_blocking(sess, 0);
    ssh_session_unlock();
    
    if (tls_sftp_handle) {
        std::hash<std::thread::id> hash_fn;
        fprintf(stderr, "[SFTP] Channel OPENED (tid=%zu, handle=%p)\n", 
                hash_fn(std::this_thread::get_id()), (void*)tls_sftp_handle);
    }
    return tls_sftp_handle != nullptr;
}

// ============================================================================
// REMOTE SFTP OPERATIONS
// ============================================================================

// List remote directory via SFTP and return JSON
static void list_directory_sftp_json(SOCKET client, const char *dir_path) {
    fprintf(stderr, "[SFTP] POST /api/listfiles dir=%s\n", dir_path);
    
    if (!ensure_sftp_init_thread()) {
        fprintf(stderr, "[SFTP] SFTP not initialized\n");
        send_error(client, "500 Internal Server Error", "SFTP not initialized");
        return;
    }
    
    LIBSSH2_SESSION *sess = ssh_get_session();
    int sock = ssh_get_socket();
    
    ssh_session_lock();
    libssh2_session_set_blocking(sess, 0);
    
    LIBSSH2_SFTP_HANDLE *dirh = nullptr;
    int rc;
    int retries = 0;
    while (!dirh && retries < 100) {
        dirh = libssh2_sftp_opendir(tls_sftp_handle, dir_path);
        if (!dirh) {
            if (libssh2_session_last_errno(sess) == LIBSSH2_ERROR_EAGAIN) {
                ssh_session_unlock();
                waitsocket_sftp(sock, sess);
                ssh_session_lock();
                retries++;
                continue;
            }
            fprintf(stderr, "[SFTP] 404 opendir: %s (errno=%d)\n", dir_path, libssh2_session_last_errno(sess));
            libssh2_session_set_blocking(sess, 1);
            ssh_session_unlock();
            close_thread_sftp_handle();
            send_error(client, "404 Not Found", "Directory not found or not accessible");
            return;
        }
    }

    char buffer[16384];
    int pos = 0;
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "{\"directory\":\"%s\",\"entries\":[", dir_path);

    bool first = true;
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    char filename[512];
    int entry_count = 0;
    
    for (;;) {
        rc = libssh2_sftp_readdir(dirh, filename, sizeof(filename) - 1, &attrs);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_session_unlock();
            waitsocket_sftp(sock, sess);
            ssh_session_lock();
            continue;
        }
        if (rc <= 0) break;
        
        const char *type = "file";
        if (LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) {
            type = "directory";
        } else if (LIBSSH2_SFTP_S_ISLNK(attrs.permissions)) {
            type = "link";
        }

        if (!first) {
            pos += snprintf(buffer + pos, sizeof(buffer) - pos, ",");
        }
        first = false;
        entry_count++;

        pos += snprintf(buffer + pos, sizeof(buffer) - pos,
            "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%llu,\"modified\":%lu}",
            filename, type, (unsigned long long)(attrs.filesize), (unsigned long)(attrs.mtime));

        if (pos >= (int)sizeof(buffer) - 256) {
            break;
        }
    }

    while ((rc = libssh2_sftp_closedir(dirh)) == LIBSSH2_ERROR_EAGAIN) {
        ssh_session_unlock();
        waitsocket_sftp(sock, sess);
        ssh_session_lock();
    }

    libssh2_session_set_blocking(sess, 1);
    ssh_session_unlock();

    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "]}");
    
    fprintf(stderr, "[SFTP] OK: %s (%d entries)\n", dir_path, entry_count);
    send_response(client, "200 OK", "application/json", (uint8_t *)buffer, pos);
    
    // CLOSE HANDLE AFTER TRANSACTION
    close_thread_sftp_handle();
}

// Serve a remote file via SFTP
static void serve_file_sftp(SOCKET client, const char *file_path) {
    fprintf(stderr, "[SFTP] GET %s\n", file_path);
    
    if (!ensure_sftp_init_thread()) {
        fprintf(stderr, "[SFTP] SFTP not initialized\n");
        send_error(client, "500 Internal Server Error", "SFTP not initialized");
        return;
    }

    LIBSSH2_SESSION *sess = ssh_get_session();
    int sock = ssh_get_socket();
    
    ssh_session_lock();
    libssh2_session_set_blocking(sess, 0);

    // Quick stat to check if file exists — fail fast on 404
    LIBSSH2_SFTP_ATTRIBUTES attrs = {};
    int stat_rc = libssh2_sftp_stat(tls_sftp_handle, file_path, &attrs);
    if (stat_rc == LIBSSH2_ERROR_EAGAIN) {
        // Retry a few times on EAGAIN
        for (int i = 0; i < 50 && stat_rc == LIBSSH2_ERROR_EAGAIN; i++) {
            ssh_session_unlock();
            waitsocket_sftp(sock, sess);
            ssh_session_lock();
            stat_rc = libssh2_sftp_stat(tls_sftp_handle, file_path, &attrs);
        }
    }
    
    if (stat_rc != 0 || !(attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) {
        // File not found or can't stat — fail immediately without opening
        fprintf(stderr, "[SFTP] 404: %s (stat failed: %d)\n", file_path, stat_rc);
        libssh2_session_set_blocking(sess, 1);
        ssh_session_unlock();
        close_thread_sftp_handle();
        send_error(client, "404 Not Found", "File not found");
        return;
    }
    
    // Check if it's a directory — serve index.html
    if (LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) {
        fprintf(stderr, "[SFTP] Directory detected: %s — serving index.html\n", file_path);
        libssh2_session_set_blocking(sess, 1);
        ssh_session_unlock();
        close_thread_sftp_handle();
        
        // Return index.html for any directory request
        send_response(client, "200 OK", "text/html", index_html, index_html_len);
        return;
    }
    
    // It's a file, check size attribute
    if (!(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
        fprintf(stderr, "[SFTP] 400: %s (no size in attrs)\n", file_path);
        libssh2_session_set_blocking(sess, 1);
        ssh_session_unlock();
        close_thread_sftp_handle();
        send_error(client, "400 Bad Request", "Cannot determine file size");
        return;
    }
    
    uint64_t file_size = attrs.filesize;
    if (file_size == 0 || file_size > 100 * 1024 * 1024) {  // 100MB limit
        fprintf(stderr, "[SFTP] 413: %s (size=%llu)\n", file_path, (unsigned long long)file_size);
        libssh2_session_set_blocking(sess, 1);
        ssh_session_unlock();
        close_thread_sftp_handle();
        send_error(client, "413 Payload Too Large", "File too large");
        return;
    }

    LIBSSH2_SFTP_HANDLE *fh = nullptr;
    int retries = 0;
    while (!fh && retries < 100) {
        fh = libssh2_sftp_open(tls_sftp_handle, file_path, LIBSSH2_FXF_READ, 0);
        if (!fh) {
            if (libssh2_session_last_errno(sess) == LIBSSH2_ERROR_EAGAIN) {
                ssh_session_unlock();
                waitsocket_sftp(sock, sess);
                ssh_session_lock();
                retries++;
                continue;
            }
            fprintf(stderr, "[SFTP] Cannot open: %s\n", file_path);
            libssh2_session_set_blocking(sess, 1);
            ssh_session_unlock();
            close_thread_sftp_handle();
            send_error(client, "500 Internal Server Error", "Cannot open file");
            return;
        }
    }

    uint8_t *file_content = (uint8_t *)malloc(file_size);
    if (!file_content) {
        while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) {
            ssh_session_unlock();
            waitsocket_sftp(sock, sess);
            ssh_session_lock();
        }
        libssh2_session_set_blocking(sess, 1);
        ssh_session_unlock();
        close_thread_sftp_handle();
        send_error(client, "500 Internal Server Error", "Memory allocation failed");
        return;
    }

    uint64_t total_read = 0;
    bool read_ok = true;
    
    while (total_read < file_size) {
        ssize_t n = libssh2_sftp_read(fh, (char *)file_content + total_read, 
                                       (size_t)(file_size - total_read));
        if (n == LIBSSH2_ERROR_EAGAIN) {
            ssh_session_unlock();
            waitsocket_sftp(sock, sess);
            ssh_session_lock();
            continue;
        }
        if (n < 0) {
            read_ok = false;
            break;
        }
        if (n == 0) break;
        total_read += n;
    }

    while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) {
        ssh_session_unlock();
        waitsocket_sftp(sock, sess);
        ssh_session_lock();
    }

    libssh2_session_set_blocking(sess, 1);
    ssh_session_unlock();

    if (!read_ok || total_read != file_size) {
        fprintf(stderr, "[SFTP] Read error: %s (read %llu/%llu bytes)\n", 
                file_path, (unsigned long long)total_read, (unsigned long long)file_size);
        free(file_content);
        close_thread_sftp_handle();
        send_error(client, "500 Internal Server Error", "Error reading file");
        return;
    }

    fprintf(stderr, "[SFTP] OK: %s (%llu bytes)\n", file_path, (unsigned long long)file_size);

    // Determine content type (basic)
    const char *content_type = "application/octet-stream";
    const char *ext = strrchr(file_path, '.');
    if (ext) {
        if (strcmp(ext, ".txt") == 0) content_type = "text/plain";
        else if (strcmp(ext, ".html") == 0) content_type = "text/html";
        else if (strcmp(ext, ".json") == 0) content_type = "application/json";
        else if (strcmp(ext, ".pdf") == 0) content_type = "application/pdf";
        else if (strcmp(ext, ".png") == 0) content_type = "image/png";
        else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) content_type = "image/jpeg";
        else if (strcmp(ext, ".gif") == 0) content_type = "image/gif";
    }

    send_response(client, "200 OK", content_type, file_content, (int)total_read);
    free(file_content);
    
    // CLOSE HANDLE AFTER TRANSACTION
    close_thread_sftp_handle();
}

// ============================================================================
// CLIENT HANDLER
// ============================================================================

static void handle_client(SOCKET client) {
    RequestGuard guard;
    if (!guard.acquired) {
        fprintf(stderr, "[SFTP] Too many requests (%d active)\n", s_active_requests.load() + 1);
        send_error(client, "503 Service Unavailable", "Server too busy");
        close(client);
        return;
    }

    char request[RECV_BUF_SIZE];
    int received = recv(client, request, RECV_BUF_SIZE - 1, 0);

    if (received <= 0) {
        close(client);
        return;
    }

    request[received] = '\0';

    char method[32], path[256], body[1024];
    parse_http_request(request, method, path, sizeof(path), body, sizeof(body));

    fprintf(stderr, "[SFTP] %s %s (%d active)\n", method, path, s_active_requests.load());

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/listfiles") == 0) {
        char dir_path[512];
        if (json_get_string(body, "dir", dir_path, sizeof(dir_path))) {
            list_directory_sftp_json(client, dir_path);
        } else {
            send_error(client, "400 Bad Request", "Missing 'dir' field in JSON body");
        }
    }
    else if (strcmp(path, "/api/status") == 0) {
        const char *status_json = "{\"status\": \"ok\"}";
        send_response(client, "200 OK", "application/json",
                     (uint8_t *)status_json, (int)strlen(status_json));
    }
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/") != 0) {
        // GET non-root paths serve remote files via SFTP
        serve_file_sftp(client, path);
    }
    else {
        // Root path and all other requests return index.html
        fprintf(stderr, "[SFTP] serving index.html\n");
        send_response(client, "200 OK", "text/html", index_html, index_html_len);
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
#else
    // Set up signal handler for CTRL+C
    signal(SIGINT, sigint_handler);
#endif

    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        fprintf(stderr, "SFTP webserver: socket creation failed\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }
    
    s_listen_socket_global = listen_socket;

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

    fprintf(stderr, "[SFTP] Listening on localhost:%d\n", WEBSERVER_PORT);
    s_server_running.store(true, std::memory_order_release);

    while (!s_server_stop_requested.load(std::memory_order_acquire)) {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms timeout for faster shutdown detection

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_socket, &readfds);

        int select_result = select((int)listen_socket + 1, &readfds, nullptr, nullptr, &tv);

        if (select_result < 0) {
            break;
        }

        if (select_result == 0) {
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

        handle_client(client_socket);
    }

    // Clean up thread-local SFTP handle on shutdown
    close_thread_sftp_handle();
    
    close(listen_socket);
    s_listen_socket_global = INVALID_SOCKET;
    s_server_running.store(false, std::memory_order_release);

#ifdef _WIN32
    WSACleanup();
#endif

    fprintf(stderr, "[SFTP] Webserver stopped\n");
}

// ============================================================================
// PUBLIC API
// ============================================================================

void sftp_webserver_start(void) {
    std::lock_guard<std::mutex> lock(s_server_mutex);

    if (s_server_running.load(std::memory_order_acquire)) {
        return;
    }

    s_server_stop_requested.store(false, std::memory_order_release);

    if (s_server_thread) {
        delete s_server_thread;
    }

    s_server_thread = new std::thread(server_main);
    fprintf(stderr, "[SFTP] Webserver starting...\n");
}

void sftp_webserver_stop(void) {
    std::lock_guard<std::mutex> lock(s_server_mutex);

    if (!s_server_running.load(std::memory_order_acquire)) {
        return;
    }

    fprintf(stderr, "[SFTP] Webserver stopping...\n");
    s_server_stop_requested.store(true, std::memory_order_release);

    if (s_server_thread) {
        fprintf(stderr, "[SFTP] Waiting for webserver thread...\n");
        s_server_thread->join();
        fprintf(stderr, "[SFTP] Webserver thread exited\n");
        delete s_server_thread;
        s_server_thread = nullptr;
    }
    
    s_server_running.store(false, std::memory_order_release);
}

bool sftp_webserver_is_running(void) {
    return s_server_running.load(std::memory_order_acquire);
}

#endif // USESSH
