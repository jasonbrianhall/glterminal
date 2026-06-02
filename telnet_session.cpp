// telnet_session.cpp — libtelnet Telnet session / raw TCP for gl_terminal

#include "telnet_session.h"
#include "terminal.h"
#include "term_pty.h"   // term_feed, g_term_write_override
#include "libtelnet.h"

#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#else
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
#endif

// ============================================================================
// STATE
// ============================================================================

static telnet_t *s_telnet   = nullptr;
static int       s_sock     = -1;
static bool      s_active   = false;
static bool      s_closed   = false;
static bool      s_raw_mode = false;  // true = bypass libtelnet entirely

// Terminal dimensions — kept in sync so NAWS sub-neg can read them.
static int s_cols = 80;
static int s_rows = 24;

// Stored terminal type for TTYPE sub-negotiation response.
static std::string s_ttype = "xterm-256color";

// Terminal pointer used inside the libtelnet callback (set before any I/O).
static Terminal *s_term = nullptr;

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

// Resolve hostname and return a connected, non-blocking TCP socket, or -1.
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

    SDL_Log("[TELNET] connected to %s:%d (fd=%d)\n", host.c_str(), port, fd);
    return fd;
}

// Send NAWS (window size) sub-negotiation for the current dimensions.
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
// LIBTELNET CALLBACK  (not used in raw mode)
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
            int         rem = (int)ev->data.size;
            while (rem > 0) {
                int n = (int)send(s_sock, ptr, (size_t)rem, 0);
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
            SDL_Log("[TELNET] server DO NAWS — sending %dx%d\n", s_cols, s_rows);
            send_naws(s_cols, s_rows);
            break;
        case TELNET_TELOPT_TTYPE:
            SDL_Log("[TELNET] server DO TTYPE\n");
            break;
        default:
            break;
        }
        break;

    case TELNET_EV_SUBNEGOTIATION:
        if (ev->sub.telopt == TELNET_TELOPT_TTYPE) {
            if (ev->sub.size >= 1 && (unsigned char)ev->sub.buffer[0] == 1) {
                SDL_Log("[TELNET] server SB TTYPE SEND — replying '%s'\n", s_ttype.c_str());
                std::string reply = std::string("\x00", 1) + s_ttype;
                telnet_subnegotiation(telnet, TELNET_TELOPT_TTYPE,
                                      reply.c_str(), (size_t)reply.size());
            }
        }
        break;

    case TELNET_EV_ERROR:
        SDL_Log("[TELNET] error: %s\n", ev->error.msg ? ev->error.msg : "(unknown)");
        break;

    default:
        break;
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
    // Raw mode: explicit flag, or any non-23 port that isn't a known Telnet port
    s_raw_mode = cfg.raw_mode || cfg.port != 23;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    s_sock = tcp_connect(cfg.host, cfg.port);
    if (s_sock < 0)
        return false;

    if (!s_raw_mode) {
        // Full Telnet negotiation via libtelnet
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
        SDL_Log("[TELNET] mode=telnet %s:%d ttype=%s\n",
                cfg.host.c_str(), cfg.port, cfg.ttype.c_str());
    } else {
        // Raw TCP — no Telnet negotiation at all
        s_telnet = nullptr;
        SDL_Log("[TELNET] mode=raw %s:%d\n", cfg.host.c_str(), cfg.port);
    }

    g_term_write_override = telnet_write_bridge;
    s_active = true;
    return true;
}

bool telnet_read(Terminal *t) {
    if (s_sock < 0) return false;

    s_term = t;

    char buf[4096];
    bool got_data = false;

    for (;;) {
#ifndef _WIN32
        ssize_t n = recv(s_sock, buf, sizeof(buf), MSG_DONTWAIT);
#else
        int n = recv(s_sock, buf, (int)sizeof(buf), 0);
#endif
        if (n > 0) {
            if (s_raw_mode) {
                // Bypass libtelnet — feed bytes directly into the VT parser
                term_feed(t, buf, (int)n);
            } else {
                telnet_recv(s_telnet, buf, (size_t)n);
            }
            got_data = true;
        } else if (n == 0) {
            s_closed = true;  // keep s_active/socket open so writes still work
            break;
        } else {
#ifndef _WIN32
            if (errno == EAGAIN || errno == EWOULDBLOCK)
#else
            if (WSAGetLastError() == WSAEWOULDBLOCK)
#endif
                break;
            SDL_Log("[TELNET] recv error: %s\n", strerror(errno));
            s_closed = true;
            break;
        }
    }
    return got_data;
}

void telnet_write(Terminal *t, const char *buf, int n) {
    if (s_sock < 0 || n <= 0) return;
    if (s_raw_mode) {
        // Local echo — no PTY means no server echo in raw TCP mode.
        // Only echo printable bytes and CR; suppress escape sequences.
        if (t && n >= 1) {
            unsigned char first = (unsigned char)buf[0];
            if (first == '\r') {
                term_feed(t, "\r\n", 2);   // Enter: show as newline
            } else if (first >= 0x20 && first < 0x7F) {
                term_feed(t, buf, n);      // printable ASCII — echo as-is
            } else if (first == 0x08 || first == 0x7F) {
                term_feed(t, "\b \b", 3);  // backspace
            }
            // Suppress ESC sequences, control chars etc.
        }
        const char *ptr = buf;
        int rem = n;
        // HTTP and most raw TCP protocols expect \r\n line endings.
        // handle_key sends bare \r for Enter — upgrade it here.
        char crlf[2] = { '\r', '\n' };
        if (n == 1 && buf[0] == '\r') {
            ptr = crlf;
            rem = 2;
        }
        while (rem > 0) {
            int sent = (int)send(s_sock, ptr, (size_t)rem, 0);
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
    if (!s_active || s_raw_mode || !s_telnet) return;
    send_naws(cols, rows);
}

bool telnet_active()         { return s_active; }
bool telnet_channel_closed() { return s_closed; }

void telnet_disconnect() {
    g_term_write_override = nullptr;

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
    s_term     = nullptr;

#ifdef _WIN32
    WSACleanup();
#endif
    SDL_Log("[TELNET] disconnected\n");
}
