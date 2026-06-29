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
static int g_tunnel_counter = 0;
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
    return bytes_read == 0;
}

static bool sftp_put_file(LIBSSH2_SFTP *sftp, const char *remote_path,
                          const uint8_t *data, size_t data_len) {
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path,
                                                     LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                                     LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | 
                                                     LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) return false;

    size_t written = 0;
    while (written < data_len) {
        int rc = libssh2_sftp_write(handle, (const char *)data + written, data_len - written);
        if (rc < 0) {
            libssh2_sftp_close_handle(handle);
            return false;
        }
        written += rc;
    }

    libssh2_sftp_close_handle(handle);
    return true;
}

static bool sftp_delete_file(LIBSSH2_SFTP *sftp, const char *remote_path) {
    return libssh2_sftp_unlink(sftp, remote_path) == 0;
}

static bool sftp_delete_dir(LIBSSH2_SFTP *sftp, const char *remote_path) {
    return libssh2_sftp_rmdir(sftp, remote_path) == 0;
}

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
            
            RemoteFile entry;
            strncpy(entry.name, name, sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
            entry.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                           LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            entry.size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
            entry.modified = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? attrs.mtime : 0;
            entries.push_back(entry);
        } else if (rc == 0) {
            break;
        } else {
            SDL_Log("[SFTP] readdir error: %d for path: %s", rc, path);
            libssh2_sftp_closedir(dir);
            return false;
        }
    }

    libssh2_sftp_closedir(dir);
    return true;
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

static std::string get_index_html() {
    return std::string((const char *)index_html, index_html_len);
}

static const char* get_mime_type(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".webp") == 0) return "image/webp";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".bmp") == 0) return "image/bmp";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".pdf") == 0) return "application/pdf";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".xml") == 0) return "application/xml";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".csv") == 0) return "text/csv";
    if (strcmp(ext, ".md") == 0) return "text/markdown";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcmp(ext, ".webm") == 0) return "video/webm";
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcmp(ext, ".wav") == 0) return "audio/wav";
    
    return "application/octet-stream";
}

// ============================================================================
// Request Handler
// ============================================================================

