#pragma once

// ============================================================================
// PORT FORWARDING  — local (-L), remote (-R), and SOCKS5 dynamic (-D).
// Compiled only when USESSH is defined.
//
// Local forwarding  (-L local_port:remote_host:remote_port):
//   Binds a TCP port on localhost.  Each accepted connection opens a
//   libssh2_channel_direct_tcpip tunnel to remote_host:remote_port through
//   the SSH session, then pumps data on a background thread.
//
// Remote forwarding (-R remote_port:local_host:local_port):
//   Asks the SSH server to bind remote_port on the server side.  Incoming
//   connections are forwarded back to local_host:local_port by a background
//   thread that polls libssh2_channel_forward_accept.
//
// SOCKS5 dynamic forwarding (-D local_port):
//   Binds a local SOCKS5 proxy port.  Each connection performs a SOCKS5
//   handshake to negotiate the destination, then opens a
//   libssh2_channel_direct_tcpip tunnel to that destination through SSH.
//   Compatible with curl --socks5, Firefox/Chrome proxy settings, etc.
//   Only CONNECT with no-auth is supported (matching OpenSSH -D behaviour).
//
// All channel I/O runs on per-tunnel threads so the main loop is never
// blocked.  The session mutex (ssh_session_lock / ssh_session_unlock) is
// held only during libssh2 calls; the rest of the time it is released so
// ssh_read / ssh_write continue to work normally.
//
// Usage (from gl_terminal_main.cpp, after ssh_connect succeeds):
//
//   // Parsed from --forward-local / --forward-remote / --dynamic CLI flags:
//   pf_add_local(5432, "db.internal", 5432);
//   pf_add_remote(8080, "localhost", 8080);
//   pf_add_socks(1080);
//
//   // In the shutdown path (before ssh_disconnect):
//   pf_shutdown_all();
// ============================================================================

#ifdef USESSH

#include <string>
#include <vector>

// Forward a local TCP port through the SSH tunnel to a remote destination.
// Spawns a background listener thread immediately.
// Returns true if the listen socket was bound successfully.
bool pf_add_local(int local_port,
                  const std::string &remote_host,
                  int                remote_port);

// Ask the SSH server to forward a remote port back to a local destination.
// Spawns a background accept thread immediately.
// Returns true if the server accepted the tcpip-forward request.
bool pf_add_remote(int                remote_port,
                   const std::string &local_host,
                   int                local_port);

// Bind a SOCKS5 proxy on local_port.  Each connection negotiates its
// destination via the SOCKS5 handshake, then tunnels through SSH.
// Spawns a background listener thread immediately.
// Returns true if the listen socket was bound successfully.
bool pf_add_socks(int local_port);

// Stop all tunnels and join all background threads.
// Must be called before ssh_disconnect().
void pf_shutdown_all();

// Parse an OpenSSH-style forwarding spec: "local_port:remote_host:remote_port"
// (same format for -L and -R).
// Returns true and fills the out-params on success.
bool pf_parse_spec(const std::string &spec,
                   int               *out_lport,
                   std::string       *out_rhost,
                   int               *out_rport);

// Snapshot of one active tunnel (for status display).
struct PfStatus {
    enum Type { LOCAL, REMOTE, SOCKS } type;
    int         local_port;
    std::string remote_host;   // "" for SOCKS
    int         remote_port;   // 0  for SOCKS
    int         active_connections;  // live tunnel count for this forward
    bool        listener_ok;         // false if bind/tcpip-forward failed
};
std::vector<PfStatus> pf_status();

#endif // USESSH
