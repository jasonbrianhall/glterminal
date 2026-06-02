// telnet_session.cpp — libtelnet Telnet / raw TCP / SSL session for gl_terminal

#include "telnet_session.h"
#include "terminal.h"
#include "term_pty.h"
#include "libtelnet.h"

#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string>

#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#else
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
#endif

// ============================================================================
// STATE
// ============================================================================

static telnet_t  *s_telnet   = nullptr;
static int        s_sock     = -1;
static bool       s_active   = false;
static bool       s_closed   = false;
static bool       s_raw_mode = false;
static bool       s_use_ssl  = false;

static SSL_CTX   *s_ssl_ctx  = nullptr;
static SSL       *s_ssl      = nullptr;

static int  s_cols = 80;
static int  s_rows = 24;
static std::string s_ttype   = "xterm-256color";
static Terminal   *s_term    = nullptr;

// ============================================================================
// HELPERS
// ============================================================================

static void sock_close(int fd) {
#ifndef _WIN32
    close(fd);
#else
    closesocket(fd);
#endif
}

static int tcp_connect(const std::string &host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        SDL_Log("[TELNET] getaddrinfo failed for host '%s'\n", host.c_str());
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = (int)socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, (int)r->ai_addrlen) == 0)
            break;
        sock_close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        SDL_Log("[TELNET] connect to %s:%d failed\n", host.c_str(), port);
        return -1;
    }

    // Set non-blocking
#ifndef _WIN32
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#else
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#endif

    SDL_Log("[TELNET] TCP connected to %s:%d\n", host.c_str(), port);
    return fd;
}

// Perform blocking SSL handshake on a non-blocking socket using poll/select.
static bool ssl_connect(const std::string &host) {
    s_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!s_ssl_ctx) {
        SDL_Log("[SSL] SSL_CTX_new failed\n");
        return false;
    }

    // Use system CA bundle for certificate verification
    SSL_CTX_set_default_verify_paths(s_ssl_ctx);
    SSL_CTX_set_verify(s_ssl_ctx, SSL_VERIFY_PEER, nullptr);

    s_ssl = SSL_new(s_ssl_ctx);
    if (!s_ssl) {
        SDL_Log("[SSL] SSL_new failed\n");
        return false;
    }

    SSL_set_fd(s_ssl, s_sock);

    // SNI — required by most modern HTTPS servers
    SSL_set_tlsext_host_name(s_ssl, host.c_str());

    // Handshake loop — socket is non-blocking so we retry on WANT_READ/WANT_WRITE
    while (true) {
        int ret = SSL_connect(s_ssl);
        if (ret == 1) break;  // success

        int err = SSL_get_error(s_ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            SDL_Delay(5);
            continue;
        }
        // Real error
        char errbuf[256];
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        SDL_Log("[SSL] handshake failed: %s\n", errbuf);
        return false;
    }

    SDL_Log("[SSL] handshake complete — %s\n", SSL_get_cipher(s_ssl));
    return true;
}

// Wrappers that route through SSL or raw socket transparently
static int net_send(const char *buf, int n) {
    if (s_use_ssl && s_ssl)
        return SSL_write(s_ssl, buf, n);
    return (int)send(s_sock, buf, (size_t)n, 0);
}

static int net_recv(char *buf, int n) {
    if (s_use_ssl && s_ssl)
        return SSL_read(s_ssl, buf, n);
#ifndef _WIN32
    return (int)recv(s_sock, buf, (size_t)n, MSG_DONTWAIT);
#else
    return (int)recv(s_sock, buf, n, 0);
#endif
}

// Returns true if the net_recv would block (no data yet)
static bool net_would_block(int ret) {
    if (s_use_ssl && s_ssl) {
        int err = SSL_get_error(s_ssl, ret);
        return err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
    }
#ifndef _WIN32
    return errno == EAGAIN || errno == EWOULDBLOCK;
#else
    return WSAGetLastError() == WSAEWOULDBLOCK;
#endif
}

static void send_naws(int cols, int rows) {
    if (!s_telnet) return;
    unsigned char naws[4] = {
        (unsigned char)((cols >> 8) & 0xFF),
        (unsigned char)( cols       & 0xFF),
        (unsigned char)((rows >> 8) & 0xFF),
        (unsigned char)( rows       & 0xFF)
    };
    telnet_subnegotiation(s_telnet, TELNET_TELOPT_NAWS, (const char *)naws, 4);
}

// ============================================================================
// LIBTELNET CALLBACK
// ============================================================================

static void telnet_event_handler(telnet_t *telnet,
                                 telnet_event_t *ev,
                                 void *user_data)
{
    (void)user_data;
    switch (ev->type) {
    case TELNET_EV_SEND:
        if (ev->data.size > 0 && s_sock >= 0) {
            const char *ptr = ev->data.buffer;
            int rem = (int)ev->data.size;
            while (rem > 0) {
                int n = net_send(ptr, rem);
                if (n > 0) { ptr += n; rem -= n; }
                else break;
            }
        }
        break;
    case TELNET_EV_DATA:
        if (ev->data.size > 0 && s_term)
            term_feed(s_term, ev->data.buffer, (int)ev->data.size);
        break;
    case TELNET_EV_WILL:
        if (ev->neg.telopt == TELNET_TELOPT_ECHO)
            SDL_Log("[TELNET] server WILL ECHO\n");
        break;
    case TELNET_EV_DO:
        switch (ev->neg.telopt) {
        case TELNET_TELOPT_NAWS:
            send_naws(s_cols, s_rows);
            break;
        case TELNET_TELOPT_TTYPE: break;
        default: break;
        }
        break;
    case TELNET_EV_SUBNEGOTIATION:
        if (ev->sub.telopt == TELNET_TELOPT_TTYPE &&
            ev->sub.size >= 1 && (unsigned char)ev->sub.buffer[0] == 1) {
            std::string reply = std::string("\x00", 1) + s_ttype;
            telnet_subnegotiation(telnet, TELNET_TELOPT_TTYPE,
                                  reply.c_str(), reply.size());
        }
        break;
    case TELNET_EV_ERROR:
        SDL_Log("[TELNET] error: %s\n", ev->error.msg ? ev->error.msg : "(unknown)");
        break;
    default: break;
    }
}

