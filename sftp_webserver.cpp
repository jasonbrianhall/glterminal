#ifdef USESSH

#include "sftp_webserver.h"
#include "ssh_session.h"
#include "sftp_overlay.h"

#include "index.h"  // Embedded index.html

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <SDL2/SDL.h>

#include <thread>
#include <atomic>
#include <memory>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define closesocket close
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR (-1)
#endif

// ============================================================================
// Globals
// ============================================================================

static std::atomic<bool> g_webserver_running(false);
static std::atomic<bool> g_webserver_should_exit(false);
static std::thread *g_webserver_thread = nullptr;
static int g_listen_socket = INVALID_SOCKET;
static int g_tunnel_counter = 0;  // Monotonic tunnel ID counter
static SDL_mutex *g_tunnel_mutex = nullptr;

// ============================================================================
// SFTP File Operations
// ============================================================================

struct RemoteFile {
    char name[512];
    bool is_dir;
    uint64_t size;
    time_t modified;
};

static bool sftp_list_directory(LIBSSH2_SFTP *sftp, const char *path, 
                                std::vector<RemoteFile> &entries) {
    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, path);
    if (!dir) {
        SDL_Log("[SFTP] opendir failed for path: %s", path);
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    char name[512];
    int rc;

    while (1) {
        rc = libssh2_sftp_readdir(dir, name, sizeof(name) - 1, &attrs);
        
        if (rc > 0) {
            // Skip . and .. entries
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }
            
            // Got an entry
            RemoteFile entry;
            strncpy(entry.name, name, sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
            entry.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                           LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            entry.size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
            entry.modified = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? attrs.mtime : 0;
            entries.push_back(entry);
        } else if (rc == 0) {
            // EOF - end of directory listing
            break;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            // Non-blocking mode would need to wait, but we set blocking mode
            // This shouldn't happen, but just in case, keep trying
            continue;
        } else {
            // Error
            SDL_Log("[SFTP] readdir error: %d for path: %s", rc, path);
            libssh2_sftp_closedir(dir);
            return false;
        }
    }

    libssh2_sftp_closedir(dir);
    return true;
}

static bool sftp_get_file(LIBSSH2_SFTP *sftp, const char *remote_path,
                          std::vector<uint8_t> &data) {
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path,
                                                     LIBSSH2_FXF_READ,
                                                     LIBSSH2_SFTP_S_IRUSR);
    if (!handle) return false;

    char buffer[65536];
    int bytes_read;

    while ((bytes_read = libssh2_sftp_read(handle, buffer, sizeof(buffer))) > 0) {
        data.insert(data.end(), (uint8_t *)buffer, (uint8_t *)buffer + bytes_read);
    }

    libssh2_sftp_close_handle(handle);
    return bytes_read == 0;  // bytes_read == 0 means EOF (success)
}

// ============================================================================
// HTTP Response Builders
// ============================================================================

static std::string http_response_headers(int status_code, const char *content_type,
                                        size_t content_length) {
    char buf[512];
    const char *status_text = "OK";
    if (status_code == 400) status_text = "Bad Request";
    else if (status_code == 404) status_text = "Not Found";
    else if (status_code == 500) status_text = "Internal Server Error";

    snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, content_length);
    return buf;
}

static std::string json_escape(const std::string &s) {
    std::string result;
    for (char c : s) {
        if (c == '"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else result += c;
    }
    return result;
}

static std::string build_directory_json(const std::vector<RemoteFile> &entries) {
    std::string json = "{\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) json += ",";
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%llu,\"modified\":%ld}",
            json_escape(entries[i].name).c_str(),
            entries[i].is_dir ? "directory" : "file",
            (unsigned long long)entries[i].size,
            (long)entries[i].modified);
        json += buf;
    }
    json += "]}";
    return json;
}

// Get embedded index.html
static std::string get_index_html() {
    return std::string((const char *)index_html, index_html_len);
}