static void handle_http_request(int client_socket, int tunnel_id) {
    char buffer[65536];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] Connection closed immediately", tunnel_id);
        return;
    }

    buffer[bytes_received] = '\0';

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

    if (strcmp(method, "POST") == 0 && strstr(path, "/api/listfiles") != nullptr) {
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (!body_start) body_start = strstr(buffer, "\n\n");
        if (body_start && *body_start == '\n') body_start += 2;
        else if (body_start) body_start += 4;
        else body_start = buffer;

        char dir_path[1024] = "/";
        char *dir_ptr = strstr(body_start ? body_start : buffer, "\"dir\":");
        if (dir_ptr) {
            dir_ptr += 6;
            while (*dir_ptr && (*dir_ptr == ' ' || *dir_ptr == '"')) dir_ptr++;
            int i = 0;
            while (i < 1023 && *dir_ptr && *dir_ptr != '"' && *dir_ptr != '}') {
                dir_path[i++] = *dir_ptr++;
            }
            dir_path[i] = '\0';
        }

        std::vector<RemoteFile> entries;
        sftp_list_directory(sftp, dir_path, entries);

        std::string json = build_directory_json(entries);
        std::string response = http_response_headers(200, "application/json", json.length());
        response += json;

        send(client_socket, response.c_str(), response.length(), 0);
        SDL_Log("[Tunnel %d] Listed directory: %s (%zu entries)", tunnel_id, dir_path, entries.size());
    }
    else if (strcmp(method, "GET") == 0) {
        std::string decoded_path = path;
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        int rc = libssh2_sftp_stat(sftp, decoded_path.c_str(), &attrs);

        bool is_directory = (rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && 
                            LIBSSH2_SFTP_S_ISDIR(attrs.permissions));
        
        if (rc != 0 && decoded_path != "/") {
            std::string response = http_response_headers(404, "text/plain", 9);
            response += "Not found";
            send(client_socket, response.c_str(), response.length(), 0);
            SDL_Log("[Tunnel %d] File not found: %s", tunnel_id, decoded_path.c_str());
        }
        else if (is_directory || decoded_path == "/") {
            std::string html = get_index_html();
            std::string response = http_response_headers(200, "text/html", html.length());
            response += html;
            send(client_socket, response.c_str(), response.length(), 0);
            SDL_Log("[Tunnel %d] Served directory listing HTML: %s", tunnel_id, decoded_path.c_str());
        }
        else {
            std::vector<uint8_t> file_data;
            if (sftp_get_file(sftp, decoded_path.c_str(), file_data)) {
                const char *content_type = get_mime_type(decoded_path.c_str());
                std::string response = http_response_headers(200, content_type, file_data.size());
                send(client_socket, response.c_str(), response.length(), 0);
                if (!file_data.empty()) {
                    send(client_socket, (const char *)file_data.data(), file_data.size(), 0);
                }
                SDL_Log("[Tunnel %d] Downloaded file: %s (%zu bytes)", tunnel_id, decoded_path.c_str(), file_data.size());
            } else {
                std::string response = http_response_headers(500, "text/plain", 14);
                response += "Download error";
                send(client_socket, response.c_str(), response.length(), 0);
                SDL_Log("[Tunnel %d] Failed to download: %s", tunnel_id, decoded_path.c_str());
            }
        }
    }
    else if (strcmp(method, "DELETE") == 0) {
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (!body_start) body_start = strstr(buffer, "\n\n");
        if (body_start && *body_start == '\n') body_start += 2;
        else if (body_start) body_start += 4;

        int deleted = 0, failed = 0;
        char *files_ptr = body_start ? strstr(body_start, "\"files\":") : nullptr;
        if (files_ptr) {
            files_ptr = strchr(files_ptr, '[');
            while (files_ptr && *files_ptr && *files_ptr != ']') {
                char *path_start = strchr(files_ptr, '"');
                if (!path_start) break;
                path_start++;
                char *path_end = strchr(path_start, '"');
                if (!path_end) break;
                
                std::string file_path(path_start, path_end);
                if (sftp_delete_file(sftp, file_path.c_str()) || sftp_delete_dir(sftp, file_path.c_str())) {
                    deleted++;
                } else {
                    failed++;
                }
                files_ptr = path_end + 1;
            }
        }

        char response_body[256];
        snprintf(response_body, sizeof(response_body), "{\"deleted\":%d,\"failed\":%d}", deleted, failed);
        std::string response = http_response_headers(200, "application/json", strlen(response_body));
        response += response_body;
        send(client_socket, response.c_str(), response.length(), 0);
        SDL_Log("[Tunnel %d] Delete: %d ok, %d failed", tunnel_id, deleted, failed);
    }
    else if (strcmp(method, "POST") == 0) {
        char dir_path[1024] = "/";
        if (strlen(path) > 1) {
            strncpy(dir_path, path, sizeof(dir_path) - 1);
            dir_path[sizeof(dir_path) - 1] = '\0';
        }

        char *content_type_line = strstr(buffer, "Content-Type: multipart/form-data; boundary=");
        if (!content_type_line) {
            std::string response = http_response_headers(400, "text/plain", 18);
            response += "Invalid request";
            send(client_socket, response.c_str(), response.length(), 0);
            SDL_Log("[Tunnel %d] No multipart boundary", tunnel_id);
        } else {
            content_type_line += strlen("Content-Type: multipart/form-data; boundary=");
            char boundary[256];
            int i = 0;
            while (i < 255 && content_type_line[i] && content_type_line[i] != '\r' && content_type_line[i] != '\n') {
                boundary[i] = content_type_line[i];
                i++;
            }
            boundary[i] = '\0';

            int uploaded = 0, failed = 0;
            char search_str[512];
            snprintf(search_str, sizeof(search_str), "--%s", boundary);
            char *part = strstr(buffer, search_str);
            
            while (part) {
                char *filename_start = strstr(part, "filename=\"");
                if (!filename_start) break;
                filename_start += strlen("filename=\"");
                char *filename_end = strchr(filename_start, '"');
                if (!filename_end) break;
                
                std::string filename(filename_start, filename_end);

                char *file_data_start = strstr(filename_end, "\r\n\r\n");
                if (!file_data_start) {
                    file_data_start = strstr(filename_end, "\n\n");
                    if (!file_data_start) {
                        part = strstr(part + 1, search_str);
                        continue;
                    }
                    file_data_start += 2;
                } else {
                    file_data_start += 4;
                }

                snprintf(search_str, sizeof(search_str), "\r\n--%s", boundary);
                char *file_data_end = strstr(file_data_start, search_str);
                if (!file_data_end) {
                    snprintf(search_str, sizeof(search_str), "\n--%s", boundary);
                    file_data_end = strstr(file_data_start, search_str);
                }
                if (!file_data_end) {
                    snprintf(search_str, sizeof(search_str), "--%s--", boundary);
                    file_data_end = strstr(file_data_start, search_str);
                }
                if (!file_data_end) break;

                size_t file_size = file_data_end - file_data_start;
                while (file_size > 0 && (file_data_start[file_size - 1] == '\r' || file_data_start[file_size - 1] == '\n')) {
                    file_size--;
                }

                std::string remote_path = dir_path;
                if (!remote_path.empty() && remote_path.back() != '/') {
                    remote_path += '/';
                }
                remote_path += filename;

                if (sftp_put_file(sftp, remote_path.c_str(), (const uint8_t *)file_data_start, file_size)) {
                    uploaded++;
                    SDL_Log("[Tunnel %d] Uploaded: %s (%zu bytes)", tunnel_id, remote_path.c_str(), file_size);
                } else {
                    failed++;
                }

                snprintf(search_str, sizeof(search_str), "--%s", boundary);
                part = strstr(file_data_end, search_str);
            }

            char response_body[256];
            snprintf(response_body, sizeof(response_body), "{\"uploaded\":%d,\"failed\":%d}", uploaded, failed);
            std::string response = http_response_headers(200, "application/json", strlen(response_body));
            response += response_body;
            send(client_socket, response.c_str(), response.length(), 0);
            SDL_Log("[Tunnel %d] Upload: %d ok, %d failed", tunnel_id, uploaded, failed);
        }
    }
    else {
        std::string response = http_response_headers(400, "text/plain", 18);
        response += "Method not allowed";
        send(client_socket, response.c_str(), response.length(), 0);
        SDL_Log("[Tunnel %d] Method not allowed: %s", tunnel_id, method);
    }

    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(sess, 0);
    ssh_session_unlock();

    closesocket(client_socket);
    SDL_Log("[Tunnel %d] Closed", tunnel_id);
}

