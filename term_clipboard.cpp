// term_clipboard.cpp — rich (HTML + plain text) clipboard copy.
//
// SDL2's SDL_SetClipboardText() only offers plain text, so Word/LibreOffice
// lose all formatting. This offers BOTH flavors at once:
//   Windows: CF_UNICODETEXT + "HTML Format" (CF_HTML)
//   Linux/X11: a tiny detached helper process owns CLIPBOARD and serves
//              text/html + UTF8_STRING (link with -lX11)
// Anything else falls back to plain SDL_SetClipboardText().

#include "term_clipboard.h"
#include "terminal.h"
#include "term_color.h"
#include "gl_terminal.h"

#include <SDL2/SDL.h>
#include <string>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#  include <SDL2/SDL_syswm.h>
extern SDL_Window *g_sdl_window;
#elif defined(__linux__)
#  include <X11/Xlib.h>
#  include <X11/Xatom.h>
#  include <unistd.h>
#  include <sys/wait.h>
#endif

// ============================================================================
// SELECTION -> TEXT / HTML
// ============================================================================

static void append_utf8(std::string &s, uint32_t cp) {
    if      (cp < 0x80)    s += (char)cp;
    else if (cp < 0x800)   { s += (char)(0xC0|(cp>>6)); s += (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0|(cp>>12)); s += (char)(0x80|((cp>>6)&0x3F)); s += (char)(0x80|(cp&0x3F)); }
    else { s += (char)(0xF0|(cp>>18)); s += (char)(0x80|((cp>>12)&0x3F)); s += (char)(0x80|((cp>>6)&0x3F)); s += (char)(0x80|(cp&0x3F)); }
}

static void append_escaped(std::string &s, uint32_t cp) {
    if      (cp == '<') s += "&lt;";
    else if (cp == '>') s += "&gt;";
    else if (cp == '&') s += "&amp;";
    else                append_utf8(s, cp);
}

static void hex_of(char out[8], TermColor c) {
    snprintf(out, 8, "#%02x%02x%02x",
             (int)(c.r*255+.5f), (int)(c.g*255+.5f), (int)(c.b*255+.5f));
}

// Normalized selection bounds; returns false if nothing is selected.
static bool sel_bounds(Terminal *t, int &r0, int &c0, int &r1, int &c1) {
    if (!t->sel_exists && !t->sel_active) return false;
    r0 = t->sel_start_row; c0 = t->sel_start_col;
    r1 = t->sel_end_row;   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) { int a=r0,b=c0; r0=r1; c0=c1; r1=a; c1=b; }
    return true;
}

static int last_nonspace_col(Terminal *t, int r, int cs, int ce) {
    int last = cs - 1;
    for (int c = cs; c <= ce; c++) {
        uint32_t cp = vcell(t, r, c)->cp;
        if (cp && cp != ' ') last = c;
    }
    return last;
}

static std::string selection_to_text(Terminal *t) {
    std::string out;
    int r0, c0, r1, c1;
    if (!sel_bounds(t, r0, c0, r1, c1)) return out;
    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last = last_nonspace_col(t, r, cs, ce);
        for (int c = cs; c <= last; c++) {
            uint32_t cp = vcell(t, r, c)->cp;
            append_utf8(out, cp ? cp : ' ');
        }
        if (r < r1) out += '\n';
    }
    return out;
}

