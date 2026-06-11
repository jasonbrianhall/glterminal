// serial_session.cpp — cross-platform RS-232/USB serial console for gl_terminal

#include "serial_session.h"
#include "terminal.h"
#include "term_pty.h"

#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifndef _WIN32
#  include <unistd.h>
#  include <fcntl.h>
#  include <termios.h>
#  include <sys/ioctl.h>
   typedef int SerialHandle;
#  define INVALID_SERIAL_HANDLE (-1)
#else
#  include <windows.h>
   typedef HANDLE SerialHandle;
#  define INVALID_SERIAL_HANDLE (INVALID_HANDLE_VALUE)
#endif

// ============================================================================
// STATE
// ============================================================================

static SerialHandle  s_fd        = INVALID_SERIAL_HANDLE;
static bool          s_active    = false;
static bool          s_closed    = false;
static bool          s_echo      = false;
static bool          s_nl_crnl   = true;   // translate \n→\r\n on receive
static Terminal     *s_term      = nullptr;

// ============================================================================
// HELPERS — POSIX
// ============================================================================

#ifndef _WIN32

static speed_t baud_to_speed(int baud) {
    switch (baud) {
    case 300:    return B300;
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default:
        SDL_Log("[SERIAL] unknown baud %d, defaulting to 9600\n", baud);
        return B9600;
    }
}

