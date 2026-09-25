// term_clipboard.cpp — rich (HTML + plain text) clipboard copy.
//
// SDL2's SDL_SetClipboardText() only offers plain text, so Word/LibreOffice
// lose all formatting. This offers BOTH flavors at once:
//   Windows: CF_UNICODETEXT + "HTML Format" (CF_HTML)
//   Linux/X11: a tiny detached helper process owns CLIPBOARD and serves
//              text/html + UTF8_STRING (link with -lX11)
// Anything else falls back to plain SDL_SetClipboardText().
//
// Kitty and Sixel graphics inside the selection are included as images:
//   Linux:   embedded as data: URIs (LibreOffice embeds them on paste)
//   Windows: written to %TEMP%\FelixTerminalClip\ and referenced by file://
//            URL, since Word does not reliably accept data: URIs on paste
// When the selection is ONLY an image (no text), the PNG is also offered as
// a plain image (image/png on X11, "PNG" on Windows) for image editors.

#include "term_clipboard.h"
#include "terminal.h"
#include "term_color.h"
#include "gl_terminal.h"
#include "kitty_graphics.h"
#include "sixel_graphics.h"
#include "term_ui.h"          // term_row_links

#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <string.h>
#include <algorithm>
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

// Widest an image may paste at, in CSS px (6.5in text column at 96 dpi).
#define CLIP_IMG_MAX_W 624

// ============================================================================
// HELPERS
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

static std::string b64_encode(const std::vector<uint8_t> &src) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t len = src.size();
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)src[i] << 16;
        if (i+1 < len) b |= (uint32_t)src[i+1] << 8;
        if (i+2 < len) b |= (uint32_t)src[i+2];
        out += T[(b >> 18) & 0x3F];
        out += T[(b >> 12) & 0x3F];
        out += (i+1 < len) ? T[(b >> 6) & 0x3F] : '=';
        out += (i+2 < len) ? T[b & 0x3F] : '=';
    }
    return out;
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

// ============================================================================
// SELECTION -> TEXT
// ============================================================================

static std::string selection_to_text(Terminal *t, int r0, int c0, int r1, int c1) {
    std::string out;
    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last = last_nonspace_col(t, r, cs, ce);
        for (int c = cs; c <= last; c++) {
            if (cell_is_wide_tail(vcell(t, r, c))) continue;  // 2nd half of a wide char
            uint32_t cp = vcell(t, r, c)->cp;
            append_utf8(out, cp ? cp : ' ');
        }
        if (r < r1) out += '\n';
    }
    return out;
}

static bool text_is_blank(const std::string &s) {
    for (char ch : s) if (ch != ' ' && ch != '\n') return false;
    return true;
}

// ============================================================================
// IMAGES
// ============================================================================

struct ClipImage {
    KittyPngImage k;
    std::string   src;   // URL for <img src=...>
};

#ifdef _WIN32
// Percent-encode a UTF-8 path for use in a file:/// URL.
static std::string file_url_from_utf8(const std::string &path) {
    std::string url = "file:///";
    for (unsigned char ch : path) {
        if (ch == '\\') ch = '/';
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || strchr("-._~/:", ch)) {
            url += (char)ch;
        } else {
            char esc[4];
            snprintf(esc, sizeof(esc), "%%%02X", ch);
            url += esc;
        }
    }
    return url;
}