// Determine MIME type from file extension
static const char* get_mime_type(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    
    // Images
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".webp") == 0) return "image/webp";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".bmp") == 0) return "image/bmp";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    
    // Documents
    if (strcmp(ext, ".pdf") == 0) return "application/pdf";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".xml") == 0) return "application/xml";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".csv") == 0) return "text/csv";
    if (strcmp(ext, ".md") == 0) return "text/markdown";
    
    // Code
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".py") == 0) return "text/x-python";
    if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0) return "text/x-c++src";
    if (strcmp(ext, ".c") == 0) return "text/x-csrc";
    if (strcmp(ext, ".h") == 0 || strcmp(ext, ".hpp") == 0) return "text/x-chdr";
    if (strcmp(ext, ".java") == 0) return "text/x-java";
    if (strcmp(ext, ".rb") == 0) return "text/x-ruby";
    if (strcmp(ext, ".go") == 0) return "text/x-go";
    if (strcmp(ext, ".rs") == 0) return "text/x-rust";
    
    // Video
    if (strcmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcmp(ext, ".webm") == 0) return "video/webm";
    if (strcmp(ext, ".ogg") == 0) return "video/ogg";
    if (strcmp(ext, ".mov") == 0) return "video/quicktime";
    if (strcmp(ext, ".mkv") == 0) return "video/x-matroska";
    if (strcmp(ext, ".avi") == 0) return "video/x-msvideo";
    
    // Audio
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcmp(ext, ".wav") == 0) return "audio/wav";
    if (strcmp(ext, ".m4a") == 0) return "audio/mp4";
    if (strcmp(ext, ".flac") == 0) return "audio/flac";
    if (strcmp(ext, ".aac") == 0) return "audio/aac";
    if (strcmp(ext, ".ogg") == 0) return "audio/ogg";
    
    // Archives
    if (strcmp(ext, ".zip") == 0) return "application/zip";
    if (strcmp(ext, ".tar") == 0) return "application/x-tar";
    if (strcmp(ext, ".gz") == 0 || strcmp(ext, ".gzip") == 0) return "application/gzip";
    if (strcmp(ext, ".7z") == 0) return "application/x-7z-compressed";
    if (strcmp(ext, ".rar") == 0) return "application/x-rar-compressed";
    
    return "application/octet-stream";
}

// ============================================================================
// Request Handler
// ============================================================================