static bool serial_open_posix(const SerialConfig &cfg) {
    // Open blocking — O_NONBLOCK on open() causes pts/tty slaves to fail or
    // return EOF immediately. We switch to non-blocking after tcsetattr.
    int fd = open(cfg.port.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        SDL_Log("[SERIAL] open(%s) failed: %s\n", cfg.port.c_str(), strerror(errno));
        return false;
    }

    struct termios tio = {};
    if (tcgetattr(fd, &tio) < 0) {
        SDL_Log("[SERIAL] tcgetattr failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    // Raw mode — no processing
    cfmakeraw(&tio);

    // Baud rate
    speed_t speed = baud_to_speed(cfg.baud);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    // Data bits
    tio.c_cflag &= ~CSIZE;
    switch (cfg.data_bits) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    default:tio.c_cflag |= CS8; break;
    }

    // Stop bits
    if (cfg.stop_bits == SerialStopBits::TWO)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= ~CSTOPB;

    // Parity
    tio.c_cflag &= ~(PARENB | PARODD);
    if (cfg.parity == SerialParity::ODD)
        tio.c_cflag |= (PARENB | PARODD);
    else if (cfg.parity == SerialParity::EVEN)
        tio.c_cflag |= PARENB;
    // MARK/SPACE parity not universally supported on POSIX; log and skip
    else if (cfg.parity == SerialParity::MARK || cfg.parity == SerialParity::SPACE)
        SDL_Log("[SERIAL] MARK/SPACE parity not supported on this platform, using NONE\n");

    // Flow control
    tio.c_cflag &= ~CRTSCTS;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    if (cfg.flow == SerialFlow::HARDWARE)
        tio.c_cflag |= CRTSCTS;
    else if (cfg.flow == SerialFlow::SOFTWARE)
        tio.c_iflag |= (IXON | IXOFF);

    // Enable receiver, local mode
    tio.c_cflag |= (CREAD | CLOCAL);

    // Non-blocking reads
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        SDL_Log("[SERIAL] tcsetattr failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    // Switch to non-blocking NOW — after open() and tcsetattr() are done.
    // Setting O_NONBLOCK at open time breaks pts/tty slaves.
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    tcflush(fd, TCIOFLUSH);
    s_fd = fd;
    return true;
}

#else // _WIN32

// ============================================================================
// HELPERS — WINDOWS
// ============================================================================

// Normalise "COM10" -> "\\.\COM10" so CreateFile works for ports > 9
static std::string win_port_path(const std::string &port) {
    if (port.size() >= 4 &&
        (port[0] == '\\' && port[1] == '\\'))
        return port;  // already a device path
    // Prefix any COMn with \\.\
    if (port.size() >= 3 &&
        (port[0] == 'C' || port[0] == 'c') &&
        (port[1] == 'O' || port[1] == 'o') &&
        (port[2] == 'M' || port[2] == 'm'))
        return std::string("\\\\.\\") + port;
    return port;
}

static DWORD baud_to_cbr(int baud) {
    // CBR_ constants are just the baud values as DWORDs on Windows
    return (DWORD)baud;
}

static bool serial_open_win32(const SerialConfig &cfg) {
    std::string path = win_port_path(cfg.port);

    HANDLE h = CreateFileA(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SDL_Log("[SERIAL] CreateFile(%s) failed: %lu\n", path.c_str(), GetLastError());
        return false;
    }

    // Set timeouts — non-blocking equivalent
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout         = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadTotalTimeoutConstant    = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = 0;
    SetCommTimeouts(h, &timeouts);

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        SDL_Log("[SERIAL] GetCommState failed: %lu\n", GetLastError());
        CloseHandle(h);
        return false;
    }

    dcb.BaudRate = baud_to_cbr(cfg.baud);
    dcb.ByteSize = (BYTE)cfg.data_bits;

    switch (cfg.stop_bits) {
    case SerialStopBits::ONE:  dcb.StopBits = ONESTOPBIT;   break;
    case SerialStopBits::ONE5: dcb.StopBits = ONE5STOPBITS; break;
    case SerialStopBits::TWO:  dcb.StopBits = TWOSTOPBITS;  break;
    }

    switch (cfg.parity) {
    case SerialParity::NONE:  dcb.Parity = NOPARITY;   dcb.fParity = FALSE; break;
    case SerialParity::ODD:   dcb.Parity = ODDPARITY;  dcb.fParity = TRUE;  break;
    case SerialParity::EVEN:  dcb.Parity = EVENPARITY; dcb.fParity = TRUE;  break;
    case SerialParity::MARK:  dcb.Parity = MARKPARITY; dcb.fParity = TRUE;  break;
    case SerialParity::SPACE: dcb.Parity = SPACEPARITY;dcb.fParity = TRUE;  break;
    }

    if (cfg.flow == SerialFlow::HARDWARE) {
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl  = RTS_CONTROL_HANDSHAKE;
        dcb.fOutX = dcb.fInX = FALSE;
    } else if (cfg.flow == SerialFlow::SOFTWARE) {
        dcb.fOutX = dcb.fInX = TRUE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl  = RTS_CONTROL_ENABLE;
        dcb.XonChar  = 0x11;
        dcb.XoffChar = 0x13;
    } else {
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl  = RTS_CONTROL_ENABLE;
        dcb.fOutX = dcb.fInX = FALSE;
    }

    dcb.fBinary = TRUE;
    dcb.fNull   = FALSE;

    if (!SetCommState(h, &dcb)) {
        SDL_Log("[SERIAL] SetCommState failed: %lu\n", GetLastError());
        CloseHandle(h);
        return false;
    }

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s_fd = h;
    return true;
}

#endif // _WIN32

// ============================================================================
// WRITE BRIDGE
// ============================================================================

static void serial_write_bridge(Terminal *t, const char *buf, int n) {
    serial_write(t, buf, n);
}

// Feed received bytes into the terminal, optionally expanding bare \n → \r\n.
// Most serial devices (Cisco, modems) send \r\n already; some send bare \n.
// The VT parser needs \r to reset the column — without it text staircases.
static void serial_feed(Terminal *t, const char *buf, int n) {
    if (!s_nl_crnl) {
        term_feed(t, buf, n);
        return;
    }
    // Scan for bare \n (not preceded by \r) and expand inline.
    const char *p   = buf;
    const char *end = buf + n;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', end - p);
        if (!nl) {
            term_feed(t, p, (int)(end - p));
            break;
        }
        // Feed everything up to (not including) the \n
        if (nl > p)
            term_feed(t, p, (int)(nl - p));
        // If previous char was already \r, don't double-insert
        bool already_cr = (nl > buf && nl[-1] == '\r') ||
                          (nl == buf && p == buf);  // can't check previous chunk; safe to insert
        if (!already_cr)
            term_feed(t, "\r\n", 2);
        else
            term_feed(t, "\n", 1);
        p = nl + 1;
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

bool serial_connect(const SerialConfig &cfg, Terminal *t) {
    s_term     = t;
    s_closed   = false;
    s_echo     = cfg.local_echo;
    s_nl_crnl  = cfg.nl_to_crnl;

#ifndef _WIN32
    if (!serial_open_posix(cfg)) return false;
#else
    if (!serial_open_win32(cfg)) return false;
#endif

    const char *parity_str[] = { "N", "O", "E", "M", "S" };
    const char *stop_str[]   = { "1", "1.5", "2" };
    const char *flow_str[]   = { "none", "hw", "sw" };
    SDL_Log("[SERIAL] opened %s @ %d %d%s%s flow=%s\n",
            cfg.port.c_str(), cfg.baud,
            cfg.data_bits,
            parity_str[(int)cfg.parity],
            stop_str[(int)cfg.stop_bits],
            flow_str[(int)cfg.flow]);

    g_term_write_override = serial_write_bridge;
    s_active = true;
    return true;
}

bool serial_read(Terminal *t) {
    if (s_fd == INVALID_SERIAL_HANDLE || s_closed) return false;
    s_term = t;

    char buf[4096];
    bool got_data = false;

#ifndef _WIN32
    for (;;) {
        ssize_t n = read(s_fd, buf, sizeof(buf));
        if (n > 0) {
            serial_feed(t, buf, (int)n);
            got_data = true;
        } else if (n == 0) {
            // On a pts/tty with O_NONBLOCK, read()=0 means no data (not EOF).
            // Real device disconnect comes as EIO, not n==0.
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EIO) {
                // EIO = device physically disconnected (USB unplug, etc.)
                SDL_Log("[SERIAL] port closed (EIO — device disconnected)\n");
                s_closed = true;
            } else {
                SDL_Log("[SERIAL] read error: %s\n", strerror(errno));
                s_closed = true;
            }
            break;
        }
    }
#else
    // Overlapped read loop
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    for (;;) {
        DWORD bytes_read = 0;
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(s_fd, buf, sizeof(buf), &bytes_read, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                // Non-blocking: check if data is available without waiting
                if (!GetOverlappedResult(s_fd, &ov, &bytes_read, FALSE)) {
                    DWORD err2 = GetLastError();
                    if (err2 == ERROR_IO_INCOMPLETE) break; // no data yet
                    SDL_Log("[SERIAL] read error: %lu\n", err2);
                    s_closed = true;
                    break;
                }
            } else {
                SDL_Log("[SERIAL] ReadFile error: %lu\n", err);
                s_closed = true;
                break;
            }
        }
        if (bytes_read > 0) {
            serial_feed(t, buf, (int)bytes_read);
            got_data = true;
        } else {
            break; // no more data right now
        }
    }

    CloseHandle(ov.hEvent);
#endif

    return got_data;
}