// ============================================================================
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

    g_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen_socket == INVALID_SOCKET) {
        SDL_Log("[WebServer] socket() failed: %s", strerror(errno));
        return;
    }

    int reuse = 1;
    setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53716);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(g_listen_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SDL_Log("[WebServer] bind() failed: %s", strerror(errno));
        closesocket(g_listen_socket);
        return;
    }

    if (listen(g_listen_socket, 5) < 0) {
        SDL_Log("[WebServer] listen() failed: %s", strerror(errno));
        closesocket(g_listen_socket);
        return;
    }

#ifndef _WIN32
    int flags = fcntl(g_listen_socket, F_GETFL, 0);
    fcntl(g_listen_socket, F_SETFL, flags | O_NONBLOCK);
#else
    u_long mode = 1;
    ioctlsocket(g_listen_socket, FIONBIO, &mode);
#endif

    g_webserver_running.store(true);
    SDL_Log("[WebServer] Listening on localhost:53716");

    while (!g_webserver_should_exit.load()) {
        struct sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        socklen_t addr_len = sizeof(client_addr);

        int client_socket = accept(g_listen_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket == INVALID_SOCKET) {
            SDL_Delay(10);
            continue;
        }

        int tunnel_id;
        SDL_LockMutex(g_tunnel_mutex);
        tunnel_id = ++g_tunnel_counter;
        SDL_UnlockMutex(g_tunnel_mutex);

        SDL_Log("[Tunnel %d] Opened", tunnel_id);
        handle_http_request(client_socket, tunnel_id);
    }

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