// An HTML fragment (<pre> block) with inline styles on every run, so
// Word/LibreOffice keep colors even if they drop the <pre> styling.
static std::string selection_to_html_fragment(Terminal *t) {
    std::string h;
    int r0, c0, r1, c1;
    if (!sel_bounds(t, r0, c0, r1, c1)) return h;

    // No background and no default text color: the paste inherits the
    // document's own colors (black on white), keeping only explicit colors.
    h += "<pre style=\"font-family:'DejaVu Sans Mono',Consolas,'Courier New',monospace;"
         "font-size:10pt;margin:0\">";

    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last = last_nonspace_col(t, r, cs, ce);

        std::string cur_style;
        bool open = false;
        for (int c = cs; c <= last; c++) {
            Cell *cell = vcell(t, r, c);
            uint32_t cp = cell->cp ? cell->cp : ' ';
            TermColorVal fg = cell->fg;
            uint8_t a = cell->attrs;
            if (a & ATTR_REVERSE) fg = cell->bg;  // reversed: text takes the cell's bg color

            // Default foreground (palette 7) is left uncolored so it pastes as
            // the document's normal text color instead of light gray/white.
            bool fg_default = !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) == 7;

            std::string style;
            if (!fg_default) {
                // Match the on-screen rendering rules in term_render()
                TermColor fc = tcolor_resolve(fg);
                if ((a & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
                    fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg) + 8));
                if (a & ATTR_DIM) { fc.r *= 0.5f; fc.g *= 0.5f; fc.b *= 0.5f; }
                char hex[8];
                hex_of(hex, fc);
                style += "color:"; style += hex;
            } else if (a & ATTR_DIM) {
                style += "color:#808080";
            }
            auto add = [&style](const char *prop) {
                if (!style.empty()) style += ';';
                style += prop;
            };
            if (a & ATTR_BOLD)   add("font-weight:bold");
            if (a & ATTR_ITALIC) add("font-style:italic");
            std::string deco;
            if (a & ATTR_UNDERLINE) deco += " underline";
            if (a & ATTR_STRIKE)    deco += " line-through";
            if (a & ATTR_OVERLINE)  deco += " overline";
            if (!deco.empty()) add(("text-decoration:" + deco.substr(1)).c_str());

            if (!open || style != cur_style) {
                if (open) h += "</span>";
                if (style.empty()) h += "<span>";
                else { h += "<span style=\""; h += style; h += "\">"; }
                cur_style = style;
                open = true;
            }
            append_escaped(h, cp);
        }
        if (open) h += "</span>";
        if (r < r1) h += '\n';
    }
    h += "</pre>";
    return h;
}

// ============================================================================
// PLATFORM CLIPBOARD
// ============================================================================

#ifdef _WIN32

static bool clipboard_set_rich(const std::string &plain, const std::string &frag) {
    // CF_HTML requires a header with byte offsets into the payload.
    const std::string pre  = "<html><head><meta charset=\"utf-8\"></head><body>\r\n<!--StartFragment-->";
    const std::string post = "<!--EndFragment-->\r\n</body></html>";
    const char *fmt = "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\n"
                      "StartFragment:%010d\r\nEndFragment:%010d\r\n";
    int hdr_len    = snprintf(nullptr, 0, fmt, 0, 0, 0, 0);
    int start_frag = hdr_len + (int)pre.size();
    int end_frag   = start_frag + (int)frag.size();
    int end_html   = end_frag + (int)post.size();
    char hdr[160];
    snprintf(hdr, sizeof(hdr), fmt, hdr_len, end_html, start_frag, end_frag);
    std::string cf = std::string(hdr) + pre + frag + post;

    HWND hwnd = nullptr;
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (g_sdl_window && SDL_GetWindowWMInfo(g_sdl_window, &wm))
        hwnd = wm.info.win.window;
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();

    static UINT cf_html = RegisterClipboardFormatA("HTML Format");
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, cf.size() + 1)) {
        memcpy(GlobalLock(g), cf.c_str(), cf.size() + 1);
        GlobalUnlock(g);
        SetClipboardData(cf_html, g);
    }

    std::string crlf;
    crlf.reserve(plain.size() + 64);
    for (char ch : plain) { if (ch == '\n') crlf += '\r'; crlf += ch; }
    int wn = MultiByteToWideChar(CP_UTF8, 0, crlf.c_str(), -1, nullptr, 0);
    if (wn > 0) {
        if (HGLOBAL w = GlobalAlloc(GMEM_MOVEABLE, wn * sizeof(wchar_t))) {
            MultiByteToWideChar(CP_UTF8, 0, crlf.c_str(), -1, (wchar_t *)GlobalLock(w), wn);
            GlobalUnlock(w);
            SetClipboardData(CF_UNICODETEXT, w);
        }
    }
    CloseClipboard();
    return true;
}