void serial_write(Terminal *t, const char *buf, int n) {
    if (s_fd == INVALID_SERIAL_HANDLE || n <= 0) return;

    // Local echo
    if (s_echo && t && n >= 1) {
        unsigned char first = (unsigned char)buf[0];
        if (first == '\r') {
            term_feed(t, "\r\n", 2);
        } else if (first >= 0x20 && first < 0x7F) {
            term_feed(t, buf, n);
        } else if (first == 0x08 || first == 0x7F) {
            term_feed(t, "\b \b", 3);
        }
    }

#ifndef _WIN32
    const char *ptr = buf;
    int rem = n;
    while (rem > 0) {
        ssize_t sent = write(s_fd, ptr, (size_t)rem);
        if (sent > 0) { ptr += sent; rem -= (int)sent; }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { SDL_Log("[SERIAL] write error: %s\n", strerror(errno)); break; }
    }
#else
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return;

    const char *ptr = buf;
    int rem = n;
    while (rem > 0) {
        DWORD written = 0;
        BOOL ok = WriteFile(s_fd, ptr, (DWORD)rem, &written, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(s_fd, &ov, &written, TRUE); // wait for write
        if (!ok || written == 0) {
            SDL_Log("[SERIAL] write error: %lu\n", GetLastError());
            break;
        }
        ptr += written;
        rem -= (int)written;
        ResetEvent(ov.hEvent);
    }

    CloseHandle(ov.hEvent);
#endif
}

bool serial_active()         { return s_active; }
bool serial_channel_closed() { return s_closed; }

void serial_disconnect() {
    g_term_write_override = nullptr;

#ifndef _WIN32
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = INVALID_SERIAL_HANDLE;
    }
#else
    if (s_fd != INVALID_HANDLE_VALUE) {
        CloseHandle(s_fd);
        s_fd = INVALID_HANDLE_VALUE;
    }
#endif

    s_active  = false;
    s_closed  = false;
    s_echo    = false;
    s_nl_crnl = true;
    s_term    = nullptr;
    SDL_Log("[SERIAL] disconnected\n");
}
