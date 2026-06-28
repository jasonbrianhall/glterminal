#pragma once

#include <stdbool.h>

// Web server for SFTP console (F12 hotkey)
// Runs on localhost:53716

void sftp_webserver_start(void);
bool sftp_webserver_is_running(void);
void sftp_webserver_stop(void);
