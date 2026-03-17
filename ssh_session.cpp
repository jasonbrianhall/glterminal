// ssh_session.cpp — libssh2 SSH session for gl_terminal
// Compiled only when USESSH is defined.

#ifdef USESSH

#include "ssh_session.h"
#include "terminal.h"
#include "term_pty.h"   // term_feed, g_term_write_override

#include <libssh2.h>

#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <netdb.h>
#  include <arpa/inet.h>
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

static LIBSSH2_SESSION *s_session  = nullptr;
static LIBSSH2_CHANNEL *s_channel  = nullptr;
static int              s_sock     = -1;   // TCP socket fd
static bool             s_active   = false;

// ============================================================================
// HELPERS
// ============================================================================

static std::string last_ssh2_error(const char *context) {
    char *msg = nullptr;
    int   len = 0;
    if (s_session)
        libssh2_session_last_error(s_session, &msg, &len, 0);
    char buf[512];
    if (msg && len > 0)
        snprintf(buf, sizeof(buf), "[SSH] %s: %.*s", context, len, msg);
    else
        snprintf(buf, sizeof(buf), "[SSH] %s: unknown error", context);
    return buf;
}

// Resolve hostname and open a non-blocking TCP socket.
// Returns a valid socket fd or -1 on failure.
static int tcp_connect(const std::string &host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        SDL_Log("[SSH] getaddrinfo failed for host '%s'\n", host.c_str());
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = (int)socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, r->ai_addr, (int)r->ai_addrlen) == 0)
            break;

#ifndef _WIN32
        close(fd);
#else
        closesocket(fd);
#endif
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        SDL_Log("[SSH] connect to %s:%d failed\n", host.c_str(), port);
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

    return fd;
}

// Attempt authentication via ssh-agent.
static bool auth_agent(const std::string &user) {
    LIBSSH2_AGENT *agent = libssh2_agent_init(s_session);
    if (!agent) return false;

    if (libssh2_agent_connect(agent) != 0) {
        libssh2_agent_free(agent);
        return false;
    }
    if (libssh2_agent_list_identities(agent) != 0) {
        libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        return false;
    }

    struct libssh2_agent_publickey *identity = nullptr, *prev = nullptr;
    bool ok = false;
    while (libssh2_agent_get_identity(agent, &identity, prev) == 0) {
        int rc;
        while ((rc = libssh2_agent_userauth(agent, user.c_str(), identity)) ==
               LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);
        if (rc == 0) {
            SDL_Log("[SSH] authenticated via ssh-agent identity '%s'\n",
                    identity->comment ? identity->comment : "(unnamed)");
            ok = true;
            break;
        }
        prev = identity;
    }

    libssh2_agent_disconnect(agent);
    libssh2_agent_free(agent);
    return ok;
}

// Attempt public-key file authentication.
static bool auth_key(const std::string &user,
                     const std::string &priv_path,
                     const std::string &pub_path_in,
                     const std::string &passphrase) {
    std::string pub_path = pub_path_in.empty() ? priv_path + ".pub" : pub_path_in;

    // Check files are accessible before handing off to libssh2
    if (access(priv_path.c_str(), R_OK) != 0)
        SDL_Log("[SSH] key auth: cannot read private key '%s': %s\n",
                priv_path.c_str(), strerror(errno));
    if (access(pub_path.c_str(), R_OK) != 0)
        SDL_Log("[SSH] key auth: cannot read public key '%s': %s\n",
                pub_path.c_str(), strerror(errno));

    SDL_Log("[SSH] attempting key auth: user='%s' pub='%s' priv='%s'\n",
            user.c_str(), pub_path.c_str(), priv_path.c_str());

    int rc;
    while ((rc = libssh2_userauth_publickey_fromfile(
                s_session,
                user.c_str(),
                pub_path.c_str(),
                priv_path.c_str(),
                passphrase.empty() ? nullptr : passphrase.c_str())) ==
           LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);

    if (rc == 0) {
        SDL_Log("[SSH] authenticated via key '%s'\n", priv_path.c_str());
        return true;
    }
    SDL_Log("[SSH] key auth failed (rc=%d): %s\n", rc, last_ssh2_error("key auth").c_str());
    return false;
}

// Attempt password authentication.
static bool auth_password(const std::string &user, const std::string &password) {
    int rc;
    while ((rc = libssh2_userauth_password(
                s_session, user.c_str(), password.c_str())) ==
           LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);

    if (rc == 0) {
        SDL_Log("[SSH] authenticated via password\n");
        return true;
    }
    SDL_Log("%s\n", last_ssh2_error("password auth failed").c_str());
    return false;
}

// Map libssh2 hostkey type integer to the corresponding LIBSSH2_KNOWNHOST_KEY_*
// flag.  The old code used a ternary that fell back to SSHDSS for anything
// non-RSA, which breaks Ed25519 and ECDSA — the key types preferred by all
// modern OpenSSH servers.
static int hostkey_type_to_knownhost_flag(int key_type) {
    switch (key_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_384
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_521
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
    default:
        SDL_Log("[SSH] WARNING: unknown host key type %d — falling back to DSS flag\n",
                key_type);
        return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    }
}