static void handle_http_request(int client_socket, int tunnel_id) {
    char buffer[8192];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] Connection closed immediately", tunnel_id);
        return;
    }

    buffer[bytes_received] = '\0';

    // Parse HTTP request line
    char method[32], path[1024], version[32];
    if (sscanf(buffer, "%31s %1023s %31s", method, path, version) != 3) {
        std::string response = http_response_headers(400, "text/plain", 11);
        response += "Bad Request";
        send(client_socket, response.c_str(), response.length(), 0);
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] Malformed request", tunnel_id);
        return;
    }

    SDL_Log("[Tunnel %d] Request: %s %s", tunnel_id, method, path);

    // Establish SFTP tunnel for this request
    ssh_session_lock();
    LIBSSH2_SESSION *sess = ssh_get_session();
    if (!sess) {
        ssh_session_unlock();
        std::string response = http_response_headers(500, "text/plain", 22);
        response += "SSH session not active";
        send(client_socket, response.c_str(), response.length(), 0);
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] No SSH session", tunnel_id);
        return;
    }

    // Set blocking mode for SFTP operations
    libssh2_session_set_blocking(sess, 1);
    
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(sess);

    if (!sftp) {
        libssh2_session_set_blocking(sess, 0);
        ssh_session_unlock();
        std::string response = http_response_headers(500, "text/plain", 22);
        response += "SFTP init failed";
        send(client_socket, response.c_str(), response.length(), 0);
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] SFTP init failed", tunnel_id);
        return;
    }

    // Handle /api/listfiles request
    if (strcmp(method, "POST") == 0 && strstr(path, "/api/listfiles") != nullptr) {
        // Parse JSON body to get directory path
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (!body_start) {
            body_start = strstr(buffer, "\n\n");
            if (body_start) body_start += 2;
            else body_start = (char *)buffer;
        } else {
            body_start += 4;
        }

        char dir_path[1024] = "/";
        // Very basic JSON parsing for {"dir": "/path/to/dir"}
        char *dir_ptr = strstr(body_start, "\"dir\":");
        if (dir_ptr) {
            dir_ptr += 6;
            while (*dir_ptr && (*dir_ptr == ' ' || *dir_ptr == '"')) dir_ptr++;
            int i = 0;
            while (i < 1023 && *dir_ptr && *dir_ptr != '"' && *dir_ptr != '}') {
                dir_path[i++] = *dir_ptr++;
            }
            dir_path[i] = '\0';
        }

        // Normalize: decode %20 and backslashes to forward slashes
        std::string normalized = dir_path;
        size_t pos = 0;
        while ((pos = normalized.find("%20", pos)) != std::string::npos) {
            normalized.replace(pos, 3, " ");
            pos += 1;
        }
        pos = 0;
        while ((pos = normalized.find('\\', pos)) != std::string::npos) {
            normalized[pos] = '/';
        }

        std::vector<RemoteFile> entries;
        bool success = sftp_list_directory(sftp, normalized.c_str(), entries);

        std::string json = build_directory_json(entries);
        std::string response = http_response_headers(200, "application/json", json.length());
        response += json;

        send(client_socket, response.c_str(), response.length(), 0);
        SDL_Log("[Tunnel %d] Listed directory: %s (%zu entries, success=%d)", tunnel_id, normalized.c_str(), entries.size(), success);
    }
    // Handle file/directory GET requests
    else if (strcmp(method, "GET") == 0) {
        // Decode URL path (basic: just handle %20 for spaces and backslashes)
        std::string decoded_path = path;
        size_t pos = 0;
        while ((pos = decoded_path.find("%20", pos)) != std::string::npos) {
            decoded_path.replace(pos, 3, " ");
            pos += 1;
        }
        // Normalize backslashes to forward slashes
        pos = 0;
        while ((pos = decoded_path.find('\\', pos)) != std::string::npos) {
            decoded_path[pos] = '/';
        }

        // Check if path is a directory or file
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        int rc = libssh2_sftp_stat(sftp, decoded_path.c_str(), &attrs);

        // If stat fails or it's a directory, serve index.html
        bool is_directory = (rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && 
                            LIBSSH2_SFTP_S_ISDIR(attrs.permissions));
        
        if (rc != 0 && decoded_path != "/") {
            // Not found (and not root)
            std::string response = http_response_headers(404, "text/plain", 9);
            response += "Not found";
            send(client_socket, response.c_str(), response.length(), 0);
            SDL_Log("[Tunnel %d] File not found: %s", tunnel_id, decoded_path.c_str());
        }
        else if (is_directory || decoded_path == "/") {
            // Directory: serve embedded index.html
            std::string html = get_index_html();
            std::string response = http_response_headers(200, "text/html", html.length());
            response += html;
            send(client_socket, response.c_str(), response.length(), 0);
            SDL_Log("[Tunnel %d] Served directory listing HTML: %s", tunnel_id, decoded_path.c_str());
        }
        else {
            // File: download it
            std::vector<uint8_t> file_data;
            if (sftp_get_file(sftp, decoded_path.c_str(), file_data)) {
                // Determine content type based on file extension
                const char *content_type = get_mime_type(decoded_path.c_str());

                std::string response = http_response_headers(200, content_type, file_data.size());
                send(client_socket, response.c_str(), response.length(), 0);
                if (!file_data.empty()) {
                    send(client_socket, (const char *)file_data.data(), file_data.size(), 0);
                }
                SDL_Log("[Tunnel %d] Downloaded file: %s (%zu bytes, type=%s)", tunnel_id, 
                         decoded_path.c_str(), file_data.size(), content_type);
            } else {
                std::string response = http_response_headers(500, "text/plain", 14);
                response += "Download error";
                send(client_socket, response.c_str(), response.length(), 0);
                SDL_Log("[Tunnel %d] Failed to download: %s", tunnel_id, decoded_path.c_str());
            }
        }
    }
    else {
        std::string response = http_response_headers(400, "text/plain", 18);
        response += "Method not allowed";
        send(client_socket, response.c_str(), response.length(), 0);
        SDL_Log("[Tunnel %d] Method not allowed: %s", tunnel_id, method);
    }

    // Clean up SFTP
    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(sess, 0);
    ssh_session_unlock();

    closesocket(client_socket);
    SDL_Log("[Tunnel %d] Closed", tunnel_id);
}

// Web Server Thread
// ============================================================================

static void webserver_thread_func() {
    SDL_Log("[WebServer] Starting on port 53716");

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        SDL_Log("[WebServer] WSAStartup failed");
        return;
    }
