// ssh_session.cpp — libssh2 SSH session for gl_terminal
// Compiled only when USESSH is defined.

#ifdef USESSH

#include "ssh_session.h"
#include "terminal.h"
#include "term_pty.h"   // term_feed, g_term_write_override

#include <libssh2.h>
#include <openssl/evp.h>

#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <string>
#include <mutex>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <sys/stat.h>
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
static bool             s_have_pty = false;

// All libssh2 calls on this session must hold this mutex.
// The SFTP transfer thread acquires it for the duration of a transfer;
// the main thread holds it during ssh_read / ssh_write.
// Must be recursive: ssh_read holds the lock, calls term_feed, which calls
// term_write (for DA responses etc.), which calls ssh_write — same thread,
// needs to re-enter. std::mutex would deadlock; recursive_mutex allows it.
static std::recursive_mutex s_session_mutex;

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

// Numeric IP of the connected peer (for the "'host (IP)'" prompt line).
// Returns "" on failure — caller falls back to just the hostname.
static std::string get_peer_ip() {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(s_sock, (struct sockaddr *)&ss, &len) != 0)
        return "";
    char ipstr[INET6_ADDRSTRLEN] = {};
    if (getnameinfo((struct sockaddr *)&ss, len, ipstr, sizeof(ipstr),
                     nullptr, 0, NI_NUMERICHOST) != 0)
        return "";
    return ipstr;
}

// Human-readable key type name for the trust prompt ("RSA", "ED25519", ...).
static const char *hostkey_type_name(int key_type) {
    switch (key_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return "RSA";
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ECDSA";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_384
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ECDSA";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_521
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ECDSA";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ED25519";
#endif
    default: return "DSA";
    }
}

// OpenSSH-style "SHA256:base64" fingerprint of the server's host key.
static std::string hostkey_fingerprint() {
    const char *hash = libssh2_hostkey_hash(s_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash) return "";
    unsigned char b64[64] = {};
    int outlen = EVP_EncodeBlock(b64, (const unsigned char *)hash, 32);
    std::string s((char *)b64, outlen);
    while (!s.empty() && s.back() == '=')  // OpenSSH fingerprints omit padding
        s.pop_back();
    return "SHA256:" + s;
}

