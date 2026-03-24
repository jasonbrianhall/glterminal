// port_forward.cpp — SSH local and remote port forwarding for Felix Terminal
// Compiled only when USESSH is defined.

#ifdef USESSH

#include "port_forward.h"
#include "ssh_session.h"

#include <libssh2.h>
#include <SDL2/SDL.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   static void sock_close(int fd) { close(fd); }
   static void sock_set_nonblock(int fd) {
       int fl = fcntl(fd, F_GETFL, 0);
       fcntl(fd, F_SETFL, fl | O_NONBLOCK);
   }
   static int  sock_errno() { return errno; }
   static bool sock_would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
#else
#  include <winsock2.h>
#  include <ws2tcpip.h>
   static void sock_close(int fd) { closesocket(fd); }
   static void sock_set_nonblock(int fd) {
       u_long mode = 1;
       ioctlsocket(fd, FIONBIO, &mode);
   }
   static int  sock_errno() { return WSAGetLastError(); }
   static bool sock_would_block(int e) { return e == WSAEWOULDBLOCK; }
#endif

// ============================================================================
// INTERNAL TYPES
// ============================================================================

// One bidirectional tunnel: a local socket ↔ an SSH channel.
struct Tunnel {
    int                  local_fd   = -1;
    LIBSSH2_CHANNEL     *channel    = nullptr;
    std::atomic<bool>    stop{false};
    std::thread          thread;
};

struct LocalForward {
    int         local_port  = 0;
    std::string remote_host;
    int         remote_port = 0;

    int              listen_fd = -1;   // bound listener socket
    bool             listen_ok = false;
    std::atomic<bool> stop{false};
    std::thread      accept_thread;

    // Live tunnels for this forward
    std::mutex              tunnels_mtx;
    std::vector<Tunnel*>    tunnels;
};

struct RemoteForward {
    int         remote_port = 0;
    std::string local_host;
    int         local_port  = 0;

    LIBSSH2_LISTENER    *listener  = nullptr;
    bool                 listen_ok = false;
    std::atomic<bool>    stop{false};
    std::thread          accept_thread;

    std::mutex              tunnels_mtx;
    std::vector<Tunnel*>    tunnels;
};

// ============================================================================
// STATE
// ============================================================================

static std::mutex                       s_fwd_mtx;
static std::vector<LocalForward*>       s_locals;
static std::vector<RemoteForward*>      s_remotes;

// ============================================================================
// HELPERS
// ============================================================================

// Create a bound, listening TCP socket on localhost:port.
// Returns fd >= 0 on success, -1 on failure.
static int make_listen_socket(int port) {
    struct addrinfo hints = {}, *res = nullptr;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    hints.ai_family   = AF_INET;   // IPv4 only for simplicity; v6 can be added
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo("127.0.0.1", port_str, &hints, &res) != 0) {
        SDL_Log("[PF] getaddrinfo failed for port %d\n", port);
        return -1;
    }

    int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    if (bind(fd, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(fd, 16) != 0) {
        SDL_Log("[PF] bind/listen failed on port %d\n", port);
        sock_close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    sock_set_nonblock(fd);
    return fd;
}

// Connect a local TCP socket to host:port (blocking).
// Returns fd >= 0 on success, -1 on failure.
static int tcp_connect_local(const std::string &host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = (int)socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, (int)r->ai_addrlen) == 0) break;
        sock_close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// ============================================================================
// TUNNEL PUMP
//
// Runs on a background thread.  Shuttles bytes between local_fd and the SSH
// channel until EOF, error, or stop is requested.
//
// The session mutex must be held for libssh2 channel calls.  We release it
// between iterations so ssh_read / ssh_write are not starved.
// ============================================================================

