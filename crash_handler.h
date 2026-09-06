#pragma once
// crash_handler.h - drop-in crash logger for MSVC/Windows builds.
// Call install_crash_handler() as the very first line of main()/WinMain(),
// before anything else runs (SDL_Init, threads, gfx init, etc.).
//
// On an unhandled structured exception (access violation, div-by-zero,
// stack overflow, etc.) this writes a plain text stack trace -- with real
// function names and source file/line numbers IF the .pdb next to the exe
// matches the build -- to "crash_log.txt" next to the exe, then lets the
// process die normally. No debugger required.
//
// Only does anything on _MSC_VER builds; a no-op elsewhere.

#if defined(_WIN32) && defined(_MSC_VER)

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#pragma comment(lib, "dbghelp.lib")

#if defined(_M_X64) || defined(_M_AMD64)
    #define CH_MACHINE_TYPE  IMAGE_FILE_MACHINE_AMD64
    #define CH_PC(ctx)       ((ctx).Rip)
    #define CH_FRAME(ctx)    ((ctx).Rbp)
    #define CH_STACK(ctx)    ((ctx).Rsp)
#elif defined(_M_ARM64)
    #define CH_MACHINE_TYPE  IMAGE_FILE_MACHINE_ARM64
    #define CH_PC(ctx)       ((ctx).Pc)
    #define CH_FRAME(ctx)    ((ctx).Fp)
    #define CH_STACK(ctx)    ((ctx).Sp)
#elif defined(_M_IX86)
    #define CH_MACHINE_TYPE  IMAGE_FILE_MACHINE_I386
    #define CH_PC(ctx)       ((ctx).Eip)
    #define CH_FRAME(ctx)    ((ctx).Ebp)
    #define CH_STACK(ctx)    ((ctx).Esp)
#else
    #error "crash_handler.h: unsupported architecture"
#endif

namespace crash_handler_detail {

inline void write_thread_stack(FILE *f, HANDLE process, HANDLE thread, CONTEXT ctx) {
    STACKFRAME64 frame = {};
    DWORD machine = CH_MACHINE_TYPE;
    frame.AddrPC.Offset    = CH_PC(ctx);
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = CH_FRAME(ctx);
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = CH_STACK(ctx);
    frame.AddrStack.Mode   = AddrModeFlat;

    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machine, process, thread, &frame, &ctx, nullptr,
                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0)
            break;

        char symbuf[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;

        DWORD64 sym_disp = 0;
        const char *name = "???";
        if (SymFromAddr(process, frame.AddrPC.Offset, &sym_disp, sym))
            name = sym->Name;

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD line_disp = 0;

        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_disp, &line)) {
            fprintf(f, "  #%-2d %s + 0x%llx   [%s:%lu]\n",
                    i, name, (unsigned long long)sym_disp, line.FileName, line.LineNumber);
        } else {
            fprintf(f, "  #%-2d %s + 0x%llx   [no line info]\n",
                    i, name, (unsigned long long)sym_disp);
        }
    }
}

inline LONG WINAPI top_level_filter(EXCEPTION_POINTERS *ep) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    // Search for the .pdb next to the exe, and in CWD, as a fallback.
    SymInitialize(process, nullptr, TRUE);

    FILE *f = fopen("crash_log.txt", "w");
    if (!f) {
        // Fall back to a location we can definitely write to.
        f = fopen("C:\\crash_log.txt", "w");
    }
    if (f) {
        fprintf(f, "=== CRASH ===\n");
        fprintf(f, "Exception code:    0x%08X\n", ep->ExceptionRecord->ExceptionCode);
        fprintf(f, "Exception address: %p\n", ep->ExceptionRecord->ExceptionAddress);
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            fprintf(f, "Access violation %s address 0x%p\n",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "writing to" : "reading from",
                    (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        }
        fprintf(f, "\n--- Faulting thread stack ---\n");
        write_thread_stack(f, process, thread, *ep->ContextRecord);
        fprintf(f, "\n(If this shows raw addresses instead of names, the .pdb isn't\n"
                    " sitting next to the exe, or doesn't match this build.)\n");
        fclose(f);
    }

    SymCleanup(process);
    return EXCEPTION_EXECUTE_HANDLER;  // let it terminate after logging
}

} // namespace crash_handler_detail

inline void install_crash_handler() {
    SetUnhandledExceptionFilter(crash_handler_detail::top_level_filter);
}

#else

inline void install_crash_handler() {}

#endif
