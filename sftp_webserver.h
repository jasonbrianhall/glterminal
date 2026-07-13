#pragma once

#ifdef USESSH

#include <string>

// Start SFTP web server on specified address and port
// Called when user presses F12 in sftp_console or via CLI argument --web-server
// Returns true on success
bool sftp_webserver_start(const char *bind_addr = "127.0.0.1", int port = 53716);

// Start the web server in local-filesystem mode: serves root_dir directly
// off disk, no SSH/SFTP involved. Read-only (browse + download only).
// For when there's no active SSH session to browse (e.g. F9 locally).
bool sftp_webserver_start_local(const char *root_dir, const char *bind_addr = "127.0.0.1", int port = 53716);

// Stop the SFTP web server
void sftp_webserver_stop();

// Check if web server is running
bool sftp_webserver_running();

// Get the actual port the web server is listening on (only valid if running)
int sftp_webserver_get_port();

#endif // USESSH
