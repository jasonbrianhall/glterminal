#pragma once

#ifdef USESSH

#include <string>

// Start SFTP web server on port 53716
// Called when user presses F12 in sftp_console
// Returns true on success
bool sftp_webserver_start();

// Stop the SFTP web server
void sftp_webserver_stop();

// Check if web server is running
bool sftp_webserver_running();

// Get the actual port the web server is listening on (only valid if running)
int sftp_webserver_get_port();

#endif // USESSH