#elif defined(__linux__)

// Runs in a detached helper process: owns CLIPBOARD until another app
// (or our own SDL_SetClipboardText) takes it, then exits. Also means the
// copied text survives the terminal being closed.
static void x11_clip_serve(const std::string &plain, const std::string &html) {
    Display *d = XOpenDisplay(nullptr);
    if (!d) return;
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
    Atom CLIPBOARD = XInternAtom(d, "CLIPBOARD", False);
    Atom TARGETS   = XInternAtom(d, "TARGETS", False);
    Atom UTF8      = XInternAtom(d, "UTF8_STRING", False);
    Atom TEXT      = XInternAtom(d, "TEXT", False);
    Atom PLAIN     = XInternAtom(d, "text/plain;charset=utf-8", False);
    Atom HTML      = XInternAtom(d, "text/html", False);

    XSetSelectionOwner(d, CLIPBOARD, w, CurrentTime);
    if (XGetSelectionOwner(d, CLIPBOARD) != w) { XCloseDisplay(d); return; }

    long max_req = XExtendedMaxRequestSize(d);
    if (!max_req) max_req = XMaxRequestSize(d);
    long max_bytes = max_req * 4 - 1024;  // no INCR support; huge selections refused

    for (;;) {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type == SelectionClear) break;
        if (ev.type != SelectionRequest) continue;

        XSelectionRequestEvent *rq = &ev.xselectionrequest;
        XSelectionEvent se = {};
        se.type      = SelectionNotify;
        se.display   = rq->display;
        se.requestor = rq->requestor;
        se.selection = rq->selection;
        se.target    = rq->target;
        se.time      = rq->time;
        se.property  = rq->property != None ? rq->property : rq->target;

        if (rq->target == TARGETS) {
            Atom list[] = { TARGETS, HTML, UTF8, PLAIN, XA_STRING, TEXT };
            XChangeProperty(d, rq->requestor, se.property, XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)list, (int)(sizeof(list) / sizeof(list[0])));
        } else {
            const std::string *src = nullptr;
            if (rq->target == HTML) src = &html;
            else if (rq->target == UTF8 || rq->target == PLAIN ||
                     rq->target == XA_STRING || rq->target == TEXT) src = &plain;
            if (src && (long)src->size() <= max_bytes) {
                Atom type = (rq->target == TEXT) ? UTF8 : rq->target;
                XChangeProperty(d, rq->requestor, se.property, type, 8, PropModeReplace,
                                (const unsigned char *)src->data(), (int)src->size());
            } else {
                se.property = None;
            }
        }
        XSendEvent(d, rq->requestor, False, 0, (XEvent *)&se);
        XFlush(d);
    }
    XCloseDisplay(d);
}

static bool clipboard_set_rich(const std::string &plain, const std::string &frag) {
    if (!getenv("DISPLAY")) return false;  // pure Wayland without XWayland
    std::string html = "<html><head><meta charset=\"utf-8\"></head><body>" + frag + "</body></html>";

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Double-fork so the helper is reparented to init (no zombies).
        if (fork() != 0) _exit(0);
        setsid();
        // Drop inherited fds — above all the PTY master, or the shell would
        // never get SIGHUP while the helper is alive.
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
        for (int fd = 3; fd < maxfd; fd++) close(fd);
        x11_clip_serve(plain, html);
        _exit(0);
    }
    waitpid(pid, nullptr, 0);
    return true;
}

#else

static bool clipboard_set_rich(const std::string &, const std::string &) { return false; }

#endif

// ============================================================================
// PUBLIC
// ============================================================================

void term_copy_selection_rich(Terminal *t) {
    std::string plain = selection_to_text(t);
    if (plain.empty()) return;
    std::string frag = selection_to_html_fragment(t);
    if (!clipboard_set_rich(plain, frag))
        SDL_SetClipboardText(plain.c_str());
}