// Write PNGs to %TEMP%\FelixTerminalClip\ (cleared on each copy — the
// previous clipboard contents no longer need them) and return file URLs.
static bool win_write_temp_pngs(std::vector<ClipImage> &imgs) {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH - 32) return false;
    std::wstring dir = std::wstring(tmp) + L"FelixTerminalClip\\";
    CreateDirectoryW(dir.c_str(), nullptr);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { DeleteFileW((dir + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    DWORD tick = GetTickCount();
    for (size_t i = 0; i < imgs.size(); i++) {
        wchar_t name[64];
        _snwprintf(name, 64, L"clip_%lu_%u.png", (unsigned long)tick, (unsigned)i + 1);
        std::wstring wpath = dir + name;
        FILE *f = _wfopen(wpath.c_str(), L"wb");
        if (!f) return false;
        size_t wr = fwrite(imgs[i].k.png.data(), 1, imgs[i].k.png.size(), f);
        fclose(f);
        if (wr != imgs[i].k.png.size()) return false;

        int un = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string upath(un > 0 ? un - 1 : 0, '\0');
        if (un > 1) WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, &upath[0], un, nullptr, nullptr);
        imgs[i].src = file_url_from_utf8(upath);
    }
    return true;
}
#endif

static void assign_image_urls(std::vector<ClipImage> &imgs) {
#ifdef _WIN32
    if (win_write_temp_pngs(imgs)) return;
#endif
    for (ClipImage &ci : imgs)
        ci.src = "data:image/png;base64," + b64_encode(ci.k.png);
}

static std::string img_tag(const ClipImage &ci) {
    int w = ci.k.disp_w_px > 0 ? ci.k.disp_w_px : 1;
    int h = ci.k.disp_h_px > 0 ? ci.k.disp_h_px : 1;
    if (w > CLIP_IMG_MAX_W) { h = (int)((long long)h * CLIP_IMG_MAX_W / w); w = CLIP_IMG_MAX_W; }
    char dims[64];
    snprintf(dims, sizeof(dims), " width=\"%d\" height=\"%d\"", w, h);
    return "<img src=\"" + ci.src + "\"" + dims + " alt=\"[terminal image]\">";
}

// ============================================================================
// SELECTION -> HTML FRAGMENT
// ============================================================================

// A <pre> block with inline styles on every run, so Word/LibreOffice keep
// colors even if they drop the <pre> styling. No background and no default
// text color: the paste inherits the document's own colors.
static std::string selection_to_html_fragment(Terminal *t, int r0, int c0, int r1, int c1,
                                              const std::vector<ClipImage> &imgs) {
    std::vector<std::string> lines;
    size_t img_idx = 0;
    int covered_until = -1;  // last row covered by an already-emitted image

    for (int r = r0; r <= r1; r++) {
        // Images whose top edge is on this row go above the row's text
        while (img_idx < imgs.size() && imgs[img_idx].k.vrow <= r) {
            if (imgs[img_idx].k.vrow == r) {
                lines.push_back(img_tag(imgs[img_idx]));
                int end = r + imgs[img_idx].k.rows_used - 1;
                if (end > covered_until) covered_until = end;
            }
            img_idx++;
        }

        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last = last_nonspace_col(t, r, cs, ce);

        // Blank rows underneath an image are just the space it occupies on
        // screen — the <img> already takes that room, so drop them.
        if (r <= covered_until && last < cs) continue;

        std::string h;
        std::string cur_style;
        bool open = false;
        // Links on this row — OSC 8 and URLs in the text (www.google.com,
        // https://...) — become <a href> so Word/LibreOffice keep them clickable
        std::vector<TermLinkSpan> links = term_row_links(t, r);
        int cur_link = -1;       // index into links of the open <a>, or -1
        for (int c = cs; c <= last; c++) {
            Cell *cell = vcell(t, r, c);
            if (cell_is_wide_tail(cell)) continue;  // 2nd half of a wide char

            int lk = -1;
            for (int i = 0; i < (int)links.size(); i++)
                if (c >= links[i].col_start && c <= links[i].col_end) { lk = i; break; }
            if (lk != cur_link) {
                if (open) { h += "</span>"; open = false; }
                if (cur_link >= 0) h += "</a>";
                if (lk >= 0) {
                    h += "<a href=\"";
                    for (const char *q = links[lk].href.c_str(); *q; q++) {
                        if      (*q == '"') h += "&quot;";
                        else if (*q == '&') h += "&amp;";
                        else if (*q == '<') h += "&lt;";
                        else                h += *q;
                    }
                    h += "\">";
                }
                cur_link = lk;
            }
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
            if (a & ATTR_UNDERLINE) {
                static const char *ul_css[] = { nullptr, "double", "wavy", "dotted", "dashed" };
                int us = cell_ul_style(cell);
                if (us > 0 && us <= 4)
                    add((std::string("text-decoration-style:") + ul_css[us]).c_str());
                if (cell->ul_color) {
                    char hex[8];
                    hex_of(hex, tcolor_resolve(CELL_UL_COLOR(cell)));
                    add((std::string("text-decoration-color:") + hex).c_str());
                }
            }

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
        if (cur_link >= 0) h += "</a>";
        lines.push_back(std::move(h));
    }

    std::string out = "<pre style=\"font-family:'DejaVu Sans Mono',Consolas,'Courier New',monospace;"
                      "font-size:10pt;margin:0\">";
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += '\n';
        out += lines[i];
    }
    out += "</pre>";
    return out;
}

// ============================================================================
// PLATFORM CLIPBOARD
// ============================================================================

#ifdef _WIN32

static bool clipboard_set_rich(const std::string &plain, const std::string &frag,
                               const std::vector<uint8_t> *png) {
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

    if (png && !png->empty()) {
        static UINT cf_png = RegisterClipboardFormatA("PNG");
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, png->size())) {
            memcpy(GlobalLock(g), png->data(), png->size());
            GlobalUnlock(g);
            SetClipboardData(cf_png, g);
        }
    }

    if (!plain.empty()) {
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
    }
    CloseClipboard();
    return true;
}