#endif

    // Create listen socket
    g_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SDL_Log("[WebServer] socket() returned: %d", g_listen_socket);
    if (g_listen_socket == INVALID_SOCKET) {
        SDL_Log("[WebServer] socket() failed: %s (errno=%d)", strerror(errno), errno);
        return;
    }

    // Set reuse address option (must be done BEFORE bind)
    int reuse = 1;
#ifdef _WIN32
    int ret = setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    if (ret < 0) {
        SDL_Log("[WebServer] setsockopt(SO_REUSEADDR) failed");
    }
#else
    int ret = setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    SDL_Log("[WebServer] setsockopt() returned: %d", ret);
    if (ret < 0) {
        SDL_Log("[WebServer] setsockopt(SO_REUSEADDR) failed: %s (errno=%d)", strerror(errno), errno);
    }
#endif

    // Bind to port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53716);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");  // Only localhost for security
    
    SDL_Log("[WebServer] Attempting bind to 127.0.0.1:53716 (socket=%d)", g_listen_socket);

    ret = bind(g_listen_socket, (struct sockaddr *)&addr, sizeof(addr));
    SDL_Log("[WebServer] bind() returned: %d", ret);
    if (ret < 0) {
        SDL_Log("[WebServer] bind() failed: %s (errno=%d, port may be in use)", strerror(errno), errno);
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
        return;
    }
    SDL_Log("[WebServer] bind() successful");

    if (listen(g_listen_socket, 5) < 0) {
        SDL_Log("[WebServer] listen() failed: %s (errno=%d)", strerror(errno), errno);
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
        return;
    }
    SDL_Log("[WebServer] listen() successful");

    // Set to non-blocking AFTER listen
#ifndef _WIN32
    int flags = fcntl(g_listen_socket, F_GETFL, 0);
    fcntl(g_listen_socket, F_SETFL, flags | O_NONBLOCK);
#else
    u_long mode = 1;  // non-blocking
    ioctlsocket(g_listen_socket, FIONBIO, &mode);
#endif

    g_webserver_running.store(true);
    SDL_Log("[WebServer] Listening on localhost:53716");

    // Accept loop
    while (!g_webserver_should_exit.load()) {
        struct sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        socklen_t addr_len = sizeof(client_addr);

        int client_socket = accept(g_listen_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket == INVALID_SOCKET) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINTR) {
                SDL_Log("[WebServer] accept() error: %d", err);
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                SDL_Log("[WebServer] accept() error: %s", strerror(errno));
            }
#endif
            SDL_Delay(10);
            continue;
        }

        // Assign tunnel ID
        int tunnel_id;
        SDL_LockMutex(g_tunnel_mutex);
        tunnel_id = ++g_tunnel_counter;
        SDL_UnlockMutex(g_tunnel_mutex);

        SDL_Log("[Tunnel %d] Opened", tunnel_id);

        // Handle request in blocking mode
        handle_http_request(client_socket, tunnel_id);
    }

    // Cleanup
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    g_webserver_running.store(false);
    SDL_Log("[WebServer] Stopped");
}

// ============================================================================
// Public API
// ============================================================================

bool sftp_webserver_start() {
    if (g_webserver_running.load()) {
        SDL_Log("[WebServer] Already running");
        return true;
    }

    if (!g_tunnel_mutex) {
        g_tunnel_mutex = SDL_CreateMutex();
    }

    g_webserver_should_exit.store(false);
    g_webserver_thread = new std::thread(webserver_thread_func);
    
    // Give thread time to start
    SDL_Delay(100);
    
    return g_webserver_running.load();
}

void sftp_webserver_stop() {
    if (!g_webserver_running.load()) return;

    SDL_Log("[WebServer] Shutdown requested");
    g_webserver_should_exit.store(true);

    if (g_webserver_thread) {
        g_webserver_thread->join();
        delete g_webserver_thread;
        g_webserver_thread = nullptr;
    }

    if (g_tunnel_mutex) {
        SDL_DestroyMutex(g_tunnel_mutex);
        g_tunnel_mutex = nullptr;
    }
}

bool sftp_webserver_running() {
    return g_webserver_running.load();
}

#endif // USESSH
