#pragma once

#ifdef USESSH

#include <string>

// Start SFTP web server on port 53716
// Called when user presses F12 in sftp_console
// Returns true on success
bool sftp_webserver_start();

// Start the web server in local-filesystem mode: serves root_dir directly
// off disk, no SSH/SFTP involved. Read-only (browse + download only).
// For when there's no active SSH session to browse (e.g. F9 locally).
bool sftp_webserver_start_local(const char *root_dir);

// Stop the SFTP web server
void sftp_webserver_stop();

// Check if web server is running
bool sftp_webserver_running();

// Get the actual port the web server is listening on (only valid if running)
int sftp_webserver_get_port();

#endif // USESSH
