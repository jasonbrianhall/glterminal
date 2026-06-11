#pragma once

// ============================================================================
// SERIAL SESSION  — cross-platform RS-232 / USB-serial console
//
// Useful for Cisco/network gear consoles, embedded boards, /dev/ttyS0, etc.
// On Linux/macOS: /dev/ttyS0, /dev/ttyUSB0, /dev/ttyACM0, ...
// On Windows:     COM1, COM2, \\.\COM10, ...
//
// Usage:
//   SerialConfig cfg;
//   cfg.port     = "/dev/ttyUSB0";  // or "COM3"
//   cfg.baud     = 9600;
//   cfg.data_bits = 8;
//   cfg.parity   = SerialParity::NONE;
//   cfg.stop_bits = SerialStopBits::ONE;
//   cfg.flow     = SerialFlow::NONE;
//
//   if (!serial_connect(cfg, &term)) return 1;
//
//   // main loop:
//   if (serial_read(&term)) needs_render = true;
//   serial_write(&term, buf, n);
//
//   serial_disconnect();
// ============================================================================

#include "terminal.h"
#include <string>

enum class SerialParity   { NONE, ODD, EVEN, MARK, SPACE };
enum class SerialStopBits { ONE, ONE5, TWO };
enum class SerialFlow     { NONE, HARDWARE, SOFTWARE };  // SOFTWARE = XON/XOFF

struct SerialConfig {
    std::string   port;               // Linux: "/dev/ttyUSB0", "/dev/ttyS0"
                                      // Windows: "COM1", "COM3", "COM10"
    int           baud      = 9600;
    int           data_bits = 8;      // 5, 6, 7, or 8
    SerialParity  parity    = SerialParity::NONE;
    SerialStopBits stop_bits = SerialStopBits::ONE;
    SerialFlow    flow      = SerialFlow::NONE;
    bool          local_echo  = false; // echo typed chars locally (useful for raw devices)
    bool          nl_to_crnl  = true;  // translate incoming \n → \r\n (disable for devices that send proper \r\n)
};

// Standard baud rates for the connection dialog
static const int SERIAL_BAUD_RATES[] = {
    300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
};
static const int SERIAL_BAUD_COUNT = (int)(sizeof(SERIAL_BAUD_RATES)/sizeof(SERIAL_BAUD_RATES[0]));

// Connect and configure the serial port. Blocks until open or failure.
bool serial_connect(const SerialConfig &cfg, Terminal *t);

// Non-blocking read — returns true if any data was received.
bool serial_read(Terminal *t);

// Write n bytes to the serial port.
void serial_write(Terminal *t, const char *buf, int n);

// Returns true if the port is open and active.
bool serial_active();

// Returns true if the port has been closed / lost (e.g. USB unplug).
bool serial_channel_closed();

// Close the port and release resources.
void serial_disconnect();
