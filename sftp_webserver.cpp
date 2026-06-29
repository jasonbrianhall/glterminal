#ifdef USESSH

#include "sftp_webserver.h"
#include "index.h"

#include <thread>
#include <atomic>
#include <mutex>
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
    #include <dirent.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #include <dirent.h>
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

static void parse_http_request(const char *request, char *method, char *path, int path_max, char *body, int body_max) {
    method[0] = path[0] = body[0] = '\0';
    sscanf(request, "%31s %255s", method, path);
    
    // Find the body (after double CRLF)
    const char *body_start = strstr(request, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        strncpy(body, body_start, body_max - 1);
        body[body_max - 1] = '\0';
    }
}

// URL decode helper
static void url_decode(const char *src, char *dst, int dst_max) {
    int i = 0, j = 0;
    while (src[i] && j < dst_max - 1) {
        if (src[i] == '%' && i + 2 < (int)strlen(src)) {
            int hex_val;
            if (sscanf(&src[i + 1], "%2x", &hex_val) == 1) {
                dst[j++] = (char)hex_val;
                i += 3;
                continue;
            }
        }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
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

// List directory and return JSON
static void list_directory_json(SOCKET client, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        send_error(client, "404 Not Found", "Directory not found or not accessible");
        return;
    }

    char buffer[16384];
    int pos = 0;
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "{\"directory\":\"%s\",\"entries\":[", dir_path);

    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) < 0) {
            continue;
        }

        const char *type = "file";
        if (S_ISDIR(st.st_mode)) {
            type = "directory";
        } else if (S_ISLNK(st.st_mode)) {
            type = "link";
        }

        if (!first) {
            pos += snprintf(buffer + pos, sizeof(buffer) - pos, ",");
        }
        first = false;

        pos += snprintf(buffer + pos, sizeof(buffer) - pos,
            "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%ld,\"modified\":%ld}",
            entry->d_name, type, (long)st.st_size, (long)st.st_mtime);

        if (pos >= (int)sizeof(buffer) - 256) {
            break;
        }
    }

    closedir(dir);
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "]}");

    send_response(client, "200 OK", "application/json", (uint8_t *)buffer, pos);
}

// Serve file content
static void serve_file(SOCKET client, const char *file_path) {
    struct stat st;
    if (stat(file_path, &st) < 0 || !S_ISREG(st.st_mode)) {
        send_error(client, "404 Not Found", "File not found or not accessible");
        return;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        send_error(client, "403 Forbidden", "Cannot read file");
        return;
    }

    // Read entire file into buffer
    char *file_content = (char *)malloc(st.st_size);
    if (!file_content) {
        fclose(f);
        send_error(client, "500 Internal Server Error", "Memory allocation failed");
        return;
    }

    size_t bytes_read = fread(file_content, 1, st.st_size, f);
    fclose(f);

    if (bytes_read != (size_t)st.st_size) {
        free(file_content);
        send_error(client, "500 Internal Server Error", "Error reading file");
        return;
    }

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

    send_response(client, "200 OK", content_type, (uint8_t *)file_content, (int)st.st_size);
    free(file_content);
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

    char method[32], path[256], body[1024];
    parse_http_request(request, method, path, sizeof(path), body, sizeof(body));

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/listfiles") == 0) {
        char dir_path[512];
        if (json_get_string(body, "dir", dir_path, sizeof(dir_path))) {
            list_directory_json(client, dir_path);
        } else {
            send_error(client, "400 Bad Request", "Missing 'dir' field in JSON body");
        }
    }
    else if (strcmp(path, "/api/status") == 0) {
        const char *status_json = "{\"status\": \"ok\"}";
        send_response(client, "200 OK", "application/json",
                     (uint8_t *)status_json, (int)strlen(status_json));
    }
    else if (strcmp(method, "GET") == 0) {
        // Check if path exists
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISREG(st.st_mode)) {
                // It's a file, serve it
                serve_file(client, path);
            } else if (S_ISDIR(st.st_mode)) {
                // It's a directory, return index.html so JS can navigate to it
                send_response(client, "200 OK", "text/html", index_html, index_html_len);
            } else {
                // Not a regular file or directory
                send_error(client, "403 Forbidden", "Not a file or directory");
            }
        } else {
            // Path doesn't exist
            send_error(client, "404 Not Found", "File or directory not found");
        }
    }
    else {
        // All other requests return index.html
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
