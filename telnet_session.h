#pragma once

// ============================================================================
// TELNET SESSION  — libtelnet-backed remote shell.
// Mirrors the ssh_session read/write interface so the main loop needs
// only minimal guards alongside the SSH ones.
//
// Build:
//   g++ ... telnet_session.cpp libtelnet.c ... -o gl_terminal
//
// Typical usage:
//   TelnetConfig cfg;
//   cfg.host = "bbs.example.com";
//   cfg.port = 23;
//   cfg.ttype = "xterm-256color";
//
//   if (!telnet_connect(cfg, &term))  return 1;
//   if (telnet_read(&term))           needs_render = true;
//   telnet_pty_resize(term.cols, term.rows);
//   telnet_disconnect();
// ============================================================================

#include "terminal.h"
#include <string>

struct TelnetConfig {
    std::string host;
    int         port  = 23;
    std::string ttype = "xterm-256color";
};

bool telnet_connect(const TelnetConfig &cfg, Terminal *t);
bool telnet_read(Terminal *t);
void telnet_write(Terminal *t, const char *buf, int n);
void telnet_pty_resize(int cols, int rows);
bool telnet_active();
bool telnet_channel_closed();
void telnet_disconnect();
