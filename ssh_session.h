#pragma once

// ============================================================================
// SSH SESSION  — libssh2-backed remote shell, compiled only when USESSH is
// defined.  Mirrors the term_pty read/write interface so the main loop needs
// only minimal #ifdef guards.
//
// Build addition (when USESSH is defined):
//   g++ ... ssh_session.cpp ... -lssh2 -lcrypto -lssl -DUSESSH -o gl_terminal
//
// Typical usage:
//   SshConfig cfg;
//   cfg.host = "myserver.example.com";
//   cfg.port = 22;
//   cfg.user = "alice";
//   cfg.key_path = "/home/alice/.ssh/id_ed25519";  // "" = try agent then password
//   cfg.password = "";
//
//   if (!ssh_connect(cfg, &term))  // blocks until authenticated
//       return 1;
//
//   // main loop — drop-in replacements for term_read / term_write:
//   if (ssh_read(&term))   needs_render = true;
//   ssh_write(&term, buf, n);
//
//   // resize after window resize:
//   ssh_pty_resize(term.cols, term.rows);
//
//   // shutdown:
//   ssh_disconnect();
// ============================================================================

#ifdef USESSH

#include "terminal.h"
#include <string>
#include <functional>

struct SshConfig {
    std::string host;
    int         port        = 22;
    std::string user;
    // Authentication: tried in order —
    //   1. SSH agent (if running)
    //   2. key_path  (private key; key_path_pub = key_path + ".pub" if empty)
    //   3. password  (if non-empty, or prompt_password is called)
    std::string key_path;       // "" = skip file-based key auth
    std::string key_path_pub;   // "" = derive from key_path
    std::string password;       // "" = skip password auth (use prompt_password)
    // Optional: known-hosts file for host-key verification.
    // Set to "" to skip verification (INSECURE — use only on trusted networks).
    std::string known_hosts_path;  // default: ~/.ssh/known_hosts
    // Called when no password was supplied and agent/key auth both failed.
    // Receives the prompt string (e.g. "user@host's password: ") and must
    // return the password entered by the user, or "" to abort.
    // If nullptr, password auth is skipped when no password is supplied.
    std::function<std::string(const char *prompt)> prompt_password;
};

// Connect, authenticate, and open a PTY channel.
// Blocks until fully connected or failure.  On success the terminal's
// dimensions are sent to the remote side.
// Returns true on success; on failure prints a message via SDL_Log and returns false.
bool ssh_connect(const SshConfig &cfg, Terminal *t);

// Non-blocking read from the SSH channel into the terminal parser.
// Returns true if any data was received (caller should set needs_render).
bool ssh_read(Terminal *t);

// Write n bytes to the SSH channel.
void ssh_write(Terminal *t, const char *buf, int n);

// Send a PTY resize request to the remote side.
void ssh_pty_resize(int cols, int rows);

// Returns true if the remote channel has been closed / EOF'd.
bool ssh_channel_closed();

// Close channel and free libssh2 resources.
void ssh_disconnect();

// Returns true if an SSH session is currently active.
bool ssh_active();

#endif // USESSH