// Verify the server's host key against the known_hosts file.
// Returns true if the key is trusted, false if it should be rejected.
static bool verify_host_key(const std::string &host, int port,
                             const std::string &known_hosts_path) {
    // Build default path if not provided
    std::string kh_path = known_hosts_path;
    if (kh_path.empty()) {
#ifndef _WIN32
        const char *home = getenv("HOME");
        if (home) kh_path = std::string(home) + "/.ssh/known_hosts";
#else
        const char *userprofile = getenv("USERPROFILE");
        if (userprofile) kh_path = std::string(userprofile) + "\\.ssh\\known_hosts";
#endif
    }

    // known_hosts_path == "" after expansion means skip verification (INSECURE)
    if (kh_path.empty()) {
        SDL_Log("[SSH] WARNING: host key verification skipped (no known_hosts path)\n");
        return true;
    }

    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(s_session);
    if (!kh) {
        SDL_Log("[SSH] failed to init known_hosts\n");
        return false;
    }

    // Ignore failure loading the file — it may not exist yet
    libssh2_knownhost_readfile(kh, kh_path.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    size_t key_len = 0;
    int    key_type = 0;
    const char *key = libssh2_session_hostkey(s_session, &key_len, &key_type);
    if (!key) {
        SDL_Log("[SSH] could not get server host key\n");
        libssh2_knownhost_free(kh);
        return false;
    }

    int type_mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN
                  | LIBSSH2_KNOWNHOST_KEYENC_RAW
                  | hostkey_type_to_knownhost_flag(key_type);

    struct libssh2_knownhost *found = nullptr;
    int check = libssh2_knownhost_checkp(
        kh, host.c_str(), port, key, key_len, type_mask, &found);

    bool trusted = false;
    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        trusted = true;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
        // First connection — add the key and trust it
        SDL_Log("[SSH] new host '%s' — adding to known_hosts\n", host.c_str());
        libssh2_knownhost_addc(kh, host.c_str(), nullptr,
                               key, key_len, nullptr, 0, type_mask, nullptr);
        libssh2_knownhost_writefile(kh, kh_path.c_str(),
                                    LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        trusted = true;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        SDL_Log("[SSH] ERROR: host key MISMATCH for '%s' — possible MITM attack!\n",
                host.c_str());
        trusted = false;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_FAILURE:
    default:
        SDL_Log("[SSH] known_hosts check failed for '%s'\n", host.c_str());
        trusted = false;
        break;
    }

    libssh2_knownhost_free(kh);
    return trusted;
}

// Bridge called by term_write() for all output (handle_key, term_paste, etc.)
static void ssh_write_bridge(Terminal *t, const char *s, int n) {
    ssh_write(t, s, n);
}

// ============================================================================
// PUBLIC API
// ============================================================================

bool ssh_connect(const SshConfig &cfg, Terminal *t) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        SDL_Log("[SSH] WSAStartup failed: %d\n", WSAGetLastError());
        return false;
    }
