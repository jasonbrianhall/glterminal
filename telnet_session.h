#pragma once

// ============================================================================
// TELNET / RAW TCP SESSION
//
// Port 23 (default): full Telnet protocol negotiation via libtelnet.
// Any other port, or cfg.raw_mode = true: raw TCP — bytes pass straight
// through to term_feed() with no IAC processing.  Useful for HTTP, SMTP,
// finger, or any plain-text TCP service.
// ============================================================================

#include "terminal.h"
#include <string>

struct TelnetConfig {
    std::string host;
    int         port     = 23;
    std::string ttype    = "xterm-256color";
    bool        raw_mode = false;  // true = skip Telnet negotiation entirely
};

bool telnet_connect(const TelnetConfig &cfg, Terminal *t);
bool telnet_read(Terminal *t);
void telnet_write(Terminal *t, const char *buf, int n);
void telnet_pty_resize(int cols, int rows);
bool telnet_active();
bool telnet_channel_closed();
void telnet_disconnect();