static void tunnel_pump(Tunnel *t) {
    char buf[16384];

    // Make local socket non-blocking so we can interleave both directions.
    sock_set_nonblock(t->local_fd);

    while (!t->stop.load()) {
        bool did_work = false;

        // --- local socket → SSH channel ---
        ssize_t n = recv(t->local_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            ssh_session_lock();
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t rc = libssh2_channel_write(t->channel,
                                                   buf + sent,
                                                   (size_t)(n - sent));
                if (rc > 0) { sent += rc; }
                else if (rc == LIBSSH2_ERROR_EAGAIN) { SDL_Delay(1); }
                else { t->stop.store(true); break; }
            }
            ssh_session_unlock();
            did_work = true;
        } else if (n == 0) {
            break;  // local side closed
        } else {
            if (!sock_would_block(sock_errno())) break;
        }

        // --- SSH channel → local socket ---
        ssh_session_lock();
        ssize_t cn = libssh2_channel_read(t->channel, buf, sizeof(buf));
        ssh_session_unlock();

        if (cn > 0) {
            ssize_t sent = 0;
            while (sent < cn) {
                ssize_t rc = send(t->local_fd, buf + sent, (size_t)(cn - sent), 0);
                if (rc > 0) { sent += rc; }
                else if (sock_would_block(sock_errno())) { SDL_Delay(1); }
                else { t->stop.store(true); break; }
            }
            did_work = true;
        } else if (cn != LIBSSH2_ERROR_EAGAIN) {
            break;  // channel EOF or error
        }

        // Check remote EOF
        ssh_session_lock();
        bool eof = libssh2_channel_eof(t->channel) != 0;
        ssh_session_unlock();
        if (eof) break;

        if (!did_work) SDL_Delay(5);
    }

    // Teardown
    sock_close(t->local_fd);
    t->local_fd = -1;

    ssh_session_lock();
    libssh2_channel_send_eof(t->channel);
    libssh2_channel_wait_eof(t->channel);
    libssh2_channel_close(t->channel);
    libssh2_channel_wait_closed(t->channel);
    libssh2_channel_free(t->channel);
    t->channel = nullptr;
    ssh_session_unlock();

    SDL_Log("[PF] tunnel pump exited\n");
}