#endif

    // libssh2 global init (idempotent across calls)
    if (libssh2_init(0) != 0) {
        SDL_Log("[SSH] libssh2_init failed\n");
        return false;
    }

    // TCP connection
    s_sock = tcp_connect(cfg.host, cfg.port);
    if (s_sock < 0) return false;

    // Create session (non-blocking)
    s_session = libssh2_session_init();
    if (!s_session) {
        SDL_Log("[SSH] libssh2_session_init failed\n");
        return false;
    }

    // Use non-blocking I/O; we spin with SDL_Delay to avoid busy-wait
    libssh2_session_set_blocking(s_session, 0);

    // Send keepalives every 60 seconds to prevent server-side idle disconnect
    libssh2_keepalive_config(s_session, 1, 60);

    // Perform SSH handshake
    int rc;
    while ((rc = libssh2_session_handshake(s_session, s_sock)) ==
           LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);
    if (rc != 0) {
        SDL_Log("%s\n", last_ssh2_error("handshake failed").c_str());
        return false;
    }

    // Host key verification
    if (!verify_host_key(cfg.host, cfg.port, cfg.known_hosts_path)) {
        SDL_Log("[SSH] host key verification failed — aborting\n");
        return false;
    }

    // Authentication: agent → key file → password (supplied or prompted by caller)
    bool authed = false;

    // Query what the server will accept — must retry on EAGAIN for non-blocking session
    char *auth_methods = nullptr;
    do {
        auth_methods = libssh2_userauth_list(s_session, cfg.user.c_str(),
                                             (unsigned int)cfg.user.size());
        if (!auth_methods && libssh2_session_last_errno(s_session) == LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);
        else
            break;
    } while (true);
    SDL_Log("[SSH] server auth methods: %s\n", auth_methods ? auth_methods : "(none/error)");

    if (!authed) {
        SDL_Log("[SSH] trying agent auth for user '%s'\n", cfg.user.c_str());
        authed = auth_agent(cfg.user);
        if (!authed) SDL_Log("[SSH] agent auth failed or no agent\n");
    }

    if (!authed && !cfg.key_path.empty())
        authed = auth_key(cfg.user, cfg.key_path, cfg.key_path_pub, cfg.password);

    if (!authed) {
        std::string password = cfg.password;
        bool server_allows_password = !auth_methods ||
                                      strstr(auth_methods, "password") != nullptr;
        SDL_Log("[SSH] password auth stage: password=%s prompt_cb=%s server_allows=%s\n",
                password.empty() ? "empty" : "set",
                cfg.prompt_password ? "set" : "NULL",
                server_allows_password ? "yes" : "no");
        if (password.empty() && cfg.prompt_password && server_allows_password) {
            char prompt[256];
            snprintf(prompt, sizeof(prompt), "%s@%s's password: ",
                     cfg.user.c_str(), cfg.host.c_str());
            SDL_Log("[SSH] calling prompt_password callback\n");
            password = cfg.prompt_password(prompt);
            SDL_Log("[SSH] prompt_password returned: %s\n", password.empty() ? "empty" : "got password");
        }
        if (!password.empty())
            authed = auth_password(cfg.user, password);
    }

    if (!authed) {
        SDL_Log("[SSH] authentication failed for user '%s'\n", cfg.user.c_str());
        return false;
    }

    // Open channel
    while (!(s_channel = libssh2_channel_open_session(s_session)))
        if (libssh2_session_last_errno(s_session) != LIBSSH2_ERROR_EAGAIN)
            break;
    if (!s_channel) {
        SDL_Log("%s\n", last_ssh2_error("channel open failed").c_str());
        return false;
    }

    // Request PTY
    while ((rc = libssh2_channel_request_pty_ex(
                s_channel,
                "xterm-kitty", (unsigned int)strlen("xterm-kitty"),
                nullptr, 0,
                (int)t->cols, (int)t->rows,
                0, 0)) == LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);
    if (rc != 0) {
        SDL_Log("%s\n", last_ssh2_error("pty request failed").c_str());
        return false;
    }

    // Start shell
    while ((rc = libssh2_channel_shell(s_channel)) == LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);
    if (rc != 0) {
        SDL_Log("%s\n", last_ssh2_error("shell request failed").c_str());
        return false;
    }

    // Environment hints (best-effort; server may reject setenv)
    libssh2_channel_setenv(s_channel, "COLORTERM", "truecolor");

    // Route all term_write() calls (handle_key, term_paste, etc.) through SSH
    g_term_write_override = ssh_write_bridge;

    s_active = true;
    SDL_Log("[SSH] connected to %s@%s:%d\n", cfg.user.c_str(), cfg.host.c_str(), cfg.port);
    return true;
}

bool ssh_read(Terminal *t) {
    if (!s_active || !s_channel) return false;

    char buf[4096];
    bool got_data = false;

    for (;;) {
        ssize_t n = libssh2_channel_read(s_channel, buf, sizeof(buf));
        if (n > 0) {
            term_feed(t, buf, (int)n);
            got_data = true;
        } else if (n == LIBSSH2_ERROR_EAGAIN) {
            int next;
            libssh2_keepalive_send(s_session, &next);
            break;
        } else {
            // n == 0 (EOF) or error — mark closed
            if (n < 0)
                SDL_Log("%s\n", last_ssh2_error("channel read error").c_str());
            s_active = false;
            break;
        }
    }
    return got_data;
}

void ssh_write(Terminal *t, const char *buf, int n) {
    (void)t;
    if (!s_active || !s_channel || n <= 0) return;

    int sent = 0;
    while (sent < n) {
        ssize_t rc = libssh2_channel_write(s_channel, buf + sent, (size_t)(n - sent));
        if (rc > 0) {
            sent += (int)rc;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            SDL_Delay(1);
        } else {
            SDL_Log("%s\n", last_ssh2_error("channel write error").c_str());
            break;
        }
    }
}

void ssh_pty_resize(int cols, int rows) {
    if (!s_active || !s_channel) return;
    libssh2_channel_request_pty_size(s_channel, cols, rows);
}

bool ssh_channel_closed() {
    if (!s_active || !s_channel) return true;
    return libssh2_channel_eof(s_channel) != 0;
}

void ssh_disconnect() {
    // Restore normal PTY write path before tearing down the channel
    g_term_write_override = nullptr;

    if (s_channel) {
        libssh2_channel_send_eof(s_channel);
        libssh2_channel_wait_eof(s_channel);
        libssh2_channel_close(s_channel);
        libssh2_channel_wait_closed(s_channel);
        libssh2_channel_free(s_channel);
        s_channel = nullptr;
    }
    if (s_session) {
        libssh2_session_disconnect(s_session, "Normal shutdown");
        libssh2_session_free(s_session);
        s_session = nullptr;
    }
    if (s_sock >= 0) {
#ifndef _WIN32
        close(s_sock);
#else
        closesocket(s_sock);
#endif
        s_sock = -1;
    }
    s_active = false;
    libssh2_exit();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool ssh_active() { return s_active; }

#endif // USESSH