// Verify the server's host key against the known_hosts file.
// Returns true if the key is trusted, false if it should be rejected.
// If the key is unknown and cfg.prompt_host_key is set, interactively asks
// the user whether to trust it (mirroring OpenSSH's first-connection prompt)
// and, if accepted, persists it to known_hosts.
static bool verify_host_key(const SshConfig &cfg) {
    const std::string &host = cfg.host;
    int port = cfg.port;
    // Build default path if not provided
    std::string kh_path = cfg.known_hosts_path;
    if (kh_path.empty()) {
#ifndef _WIN32
        const char *home = getenv("HOME");
        if (home)
            kh_path = std::string(home) + "/.ssh/known_hosts";
#else
        // Windows: skip host key verification for now (return true to allow connection)
        return true;
#endif
    }

    // If path is still empty, allow the connection
    if (kh_path.empty()) {
        SDL_Log("[SSH] no known_hosts path, skipping host key verification\n");
        return true;
    }

    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(s_session);
    if (!kh) {
        SDL_Log("[SSH] knownhost init failed\n");
        return false;
    }

    int readrc = libssh2_knownhost_readfile(kh, kh_path.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    if (readrc < 0)
        SDL_Log("[SSH] no existing known_hosts at '%s' (or unreadable) — treating as empty\n", kh_path.c_str());

    // Get the host key from the server
    size_t hkey_len = 0;
    int hkey_type = 0;
    const char *hkey = libssh2_session_hostkey(s_session, &hkey_len, &hkey_type);

    if (!hkey) {
        SDL_Log("[SSH] cannot retrieve host key from session\n");
        libssh2_knownhost_free(kh);
        return false;
    }

    // Check the host key
    int check_flags = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    check_flags |= hostkey_type_to_knownhost_flag(hkey_type);

    struct libssh2_knownhost *store = nullptr;
    int checkrc = libssh2_knownhost_checkp(kh, host.c_str(), port, hkey, hkey_len,
                                            check_flags, &store);

    bool trusted = false;
    switch (checkrc) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        SDL_Log("[SSH] host key verified: %s:%d\n", host.c_str(), port);
        trusted = true;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: {
        SDL_Log("[SSH] WARNING: host key not in known_hosts: %s:%d\n", host.c_str(), port);

        if (!cfg.prompt_host_key) {
            trusted = false;
            break;
        }

        std::string ip         = get_peer_ip();
        std::string host_field = ip.empty() ? host : (host + " (" + ip + ")");
        std::string fp         = hostkey_fingerprint();
        const char *keyname    = hostkey_type_name(hkey_type);

        char prompt[1024];
        snprintf(prompt, sizeof(prompt),
                 "The authenticity of host '%s' can't be established.\r\n"
                 "%s key fingerprint is: %s\r\n"
                 "This key is not known by any other names.\r\n"
                 "Are you sure you want to continue connecting (yes/no/[fingerprint])? ",
                 host_field.c_str(), keyname, fp.c_str());

        std::string answer = cfg.prompt_host_key(prompt);
        for (char &c : answer) c = (char)tolower((unsigned char)c);

        if (answer == "yes") {
            // Best-effort: ~/.ssh may not exist yet on a fresh machine.
#ifndef _WIN32
            std::string dir = kh_path.substr(0, kh_path.find_last_of('/'));
            if (!dir.empty()) mkdir(dir.c_str(), 0700);
#endif
            struct libssh2_knownhost *added = nullptr;
            int addrc = libssh2_knownhost_addc(kh, host.c_str(), nullptr, hkey, hkey_len,
                                                nullptr, 0, check_flags, &added);
            if (addrc == 0 &&
                libssh2_knownhost_writefile(kh, kh_path.c_str(),
                                             LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
                SDL_Log("[SSH] host key permanently added for '%s' to known_hosts\n", host.c_str());
            } else {
                SDL_Log("[SSH] WARNING: could not persist host key to '%s'\n", kh_path.c_str());
            }
            trusted = true;
        } else {
            SDL_Log("[SSH] host key not trusted by user, aborting connection\n");
            trusted = false;
        }
        break;
    }
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        SDL_Log("[SSH] ERROR: host key mismatch (possible MITM): %s:%d\n", host.c_str(), port);
        trusted = false;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_FAILURE:
        SDL_Log("[SSH] host key check failure\n");
        trusted = false;
        break;
    default:
        SDL_Log("[SSH] unknown host key check result: %d\n", checkrc);
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
    if (!verify_host_key(cfg)) {
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
        bool server_allows_password = !auth_methods ||
                                      strstr(auth_methods, "password") != nullptr;
        SDL_Log("[SSH] password auth stage: password_set=%s prompt_cb=%s server_allows=%s\n",
                cfg.password.empty() ? "no" : "yes",
                cfg.prompt_password ? "yes" : "no",
                server_allows_password ? "yes" : "no");
        
        if (server_allows_password) {
            // Try supplied password first (if any)
            if (!cfg.password.empty()) {
                SDL_Log("[SSH] attempting with supplied password\n");
                authed = auth_password(cfg.user, cfg.password);
            }
            
            // If that failed and we have a prompt callback, allow up to 3 attempts
            if (!authed && cfg.prompt_password) {
                const int MAX_ATTEMPTS = 3;
                for (int attempt = 1; attempt <= MAX_ATTEMPTS && !authed; attempt++) {
                    char prompt[256];
                    if (attempt == 1) {
                        snprintf(prompt, sizeof(prompt), "%s@%s's password: ",
                                 cfg.user.c_str(), cfg.host.c_str());
                    } else {
                        snprintf(prompt, sizeof(prompt), "Permission denied, try again. %s@%s's password: ",
                                 cfg.user.c_str(), cfg.host.c_str());
                    }
                    
                    SDL_Log("[SSH] password prompt attempt %d/%d\n", attempt, MAX_ATTEMPTS);
                    std::string password = cfg.prompt_password(prompt);
                    
                    if (password.empty()) {
                        SDL_Log("[SSH] user aborted password entry\n");
                        break;
                    }
                    
                    authed = auth_password(cfg.user, password);
                    if (authed) {
                        SDL_Log("[SSH] authenticated via password on attempt %d\n", attempt);
                    } else {
                        SDL_Log("[SSH] password attempt %d failed\n", attempt);
                    }
                }
            }
        }
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

    // Request PTY. Servers like git@github.com refuse PTY allocation but
    // still open the channel and send a message before closing — non-fatal.
    while ((rc = libssh2_channel_request_pty_ex(
                s_channel,
                "xterm-256color", (unsigned int)strlen("xterm-256color"),
                nullptr, 0,
                (int)t->cols, (int)t->rows,
                0, 0)) == LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);
    s_have_pty = (rc == 0);
    if (!s_have_pty)
        SDL_Log("%s (continuing without PTY)\n", last_ssh2_error("pty request failed").c_str());

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

    std::unique_lock<std::recursive_mutex> lock(s_session_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;

    for (;;) {
        ssize_t n = libssh2_channel_read(s_channel, buf, sizeof(buf));
        if (n > 0) {
            term_feed(t, buf, (int)n);
            got_data = true;
            continue;
        } else if (n != LIBSSH2_ERROR_EAGAIN && n < 0) {
            SDL_Log("%s\n", last_ssh2_error("channel read error").c_str());
            s_active = false;
            break;
        }

        // Also drain stderr — servers that reject a shell (e.g. git@github.com)
        // send their message here before closing the channel.
        ssize_t ne = libssh2_channel_read_stderr(s_channel, buf, sizeof(buf));
        if (ne > 0) {
            term_feed(t, buf, (int)ne);
            got_data = true;
            continue;
        } else if (ne != LIBSSH2_ERROR_EAGAIN && ne < 0) {
            SDL_Log("%s\n", last_ssh2_error("stderr read error").c_str());
            s_active = false;
            break;
        }

        if (n == LIBSSH2_ERROR_EAGAIN && ne == LIBSSH2_ERROR_EAGAIN) {
            static uint32_t s_last_keepalive = 0;
            uint32_t now = SDL_GetTicks();
            if (now - s_last_keepalive >= 1000) {
                int next_keepalive = 0;
                libssh2_keepalive_send(s_session, &next_keepalive);
                s_last_keepalive = now;
            }
            break;
        }

        if (libssh2_channel_eof(s_channel)) {
            s_active = false;
            break;
        }
    }
    return got_data;
}

void ssh_write(Terminal *t, const char *buf, int n) {
    (void)t;
    if (!s_active || !s_channel || n <= 0) return;

    std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
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
    if (!s_active || !s_channel || !s_have_pty) return;
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
    s_have_pty = false;
    libssh2_exit();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool ssh_active()  { return s_active; }
bool ssh_has_pty() { return s_have_pty; }

LIBSSH2_SESSION *ssh_get_session() { return s_session; }
int              ssh_get_socket()  { return s_sock; }
void             ssh_session_lock()   { s_session_mutex.lock(); }
void             ssh_session_unlock() { s_session_mutex.unlock(); }

void ssh_reset_after_fork() {
    // Clean up inherited libssh2 state from parent process.
    // After fork(), child inherits parent's static variables which point to
    // freed/invalid memory. This resets them so the child can initialize
    // its own libssh2 session cleanly.
    libssh2_exit();
    
    // Reset static state
    s_session = nullptr;
    s_channel = nullptr;
    s_sock    = -1;
    s_active  = false;
    s_have_pty = false;
    
    // Re-initialize libssh2 for the child process
    if (libssh2_init(0) != 0) {
        SDL_Log("[SSH] libssh2_init (post-fork) failed\n");
    }
}

#endif // USESSH
