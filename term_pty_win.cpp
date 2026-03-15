#include "term_pty.h"
#include "terminal.h"

#include <windows.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// STATE
// ============================================================================

static HPCON                    s_hPC     = nullptr;
static HANDLE                   s_hRead   = nullptr;  // read child output
static HANDLE                   s_hWrite  = nullptr;  // write child input
static PROCESS_INFORMATION      s_pi      = {};
static HANDLE                   s_hThread = nullptr;

// Mutex protecting Terminal state — reader thread calls term_feed
// from a background thread, main thread renders from it.
static CRITICAL_SECTION         s_cs;
static bool                     s_cs_inited = false;

// Shared with main via term_read(): data the reader thread buffered
#define PIPE_BUF_SZ 65536
static char     s_pipe_buf[PIPE_BUF_SZ];
static int      s_pipe_len = 0;
static HANDLE   s_pipe_mutex = nullptr;  // protects s_pipe_buf

// ============================================================================
// READER THREAD
// ============================================================================

static DWORD WINAPI pty_reader_thread(LPVOID arg) {
    Terminal *t = (Terminal*)arg;
    char buf[4096];
    DWORD n;
    while (ReadFile(s_hRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
        WaitForSingleObject(s_pipe_mutex, INFINITE);
        int copy = (int)n;
        if (s_pipe_len + copy > PIPE_BUF_SZ)
            copy = PIPE_BUF_SZ - s_pipe_len;
        if (copy > 0) {
            memcpy(s_pipe_buf + s_pipe_len, buf, copy);
            s_pipe_len += copy;
        }
        ReleaseMutex(s_pipe_mutex);
        (void)t;
    }
    return 0;
}

// ============================================================================
// term_spawn
// ============================================================================

bool term_spawn(Terminal *t, const char *cmd) {
    s_pipe_mutex = CreateMutex(nullptr, FALSE, nullptr);

    COORD size = { (SHORT)t->cols, (SHORT)t->rows };

    HANDLE hPipeIn_r, hPipeIn_w, hPipeOut_r, hPipeOut_w;
    if (!CreatePipe(&hPipeIn_r,  &hPipeIn_w,  nullptr, 0)) return false;
    if (!CreatePipe(&hPipeOut_r, &hPipeOut_w, nullptr, 0)) return false;

    HRESULT hr = CreatePseudoConsole(size, hPipeIn_r, hPipeOut_w, 0, &s_hPC);
    CloseHandle(hPipeIn_r);
    CloseHandle(hPipeOut_w);
    if (FAILED(hr)) return false;

    s_hRead  = hPipeOut_r;
    s_hWrite = hPipeIn_w;

    // Build STARTUPINFOEX with ConPTY attribute
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrList =
        (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
    InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrList, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
        s_hPC, sizeof(s_hPC), nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrList;

    // Inherit TERM and COLORTERM so the shell knows it has colour support
    // SetEnvironmentVariable affects the current process, child inherits it.
    SetEnvironmentVariableW(L"TERM",      L"xterm-256color");
    SetEnvironmentVariableW(L"COLORTERM", L"truecolor");

    wchar_t wcmd[512];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 512);

    BOOL ok = CreateProcessW(
        nullptr, wcmd,
        nullptr, nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr,
        &si.StartupInfo, &s_pi);

    DeleteProcThreadAttributeList(attrList);
    free(attrList);

    if (!ok) return false;

    s_hThread = CreateThread(nullptr, 0, pty_reader_thread, t, 0, nullptr);
    return true;
}

// ============================================================================
// term_read  — drain the reader thread's buffer into the terminal
// ============================================================================

bool term_read(Terminal *t) {
    WaitForSingleObject(s_pipe_mutex, INFINITE);
    if (s_pipe_len == 0) {
        ReleaseMutex(s_pipe_mutex);
        return false;
    }
    // Snapshot the buffer
    char local[PIPE_BUF_SZ];
    int  local_len = s_pipe_len;
    memcpy(local, s_pipe_buf, local_len);
    s_pipe_len = 0;
    ReleaseMutex(s_pipe_mutex);

    term_feed(t, local, local_len);
    return true;
}

// ============================================================================
// term_write
// ============================================================================

void term_write(Terminal *t, const char *s, int n) {
    (void)t;
    DWORD written;
    WriteFile(s_hWrite, s, (DWORD)n, &written, nullptr);
}

// ============================================================================
// term_child_exited
// ============================================================================

bool term_child_exited(void) {
    if (!s_pi.hProcess) return true;
    return WaitForSingleObject(s_pi.hProcess, 0) == WAIT_OBJECT_0;
}

void term_pty_resize(int cols, int rows) {
    if (!s_hPC) return;
    COORD size = { (SHORT)cols, (SHORT)rows };
    ResizePseudoConsole(s_hPC, size);
}