// Reap finished tunnels from a vector, joining their threads.
static void reap_tunnels(std::vector<Tunnel*> &vec, std::mutex &mtx) {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = vec.begin();
    while (it != vec.end()) {
        Tunnel *t = *it;
        if (t->stop.load()) {
            if (t->thread.joinable()) t->thread.join();
            delete t;
            it = vec.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// LOCAL FORWARDING  (-L)
// ============================================================================

static void local_accept_loop(LocalForward *lf) {
    SDL_Log("[PF-L] listening on localhost:%d → %s:%d\n",
            lf->local_port, lf->remote_host.c_str(), lf->remote_port);

    while (!lf->stop.load()) {
        // Reap finished tunnels periodically
        reap_tunnels(lf->tunnels, lf->tunnels_mtx);

        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int client_fd = (int)accept(lf->listen_fd, (struct sockaddr*)&addr, &addrlen);
        if (client_fd < 0) {
            if (!sock_would_block(sock_errno())) {
                SDL_Log("[PF-L] accept error on port %d\n", lf->local_port);
                break;
            }
            SDL_Delay(10);
            continue;
        }

        SDL_Log("[PF-L] accepted connection on port %d, opening SSH tunnel\n", lf->local_port);

        // Open SSH direct-tcpip channel for this connection
        ssh_session_lock();
        LIBSSH2_CHANNEL *ch = nullptr;
        int attempts = 0;
        while (attempts++ < 200) {  // up to 1 second
            ch = libssh2_channel_direct_tcpip_ex(
                     ssh_get_session(),
                     lf->remote_host.c_str(), lf->remote_port,
                     "127.0.0.1", lf->local_port);
            if (ch) break;
            if (libssh2_session_last_errno(ssh_get_session()) != LIBSSH2_ERROR_EAGAIN) break;
            ssh_session_unlock();
            SDL_Delay(5);
            ssh_session_lock();
        }
        ssh_session_unlock();

        if (!ch) {
            SDL_Log("[PF-L] failed to open SSH channel for port %d\n", lf->local_port);
            sock_close(client_fd);
            continue;
        }

        // Set channel non-blocking
        ssh_session_lock();
        libssh2_channel_set_blocking(ch, 0);
        ssh_session_unlock();

        Tunnel *t = new Tunnel();
        t->local_fd = client_fd;
        t->channel  = ch;
        t->thread   = std::thread(tunnel_pump, t);

        {
            std::lock_guard<std::mutex> lk(lf->tunnels_mtx);
            lf->tunnels.push_back(t);
        }
    }

    SDL_Log("[PF-L] accept loop exiting for port %d\n", lf->local_port);
}

// ============================================================================
// REMOTE FORWARDING  (-R)
// ============================================================================

static void remote_accept_loop(RemoteForward *rf) {
    SDL_Log("[PF-R] waiting for remote connections on server port %d\n", rf->remote_port);

    while (!rf->stop.load()) {
        reap_tunnels(rf->tunnels, rf->tunnels_mtx);

        // Poll for an incoming connection from the server
        ssh_session_lock();
        LIBSSH2_CHANNEL *ch = libssh2_channel_forward_accept(rf->listener);
        ssh_session_unlock();

        if (!ch) {
            SDL_Delay(20);
            continue;
        }

        SDL_Log("[PF-R] accepted remote connection, connecting locally to %s:%d\n",
                rf->local_host.c_str(), rf->local_port);

        int local_fd = tcp_connect_local(rf->local_host, rf->local_port);
        if (local_fd < 0) {
            SDL_Log("[PF-R] failed to connect to local %s:%d\n",
                    rf->local_host.c_str(), rf->local_port);
            ssh_session_lock();
            libssh2_channel_free(ch);
            ssh_session_unlock();
            continue;
        }

        ssh_session_lock();
        libssh2_channel_set_blocking(ch, 0);
        ssh_session_unlock();

        Tunnel *t = new Tunnel();
        t->local_fd = local_fd;
        t->channel  = ch;
        t->thread   = std::thread(tunnel_pump, t);

        {
            std::lock_guard<std::mutex> lk(rf->tunnels_mtx);
            rf->tunnels.push_back(t);
        }
    }

    SDL_Log("[PF-R] remote accept loop exiting for port %d\n", rf->remote_port);
}

// ============================================================================
// PUBLIC API
// ============================================================================

bool pf_add_local(int local_port, const std::string &remote_host, int remote_port) {
    LocalForward *lf = new LocalForward();
    lf->local_port  = local_port;
    lf->remote_host = remote_host;
    lf->remote_port = remote_port;

    lf->listen_fd = make_listen_socket(local_port);
    if (lf->listen_fd < 0) {
        SDL_Log("[PF-L] failed to bind localhost:%d\n", local_port);
        delete lf;
        return false;
    }
    lf->listen_ok = true;
    lf->accept_thread = std::thread(local_accept_loop, lf);

    std::lock_guard<std::mutex> lk(s_fwd_mtx);
    s_locals.push_back(lf);
    SDL_Log("[PF-L] local forward registered: localhost:%d → %s:%d\n",
            local_port, remote_host.c_str(), remote_port);
    return true;
}

bool pf_add_remote(int remote_port, const std::string &local_host, int local_port) {
    RemoteForward *rf = new RemoteForward();
    rf->remote_port = remote_port;
    rf->local_host  = local_host;
    rf->local_port  = local_port;

    // Ask the server to bind the remote port
    ssh_session_lock();
    int bound_port = remote_port;
    LIBSSH2_LISTENER *listener = nullptr;
    int attempts = 0;
    while (attempts++ < 200) {
        listener = libssh2_channel_forward_listen_ex(
                       ssh_get_session(),
                       nullptr,   // "" = server binds on all interfaces
                       remote_port,
                       &bound_port,
                       16);
        if (listener) break;
        if (libssh2_session_last_errno(ssh_get_session()) != LIBSSH2_ERROR_EAGAIN) break;
        ssh_session_unlock();
        SDL_Delay(5);
        ssh_session_lock();
    }
    ssh_session_unlock();

    if (!listener) {
        SDL_Log("[PF-R] server rejected remote forward on port %d\n", remote_port);
        delete rf;
        return false;
    }

    rf->listener  = listener;
    rf->listen_ok = true;
    rf->accept_thread = std::thread(remote_accept_loop, rf);

    std::lock_guard<std::mutex> lk(s_fwd_mtx);
    s_remotes.push_back(rf);
    SDL_Log("[PF-R] remote forward registered: server:%d → %s:%d\n",
            bound_port, local_host.c_str(), local_port);
    return true;
}

void pf_shutdown_all() {
    SDL_Log("[PF] shutting down all port forwards\n");

    // Stop all local forwards
    {
        std::lock_guard<std::mutex> lk(s_fwd_mtx);
        for (LocalForward *lf : s_locals) {
            lf->stop.store(true);
            if (lf->listen_fd >= 0) { sock_close(lf->listen_fd); lf->listen_fd = -1; }
        }
    }
    {
        std::lock_guard<std::mutex> lk(s_fwd_mtx);
        for (LocalForward *lf : s_locals) {
            if (lf->accept_thread.joinable()) lf->accept_thread.join();
            // Stop and join all tunnels
            {
                std::lock_guard<std::mutex> tlk(lf->tunnels_mtx);
                for (Tunnel *t : lf->tunnels) {
                    t->stop.store(true);
                    if (t->local_fd >= 0) { sock_close(t->local_fd); t->local_fd = -1; }
                }
            }
            {
                std::lock_guard<std::mutex> tlk(lf->tunnels_mtx);
                for (Tunnel *t : lf->tunnels) {
                    if (t->thread.joinable()) t->thread.join();
                    delete t;
                }
                lf->tunnels.clear();
            }
            delete lf;
        }
        s_locals.clear();
    }

    // Stop all remote forwards
    {
        std::lock_guard<std::mutex> lk(s_fwd_mtx);
        for (RemoteForward *rf : s_remotes) {
            rf->stop.store(true);
            if (rf->listener) {
                ssh_session_lock();
                libssh2_channel_forward_cancel(rf->listener);
                ssh_session_unlock();
                rf->listener = nullptr;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(s_fwd_mtx);
        for (RemoteForward *rf : s_remotes) {
            if (rf->accept_thread.joinable()) rf->accept_thread.join();
            {
                std::lock_guard<std::mutex> tlk(rf->tunnels_mtx);
                for (Tunnel *t : rf->tunnels) {
                    t->stop.store(true);
                    if (t->local_fd >= 0) { sock_close(t->local_fd); t->local_fd = -1; }
                }
            }
            {
                std::lock_guard<std::mutex> tlk(rf->tunnels_mtx);
                for (Tunnel *t : rf->tunnels) {
                    if (t->thread.joinable()) t->thread.join();
                    delete t;
                }
                rf->tunnels.clear();
            }
            delete rf;
        }
        s_remotes.clear();
    }

    SDL_Log("[PF] all port forwards stopped\n");
}

bool pf_parse_spec(const std::string &spec,
                   int *out_lport, std::string *out_rhost, int *out_rport) {
    // Format: "local_port:remote_host:remote_port"
    // remote_host may be an IPv6 literal like [::1], handled by finding the
    // last ':' for remote_port.
    size_t first_colon = spec.find(':');
    if (first_colon == std::string::npos) return false;

    size_t last_colon = spec.rfind(':');
    if (last_colon == first_colon) return false;  // need at least two colons

    std::string lport_str  = spec.substr(0, first_colon);
    std::string rhost      = spec.substr(first_colon + 1, last_colon - first_colon - 1);
    std::string rport_str  = spec.substr(last_colon + 1);

    if (lport_str.empty() || rhost.empty() || rport_str.empty()) return false;

    int lp = atoi(lport_str.c_str());
    int rp = atoi(rport_str.c_str());
    if (lp <= 0 || lp > 65535 || rp <= 0 || rp > 65535) return false;

    *out_lport = lp;
    *out_rhost = rhost;
    *out_rport = rp;
    return true;
}

std::vector<PfStatus> pf_status() {
    std::vector<PfStatus> out;
    std::lock_guard<std::mutex> lk(s_fwd_mtx);

    for (LocalForward *lf : s_locals) {
        PfStatus s;
        s.type        = PfStatus::LOCAL;
        s.local_port  = lf->local_port;
        s.remote_host = lf->remote_host;
        s.remote_port = lf->remote_port;
        s.listener_ok = lf->listen_ok;
        {
            std::lock_guard<std::mutex> tlk(lf->tunnels_mtx);
            s.active_connections = (int)lf->tunnels.size();
        }
        out.push_back(s);
    }
    for (RemoteForward *rf : s_remotes) {
        PfStatus s;
        s.type        = PfStatus::REMOTE;
        s.local_port  = rf->local_port;
        s.remote_host = rf->local_host;
        s.remote_port = rf->remote_port;
        s.listener_ok = rf->listen_ok;
        {
            std::lock_guard<std::mutex> tlk(rf->tunnels_mtx);
            s.active_connections = (int)rf->tunnels.size();
        }
        out.push_back(s);
    }
    return out;
}

#endif // USESSH