#elif defined(__linux__)

// Runs in a detached helper process: owns CLIPBOARD until another app
// (or our own SDL_SetClipboardText) takes it, then exits. Also means the
// copied text survives the terminal being closed.
static void x11_clip_serve(const std::string &plain, const std::string &html,
                           const std::vector<uint8_t> &png) {
    Display *d = XOpenDisplay(nullptr);
    if (!d) return;
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
    Atom CLIPBOARD = XInternAtom(d, "CLIPBOARD", False);
    Atom TARGETS   = XInternAtom(d, "TARGETS", False);
    Atom UTF8      = XInternAtom(d, "UTF8_STRING", False);
    Atom TEXT      = XInternAtom(d, "TEXT", False);
    Atom PLAIN     = XInternAtom(d, "text/plain;charset=utf-8", False);
    Atom HTML      = XInternAtom(d, "text/html", False);
    Atom PNG       = XInternAtom(d, "image/png", False);

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
            Atom list[8]; int n = 0;
            list[n++] = TARGETS;
            list[n++] = HTML;
            if (!png.empty())   list[n++] = PNG;
            if (!plain.empty()) { list[n++] = UTF8; list[n++] = PLAIN; list[n++] = XA_STRING; list[n++] = TEXT; }
            XChangeProperty(d, rq->requestor, se.property, XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)list, n);
        } else {
            const unsigned char *data = nullptr;
            long size = 0;
            if (rq->target == HTML) {
                data = (const unsigned char *)html.data(); size = (long)html.size();
            } else if (rq->target == PNG && !png.empty()) {
                data = png.data(); size = (long)png.size();
            } else if (!plain.empty() &&
                       (rq->target == UTF8 || rq->target == PLAIN ||
                        rq->target == XA_STRING || rq->target == TEXT)) {
                data = (const unsigned char *)plain.data(); size = (long)plain.size();
            }
            if (data && size <= max_bytes) {
                Atom type = (rq->target == TEXT) ? UTF8 : rq->target;
                XChangeProperty(d, rq->requestor, se.property, type, 8, PropModeReplace,
                                data, (int)size);
            } else {
                se.property = None;
            }
        }
        XSendEvent(d, rq->requestor, False, 0, (XEvent *)&se);
        XFlush(d);
    }
    XCloseDisplay(d);
}

static bool clipboard_set_rich(const std::string &plain, const std::string &frag,
                               const std::vector<uint8_t> *png) {
    if (!getenv("DISPLAY")) return false;  // pure Wayland without XWayland
    std::string html = "<html><head><meta charset=\"utf-8\"></head><body>" + frag + "</body></html>";
    static const std::vector<uint8_t> no_png;

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
        x11_clip_serve(plain, html, png ? *png : no_png);
        _exit(0);
    }
    waitpid(pid, nullptr, 0);
    return true;
}

#else

static bool clipboard_set_rich(const std::string &, const std::string &,
                               const std::vector<uint8_t> *) { return false; }

#endif

// ============================================================================
// PUBLIC
// ============================================================================

void term_copy_selection_rich(Terminal *t) {
    int r0, c0, r1, c1;
    if (!sel_bounds(t, r0, c0, r1, c1)) return;

    std::string plain = selection_to_text(t, r0, c0, r1, c1);

    std::vector<ClipImage> imgs;
    for (KittyPngImage &k : kitty_get_png_images(t, r0, r1)) {
        ClipImage ci;
        ci.k = std::move(k);
        imgs.push_back(std::move(ci));
    }
    for (KittyPngImage &k : sixel_get_png_images(t, r0, r1)) {
        ClipImage ci;
        ci.k = std::move(k);
        imgs.push_back(std::move(ci));
    }
    std::stable_sort(imgs.begin(), imgs.end(),
        [](const ClipImage &a, const ClipImage &b){ return a.k.vrow < b.k.vrow; });

    bool blank = text_is_blank(plain);
    if (blank && imgs.empty()) return;
    if (blank) plain.clear();

    assign_image_urls(imgs);
    std::string frag = selection_to_html_fragment(t, r0, c0, r1, c1, imgs);

    // Image-only selection of a single image: also offer it as a bare PNG.
    const std::vector<uint8_t> *png = (blank && imgs.size() == 1) ? &imgs[0].k.png : nullptr;

    if (!clipboard_set_rich(plain, frag, png) && !plain.empty())
        SDL_SetClipboardText(plain.c_str());
}