// ============================================================================
// WRITE BRIDGE
// ============================================================================

static void telnet_write_bridge(Terminal *t, const char *buf, int n) {
    telnet_write(t, buf, n);
}

// ============================================================================
// PUBLIC API
// ============================================================================

bool telnet_connect(const TelnetConfig &cfg, Terminal *t) {
    s_ttype    = cfg.ttype;
    s_cols     = t->cols;
    s_rows     = t->rows;
    s_term     = t;
    s_closed   = false;
    s_raw_mode = cfg.raw_mode || cfg.port != 23;
    s_use_ssl  = cfg.use_ssl  || cfg.port == 443;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    s_sock = tcp_connect(cfg.host, cfg.port);
    if (s_sock < 0)
        return false;

    if (s_use_ssl) {
        if (!ssl_connect(cfg.host)) {
            sock_close(s_sock);
            s_sock = -1;
            return false;
        }
        SDL_Log("[TELNET] mode=ssl %s:%d\n", cfg.host.c_str(), cfg.port);
    } else if (!s_raw_mode) {
        static const telnet_telopt_t telopts[] = {
            { TELNET_TELOPT_BINARY, TELNET_WILL, TELNET_DO   },
            { TELNET_TELOPT_ECHO,   TELNET_WONT, TELNET_DO   },
            { TELNET_TELOPT_SGA,    TELNET_WILL, TELNET_DO   },
            { TELNET_TELOPT_TTYPE,  TELNET_WILL, TELNET_DONT },
            { TELNET_TELOPT_NAWS,   TELNET_WILL, TELNET_DONT },
            { -1, 0, 0 }
        };
        s_telnet = telnet_init(telopts, telnet_event_handler, 0, nullptr);
        if (!s_telnet) {
            SDL_Log("[TELNET] telnet_init failed\n");
            sock_close(s_sock);
            s_sock = -1;
            return false;
        }
        SDL_Log("[TELNET] mode=telnet %s:%d\n", cfg.host.c_str(), cfg.port);
    } else {
        SDL_Log("[TELNET] mode=raw %s:%d\n", cfg.host.c_str(), cfg.port);
    }

    g_term_write_override = telnet_write_bridge;
    s_active = true;
    return true;
}

bool telnet_read(Terminal *t) {
    if (s_sock < 0 || s_closed) return false;

    s_term = t;
    char buf[4096];
    bool got_data = false;

    for (;;) {
        int n = net_recv(buf, sizeof(buf));
        if (n > 0) {
            if (s_raw_mode || s_use_ssl) {
                term_feed(t, buf, n);
            } else {
                telnet_recv(s_telnet, buf, (size_t)n);
            }
            got_data = true;
        } else if (n == 0) {
            if (!s_closed) SDL_Log("[TELNET] server closed connection\n");
            s_closed = true;
            break;
        } else {
            if (net_would_block(n)) break;
            SDL_Log("[TELNET] recv error\n");
            s_closed = true;
            break;
        }
    }
    return got_data;
}

void telnet_write(Terminal *t, const char *buf, int n) {
    if (s_sock < 0 || n <= 0) return;
    if (s_raw_mode || s_use_ssl) {
        // Local echo
        if (t && n >= 1) {
            unsigned char first = (unsigned char)buf[0];
            if (first == '\r') {
                term_feed(t, "\r\n", 2);
            } else if (first >= 0x20 && first < 0x7F) {
                term_feed(t, buf, n);
            } else if (first == 0x08 || first == 0x7F) {
                term_feed(t, "\b \b", 3);
            }
        }
        // Send — upgrade bare \r to \r\n for HTTP
        const char *ptr = buf;
        int rem = n;
        char crlf[2] = { '\r', '\n' };
        if (n == 1 && buf[0] == '\r') { ptr = crlf; rem = 2; }
        while (rem > 0) {
            int sent = net_send(ptr, rem);
            if (sent > 0) { ptr += sent; rem -= sent; }
            else break;
        }
    } else {
        telnet_send(s_telnet, buf, (size_t)n);
    }
}

void telnet_pty_resize(int cols, int rows) {
    s_cols = cols;
    s_rows = rows;
    if (!s_active || s_raw_mode || s_use_ssl || !s_telnet) return;
    send_naws(cols, rows);
}

bool telnet_active()         { return s_active; }
bool telnet_channel_closed() { return s_closed; }

void telnet_disconnect() {
    g_term_write_override = nullptr;

    if (s_ssl) {
        SSL_shutdown(s_ssl);
        SSL_free(s_ssl);
        s_ssl = nullptr;
    }
    if (s_ssl_ctx) {
        SSL_CTX_free(s_ssl_ctx);
        s_ssl_ctx = nullptr;
    }
    if (s_telnet) {
        telnet_free(s_telnet);
        s_telnet = nullptr;
    }
    if (s_sock >= 0) {
        sock_close(s_sock);
        s_sock = -1;
    }
    s_active   = false;
    s_raw_mode = false;
    s_use_ssl  = false;
    s_closed   = false;
    s_term     = nullptr;

#ifdef _WIN32
    WSACleanup();
#endif
    SDL_Log("[TELNET] disconnected\n");
}
