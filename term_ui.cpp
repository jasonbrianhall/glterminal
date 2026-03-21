#include "term_ui.h"
#include "term_pty.h"
#include "ft_font.h"
#include "gl_renderer.h"
#include "term_color.h"
#include "gl_terminal.h"
#include "gl_bouncingcircle.h"
#include "kitty_graphics.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>    // readlink, setsid, execl, _exit, fork
#  include <sys/types.h> // pid_t
#endif

#include "fight_mode.h"
#include "crt_audio.h"
#include "font_manager.h"

// ============================================================================
// URL DETECTION
// ============================================================================

struct UrlSpan {
    int row, col_start, col_end;  // col_end is inclusive
    std::string url;   // display text
    std::string href;  // actual href (may prepend https:// for www. links)
};

static std::vector<UrlSpan> s_urls;
static int s_hovered_url = -1;  // index into s_urls, or -1

static bool is_url_char(char c) {
    // Characters valid inside a URL (not terminal punctuation)
    return (c > ' ') && c != '"' && c != '\'' && c != '<' && c != '>' && c != '`';
}

static bool starts_with(const std::string &s, const char *prefix) {
    size_t plen = strlen(prefix);
    return s.size() >= plen && s.compare(0, plen, prefix) == 0;
}

// Strip trailing punctuation that is likely not part of the URL
static std::string trim_url(const std::string &s) {
    size_t end = s.size();
    // Strip paired closers if their opener isn't in the URL
    while (end > 0) {
        char c = s[end-1];
        if (c == ')' && s.find('(') == std::string::npos) { end--; continue; }
        if (c == ']' && s.find('[') == std::string::npos) { end--; continue; }
        if (c == '}' && s.find('{') == std::string::npos) { end--; continue; }
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') { end--; continue; }
        break;
    }
    return s.substr(0, end);
}

// Scan the visible grid and rebuild s_urls
static void detect_urls(Terminal *t,
                         std::function<Cell*(int row, int col)> resolve_cell) {
    s_urls.clear();
    const char *prefixes[] = { "https://", "http://", "ftp://", "file://", "www.", nullptr };

    for (int row = 0; row < t->rows; row++) {
        // Build a plain-text string for this row
        std::string line;
        line.reserve(t->cols);
        for (int col = 0; col < t->cols; col++) {
            Cell *c = resolve_cell(row, col);
            uint32_t cp = c->cp;
            if (!cp) cp = ' ';
            // Only handle ASCII for URL scanning simplicity
            if (cp < 0x80) line += (char)cp;
            else            line += '?';  // non-ASCII placeholder keeps column alignment
        }

        size_t pos = 0;
        while (pos < (size_t)t->cols) {
            // Find earliest prefix match from pos
            size_t best = std::string::npos;
            for (int pi = 0; prefixes[pi]; pi++) {
                size_t f = line.find(prefixes[pi], pos);
                if (f < best) best = f;
            }
            if (best == std::string::npos) break;

            // Scan forward to end of URL
            size_t end = best;
            while (end < (size_t)t->cols && is_url_char(line[end])) end++;

            std::string raw = line.substr(best, end - best);
            std::string url = trim_url(raw);
            size_t min_len = starts_with(url, "www.") ? 6 : 8; // www.x.com minimum
            if (url.size() > min_len) {
                UrlSpan span;
                span.row       = row;
                span.col_start = (int)best;
                span.col_end   = (int)(best + url.size() - 1);
                span.url       = url;
                span.href      = starts_with(url, "www.") ? "https://" + url : url;
                s_urls.push_back(span);
            }
            pos = end;
        }
    }
}

static int url_at(int row, int col) {
    for (int i = 0; i < (int)s_urls.size(); i++) {
        const UrlSpan &u = s_urls[i];
        if (u.row == row && col >= u.col_start && col <= u.col_end)
            return i;
    }
    return -1;
}

void open_url(const std::string &url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
        _exit(1);
    }
#endif
}

extern int  g_font_size;
extern bool g_blink_text_on;

// ============================================================================
// MENU FONT — always DejaVu Regular at a fixed size, unaffected by the user's
// shell font choice or zoom level.
// ============================================================================

#define MENU_FONT_SIZE 14

// Dedicated FreeType face for menu rendering.  Created once from the embedded
// DejaVu Regular data via ft_make_menu_face(), so it is never affected by
// font switching or zoom changes.
static FT_Face s_menu_face = nullptr;

static void ensure_menu_face() {
    if (s_menu_face) return;
    s_menu_face = ft_make_menu_face(MENU_FONT_SIZE);
}

// Call at shutdown to free the menu face's own buffer.
void menu_font_shutdown() {
    if (s_menu_face) {
        // ft_make_menu_face stores the buffer in generic.data with a finalizer
        free(s_menu_face->generic.data);
        s_menu_face->generic.data = nullptr;
        FT_Done_Face(s_menu_face);
        s_menu_face = nullptr;
    }
}

// Render menu text using the fixed DejaVu face at MENU_FONT_SIZE.
// Temporarily swaps the global faces so draw_text() picks them up, then
// restores everything afterwards.
static float draw_text_menu(const char *text, float x, float y,
                            float r, float g, float b, float a, uint8_t attrs = 0) {
    ensure_menu_face();
    if (!s_menu_face)
        return draw_text(text, x, y, MENU_FONT_SIZE, MENU_FONT_SIZE, r, g, b, a, attrs);

    // Swap all four shell faces for the menu face (bold/obl variants fall back
    // to the same menu face, which is fine for a UI menu).
    FT_Face saved      = s_ft_face;
    FT_Face saved_reg  = s_ft_face_reg;
    FT_Face saved_obl  = s_ft_face_obl;
    FT_Face saved_bobl = s_ft_face_bobl;
    s_ft_face      = s_menu_face;
    s_ft_face_reg  = s_menu_face;
    s_ft_face_obl  = s_menu_face;
    s_ft_face_bobl = s_menu_face;

    float ret = draw_text(text, x, y, MENU_FONT_SIZE, MENU_FONT_SIZE, r, g, b, a, attrs);

    s_ft_face      = saved;
    s_ft_face_reg  = saved_reg;
    s_ft_face_obl  = saved_obl;
    s_ft_face_bobl = saved_bobl;
    return ret;
}

// ============================================================================
// CONTEXT MENU DATA
// ============================================================================

static const float OPACITY_LEVELS[] = { 1.0f, 0.85f, 0.7f, 0.5f, 0.3f, 0.1f };
static const char* OPACITY_NAMES[]  = { "100%", "85%", "70%", "50%", "30%", "10%" };
static const int   OPACITY_COUNT    = 6;

static const char* RENDER_MODE_NAMES[] = { "Normal", "CRT", "LCD", "VHS", "Focus", "Commodore 64", "Bad Composite", "Bloom", "Ghosting", "Wireframe" };
static_assert(sizeof(RENDER_MODE_NAMES)/sizeof(RENDER_MODE_NAMES[0]) == RENDER_MODE_COUNT,
              "RENDER_MODE_NAMES count mismatch");

const MenuItem MENU_ITEMS[] = {
    { "New Terminal  >", false },
    { nullptr,           true  },
    { "Copy",            false },
    { "Copy as HTML",    false },
    { "Copy as ANSI",    false },
    { "Paste",           false },
    { nullptr,           true  },
    { "Reset",           false },
    { nullptr,           true  },
    { "Color Theme  >",  false },
    { "Transparency  >", false },
    { "Render Mode  >",  false },
    { "Entertainment >", false },
    { nullptr,           true  },
    { "Select All",      false },
    { nullptr,           true  },
    { "Font  >",         false },
    { nullptr,           true  },
    { "Help",            false },
    { nullptr,           true  },
    { "Quit",            false },
};
const int MENU_COUNT = (int)(sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0]));

ContextMenu g_menu = {};

extern float g_opacity;

// ============================================================================
// SELECTION HELPERS
// ============================================================================

void pixel_to_cell(Terminal *t, int px, int py, int ox, int oy, int *row, int *col) {
    *col = (int)((px - ox) / t->cell_w);
    *row = (int)((py - oy) / t->cell_h);
    if (*col < 0) *col = 0;
    if (*row < 0) *row = 0;
    if (*col >= t->cols) *col = t->cols - 1;
    if (*row >= t->rows) *row = t->rows - 1;
    *row += t->sb_count - t->sb_offset;
}

bool cell_in_sel(Terminal *t, int r, int c) {
    if (!t->sel_exists && !t->sel_active) return false;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }
    if (r < r0 || r > r1) return false;
    if (r == r0 && c < c0) return false;
    if (r == r1 && c > c1) return false;
    return true;
}

// ============================================================================
// URL HOVER / HIT TEST
// ============================================================================

static int pixel_to_render_row(Terminal *t, int py, int oy) {
    int row = (int)((py - oy) / t->cell_h);
    if (row < 0) row = 0;
    if (row >= t->rows) row = t->rows - 1;
    return row;
}
static int pixel_to_render_col(Terminal *t, int px, int ox) {
    int col = (int)((px - ox) / t->cell_w);
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    return col;
}

bool url_update_hover(Terminal *t, int mouse_px, int mouse_py, int ox, int oy) {
    int row = pixel_to_render_row(t, mouse_py, oy);
    int col = pixel_to_render_col(t, mouse_px, ox);
    int uid = url_at(row, col);
    if (uid != s_hovered_url) {
        s_hovered_url = uid;
        return true;
    }
    return false;
}

std::string url_at_pixel(Terminal *t, int mouse_px, int mouse_py, int ox, int oy) {
    int row = pixel_to_render_row(t, mouse_py, oy);
    int col = pixel_to_render_col(t, mouse_px, ox);
    int uid = url_at(row, col);
    if (uid >= 0) return s_urls[uid].href;
    return {};
}

// ============================================================================
// CLIPBOARD
// ============================================================================

void term_select_all(Terminal *t) {
    t->sel_start_row = 0;
    t->sel_start_col = 0;
    t->sel_end_row   = t->sb_count + t->rows - 1;
    t->sel_end_col   = t->cols - 1;
    t->sel_active    = false;
    t->sel_exists    = true;
}

void term_copy_selection(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }
    int bufsize = (r1 - r0 + 1) * (t->cols + 1) + 1;
    char *buf = (char*)malloc(bufsize);
    int pos = 0;
    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }
        for (int c = cs; c <= last_nonspace; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (!cp) cp = ' ';
            if      (cp < 0x80)    { buf[pos++] = (char)cp; }
            else if (cp < 0x800)   { buf[pos++]=(char)(0xC0|(cp>>6)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000) { buf[pos++]=(char)(0xE0|(cp>>12)); buf[pos++]=(char)(0x80|((cp>>6)&0x3F)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
            else { buf[pos++]=(char)(0xF0|(cp>>18)); buf[pos++]=(char)(0x80|((cp>>12)&0x3F)); buf[pos++]=(char)(0x80|((cp>>6)&0x3F)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
        }
        if (r < r1) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    SDL_SetClipboardText(buf);
    free(buf);
    //SDL_Log("[Term] copied %d chars to clipboard\n", pos);
}

static void append_hex_color(std::string &s, float r, float g, float b) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x",
             (int)(r*255+.5f), (int)(g*255+.5f), (int)(b*255+.5f));
    s += buf;
}

static void append_html_char(std::string &s, uint32_t cp) {
    if      (cp == '<')  s += "&lt;";
    else if (cp == '>')  s += "&gt;";
    else if (cp == '&')  s += "&amp;";
    else if (cp == '"')  s += "&quot;";
    else if (cp < 0x80)  s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0|(cp>>6)); s += (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0|(cp>>12)); s += (char)(0x80|((cp>>6)&0x3F)); s += (char)(0x80|(cp&0x3F)); }
    else { s+=(char)(0xF0|(cp>>18)); s+=(char)(0x80|((cp>>12)&0x3F)); s+=(char)(0x80|((cp>>6)&0x3F)); s+=(char)(0x80|(cp&0x3F)); }
}

void term_copy_selection_html(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) { int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc; }

    const Theme &th = THEMES[g_theme_idx];
    char bg_hex[8], fg_hex[8];
    snprintf(bg_hex, sizeof(bg_hex), "#%02x%02x%02x",
             (int)(th.bg_r*255+.5f), (int)(th.bg_g*255+.5f), (int)(th.bg_b*255+.5f));
    // Default foreground from palette index 7
    TermColor deffg = tcolor_resolve(TCOLOR_PALETTE(7));
    snprintf(fg_hex, sizeof(fg_hex), "#%02x%02x%02x",
             (int)(deffg.r*255+.5f), (int)(deffg.g*255+.5f), (int)(deffg.b*255+.5f));

    std::string html;
    html.reserve(8192);
    html += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\">\n";
    html += "<title>Terminal — "; html += th.name; html += "</title>\n";
    html += "<style>\n";
    html += "  body { margin: 0; padding: 0; background: "; html += bg_hex; html += "; }\n";
    html += "  .terminal {\n";
    html += "    background: "; html += bg_hex; html += ";\n";
    html += "    color: "; html += fg_hex; html += ";\n";
    html += "    font-family: 'DejaVu Sans Mono', 'Cascadia Code', 'Fira Code', 'Consolas', monospace;\n";
    html += "    font-size: 14px;\n";
    html += "    line-height: 1.4;\n";
    html += "    white-space: pre;\n";
    html += "    overflow-x: auto;\n";
    html += "  }\n";
    html += "  .terminal a {\n";
    html += "    color: #6ab0f5;\n";
    html += "    text-decoration: underline;\n";
    html += "  }\n";
    html += "  .terminal a:hover { color: #9dcfff; }\n";
    html += "</style>\n</head>\n<body>\n<div class=\"terminal\">";

    // Collect any kitty images that fall within the selection row range
    std::vector<KittyHtmlImage> kimages = kitty_get_html_images(t, r0, r1);
    int kimg_idx = 0;

    TermColorVal last_fg = ~(TermColorVal)0, last_bg = ~(TermColorVal)0;
    uint8_t last_attrs = 0xFF;
    bool span_open = false;
    auto close_span = [&]() { if (span_open) { html += "</span>"; span_open = false; } };

    // Build a helper to detect URLs in a row of cells (selection-relative row index)
    auto row_url_spans = [&](int r) -> std::vector<UrlSpan> {
        std::vector<UrlSpan> spans;
        const char *prefixes[] = { "https://", "http://", "ftp://", "file://", "www.", nullptr };
        std::string line;
        line.reserve(t->cols);
        for (int col = 0; col < t->cols; col++) {
            uint32_t cp = vcell(t, r, col)->cp;
            if (!cp) cp = ' ';
            line += (cp < 0x80) ? (char)cp : '?';
        }
        size_t pos = 0;
        while (pos < line.size()) {
            size_t best = std::string::npos;
            for (int pi = 0; prefixes[pi]; pi++) {
                size_t f = line.find(prefixes[pi], pos);
                if (f < best) best = f;
            }
            if (best == std::string::npos) break;
            size_t end = best;
            while (end < line.size() && is_url_char(line[end])) end++;
            std::string raw = line.substr(best, end - best);
            std::string url = trim_url(raw);
            size_t min_len = starts_with(url, "www.") ? 6 : 8;
            if (url.size() > min_len) {
                UrlSpan sp;
                sp.row = r; sp.col_start = (int)best; sp.col_end = (int)(best + url.size() - 1);
                sp.url = url;
                sp.href = starts_with(url, "www.") ? "https://" + url : url;
                spans.push_back(sp);
            }
            pos = end;
        }
        return spans;
    };

    for (int r = r0; r <= r1; r++) {
        // Insert any kitty images that start on this row (before the text line)
        while (kimg_idx < (int)kimages.size() && kimages[kimg_idx].y_cell == r) {
            close_span();
            html += kimages[kimg_idx].img_tag;
            kimg_idx++;
        }

        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }

        std::vector<UrlSpan> row_urls = row_url_spans(r);
        bool link_open = false;
        auto close_link = [&]() { if (link_open) { html += "</a>"; link_open = false; } };

        for (int c = cs; c <= last_nonspace; c++) {
            Cell *cellp = vcell(t,r,c);
            uint32_t cp = cellp->cp ? cellp->cp : ' ';
            TermColorVal fg = cellp->fg, bg = cellp->bg;
            uint8_t attrs = cellp->attrs;
            if (attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }

            // Check if we're entering or leaving a URL span
            int uid = -1;
            for (int ui = 0; ui < (int)row_urls.size(); ui++) {
                if (c >= row_urls[ui].col_start && c <= row_urls[ui].col_end) { uid = ui; break; }
            }
            bool in_url = (uid >= 0);
            bool at_url_start = in_url && (c == row_urls[uid].col_start);
            bool past_url     = link_open && !in_url;
            if (past_url) { close_span(); close_link(); }
            if (at_url_start) {
                close_span();
                close_link();
                html += "<a href=\"";
                // HTML-escape the href
                for (char ch : row_urls[uid].href) {
                    if (ch == '"') html += "&quot;";
                    else           html += ch;
                }
                html += "\" >";
                link_open = true;
                // Reset span state so colors are re-emitted inside the link
                last_fg = ~(TermColorVal)0; last_bg = ~(TermColorVal)0; last_attrs = 0xFF;
            }

            if (fg != last_fg || bg != last_bg || attrs != last_attrs) {
                close_span();
                TermColor fc = tcolor_resolve(fg);
                TermColor bc = tcolor_resolve(bg);

                // Only emit background if it differs from the theme background
                bool fg_is_default = !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) == 7;
                bool bg_is_default = !TCOLOR_IS_RGB(bg) && TCOLOR_IDX(bg) == 0;
                if (!bg_is_default) {
                    bg_is_default = (fabsf(bc.r - th.bg_r) < 0.01f &&
                                     fabsf(bc.g - th.bg_g) < 0.01f &&
                                     fabsf(bc.b - th.bg_b) < 0.01f);
                }

                std::string style;
                if (!fg_is_default) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                             (int)(fc.r*255+.5f), (int)(fc.g*255+.5f), (int)(fc.b*255+.5f));
                    style += "color:"; style += hex;
                }
                if (!bg_is_default) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                             (int)(bc.r*255+.5f), (int)(bc.g*255+.5f), (int)(bc.b*255+.5f));
                    if (!style.empty()) style += ";";
                    style += "background:"; style += hex;
                }
                if (attrs & ATTR_BOLD)      { if (!style.empty()) style += ";"; style += "font-weight:bold"; }
                if (attrs & ATTR_ITALIC)    { if (!style.empty()) style += ";"; style += "font-style:italic"; }
                if (attrs & ATTR_UNDERLINE) { if (!style.empty()) style += ";"; style += "text-decoration:underline"; }

                if (!style.empty()) {
                    html += "<span style=\""; html += style; html += "\">";
                    span_open = true;
                }
                last_fg = fg; last_bg = bg; last_attrs = attrs;
            }
            append_html_char(html, cp);
        }
        close_span();
        close_link();
        last_fg = ~(TermColorVal)0;
        if (r < r1) html += '\n';
    }
    html += "</div>\n</body>\n</html>\n";
    SDL_SetClipboardText(html.c_str());
}

void term_copy_selection_ansi(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) { int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc; }

    std::string out;
    out.reserve(4096);
    TermColorVal last_fg = ~(TermColorVal)0, last_bg = ~(TermColorVal)0;
    uint8_t last_attrs = 0xFF;

    auto emit_sgr = [&](TermColorVal fg, TermColorVal bg, uint8_t attrs) {
        out += "\x1b[0";
        if (attrs & ATTR_BOLD)      out += ";1";
        if (attrs & ATTR_UNDERLINE) out += ";4";
        if (attrs & ATTR_BLINK)     out += ";5";
        if (TCOLOR_IS_RGB(fg)) {
            char buf[32]; snprintf(buf,sizeof(buf),";38;2;%d;%d;%d",(int)TCOLOR_R(fg),(int)TCOLOR_G(fg),(int)TCOLOR_B(fg)); out+=buf;
        } else {
            int idx=(int)TCOLOR_IDX(fg);
            if (idx<8)       { char b[8]; snprintf(b,sizeof(b),";3%d",idx);     out+=b; }
            else if (idx<16) { char b[8]; snprintf(b,sizeof(b),";9%d",idx-8);   out+=b; }
            else             { char b[16];snprintf(b,sizeof(b),";38;5;%d",idx); out+=b; }
        }
        if (TCOLOR_IS_RGB(bg)) {
            char buf[32]; snprintf(buf,sizeof(buf),";48;2;%d;%d;%d",(int)TCOLOR_R(bg),(int)TCOLOR_G(bg),(int)TCOLOR_B(bg)); out+=buf;
        } else {
            int idx=(int)TCOLOR_IDX(bg);
            if (idx<8)       { char b[8]; snprintf(b,sizeof(b),";4%d",idx);      out+=b; }
            else if (idx<16) { char b[8]; snprintf(b,sizeof(b),";10%d",idx-8);   out+=b; }
            else             { char b[16];snprintf(b,sizeof(b),";48;5;%d",idx);  out+=b; }
        }
        out += 'm';
        last_fg = fg; last_bg = bg; last_attrs = attrs;
    };

    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }
        for (int c = cs; c <= last_nonspace; c++) {
            Cell *cellp = vcell(t,r,c);
            uint32_t cp = cellp->cp ? cellp->cp : ' ';
            TermColorVal fg = cellp->fg, bg = cellp->bg;
            uint8_t attrs = cellp->attrs;
            if (attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            if (fg != last_fg || bg != last_bg || attrs != last_attrs)
                emit_sgr(fg, bg, attrs);
            if      (cp < 0x80)    out += (char)cp;
            else if (cp < 0x800)   { out+=(char)(0xC0|(cp>>6)); out+=(char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000) { out+=(char)(0xE0|(cp>>12)); out+=(char)(0x80|((cp>>6)&0x3F)); out+=(char)(0x80|(cp&0x3F)); }
            else { out+=(char)(0xF0|(cp>>18)); out+=(char)(0x80|((cp>>12)&0x3F)); out+=(char)(0x80|((cp>>6)&0x3F)); out+=(char)(0x80|(cp&0x3F)); }
        }
        out += "\x1b[0m";
        last_fg = ~(TermColorVal)0;
        if (r < r1) out += '\n';
    }
    SDL_SetClipboardText(out.c_str());
    //SDL_Log("[Term] copied %zu bytes of ANSI to clipboard\n", out.size());
}

void term_paste(Terminal *t) {
    if (!SDL_HasClipboardText()) return;
    char *text = SDL_GetClipboardText();
    if (text && text[0]) {
        if (t->bracketed_paste) term_write(t, "\x1b[200~", 6);
        term_write(t, text, (int)strlen(text));
        if (t->bracketed_paste) term_write(t, "\x1b[201~", 6);
    }
    SDL_free(text);
}

// ============================================================================
// RENDERING
// ============================================================================

void term_render(Terminal *t, int ox, int oy) {
    float cw = t->cell_w, ch = t->cell_h;
    bool scrolled = (t->sb_offset > 0);

    Cell blank = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    auto resolve_cell = [&](int row, int col) -> Cell* {
        if (scrolled) {
            int sb_row_idx = t->sb_count - t->sb_offset + row;
            if (sb_row_idx < 0) return &blank;
            if (sb_row_idx < t->sb_count) return sb_row(t, sb_row_idx) + col;
            int live_row = sb_row_idx - t->sb_count;
            return (live_row < t->rows) ? &CELL(t, live_row, col) : &blank;
        }
        return &CELL(t, row, col);
    };

    // Detect URLs in visible content
    detect_urls(t, resolve_cell);

    // Pass 1: backgrounds — only redraw rows that changed
    for (int row = 0; row < t->rows; row++) {
        if (!term_row_is_dirty(t, row)) continue;
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor bc = tcolor_resolve(bg);
            int vrow = row + t->sb_count - t->sb_offset;
            if (cell_in_sel(t, vrow, col)) {
                draw_rect(px, py, cw, ch, 0.3f, 0.5f, 1.0f, 0.5f);
            } else {
                draw_rect(px, py, cw, ch, bc.r, bc.g, bc.b, 1.f);
            }
        }
    }

    // Pass 2: glyphs and decorations — only redraw rows that changed
    int dirty_cells = 0;
    for (int row = 0; row < t->rows; row++) {
        if (!term_row_is_dirty(t, row)) continue;
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor fc = tcolor_resolve(fg);
            if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
                fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg)+8));
            if (c->attrs & ATTR_DIM) { fc.r *= 0.5f; fc.g *= 0.5f; fc.b *= 0.5f; }

            uint32_t cp = c->cp;
            bool blink_hidden = (c->attrs & ATTR_BLINK) && !g_blink_text_on;
            if (cp && cp != ' ' && !blink_hidden) {
                char tmp[5] = {};
                cp_to_utf8(cp, tmp);
                float baseline = py + ch * 0.82f;
                draw_text(tmp, px, baseline, g_font_size, (int)ch, fc.r, fc.g, fc.b, 1.f, c->attrs);
                dirty_cells++;
            }
            if ((c->attrs & ATTR_UNDERLINE) && !blink_hidden)
                draw_rect(px, py+ch-2, cw, 2, fc.r, fc.g, fc.b, 1.f);
            if ((c->attrs & ATTR_STRIKE) && !blink_hidden)
                draw_rect(px, py+ch*0.45f, cw, 1, fc.r, fc.g, fc.b, 1.f);
            if ((c->attrs & ATTR_OVERLINE) && !blink_hidden)
                draw_rect(px, py+1, cw, 1, fc.r, fc.g, fc.b, 1.f);

            // URL underline
            int uid = url_at(row, col);
            if (uid >= 0) {
                bool hovered = (uid == s_hovered_url);
                float ur = hovered ? 0.4f : 0.35f;
                float ug = hovered ? 0.8f : 0.6f;
                float ub = hovered ? 1.0f : 0.9f;
                float uh = hovered ? 2.f : 1.f;
                draw_rect(px, py + ch - uh - 1, cw, uh, ur, ug, ub, 1.f);
            }
        }
    }

    // Clear dirty flags — rows are now up-to-date in the term FBO
    term_clear_dirty(t);

    // Feed glyph activity to CRT audio buzz
    crt_audio_set_activity((float)dirty_cells / (float)(t->cols * t->rows));

    // Kitty graphics — render placed images over the glyph layer
    kitty_render(t, ox, oy);

    // Cursor
    if (!scrolled && t->cursor_on) {
        float cx = ox + t->cur_col * cw;
        float cy = oy + t->cur_row * ch;
        switch (t->cursor_shape) {
        case 0: draw_rect(cx, cy, cw, ch, 1,1,1, 0.3f); break;
        case 2: draw_rect(cx, cy, 2, ch, 1,1,1, 0.85f); break;
        default: draw_rect(cx, cy+ch-3, cw, 3, 1,1,1, 0.85f); break;
        }
    }

    // Scrollbar
    if (scrolled && t->sb_count > 0) {
        float win_h   = t->rows * ch;
        int   total_rows = t->sb_count + t->rows;
        float bar_h   = win_h * t->rows / total_rows;
        if (bar_h < 8) bar_h = 8;
        float bar_y   = oy + (win_h - bar_h) * (float)(total_rows - t->rows - t->sb_offset) / (total_rows - t->rows);
        float bar_x   = ox + t->cols * cw - 4;
        draw_rect(bar_x, oy, 4, win_h, 0,0,0, 0.3f);
        draw_rect(bar_x, bar_y, 4, bar_h, 0.6f, 0.6f, 0.7f, 0.8f);
    }
}

// ============================================================================
// KEYBOARD
// ============================================================================

void handle_key(Terminal *t, SDL_Keysym ks, const char *text) {
    SDL_Keymod mod = (SDL_Keymod)ks.mod;
    bool shift = (mod & KMOD_SHIFT) != 0;
    bool ctrl  = (mod & KMOD_CTRL)  != 0;
    bool alt   = (mod & KMOD_ALT)   != 0;

    if (ctrl && shift) {
        if (ks.sym == SDLK_v) { term_paste(t); return; }
        if (ks.sym == SDLK_c) { return; }
    }

    if (ctrl && ks.sym == SDLK_a) { term_select_all(t); return; }

    auto arrow = [&](const char *normal, const char *app, char letter) {
        if (!shift && !ctrl && !alt) {
            term_write(t, t->app_cursor_keys ? app : normal, 3);
        } else {
            int m = 1 + (shift?1:0) + (alt?2:0) + (ctrl?4:0);
            char seq[16];
            int n = snprintf(seq, sizeof(seq), "\x1b[1;%d%c", m, letter);
            term_write(t, seq, n);
        }
    };

    switch (ks.sym) {
    case SDLK_UP:    arrow("\x1b[A", "\x1bOA", 'A'); crt_audio_cursor_ping(); return;
    case SDLK_DOWN:  arrow("\x1b[B", "\x1bOB", 'B'); crt_audio_cursor_ping(); return;
    case SDLK_RIGHT: arrow("\x1b[C", "\x1bOC", 'C'); crt_audio_cursor_ping(); return;
    case SDLK_LEFT:  arrow("\x1b[D", "\x1bOD", 'D'); crt_audio_cursor_ping(); return;
    case SDLK_HOME:  term_write(t, t->app_cursor_keys ? "\x1bOH" : "\x1b[H", 3); crt_audio_cursor_ping(); return;
    case SDLK_END:   term_write(t, t->app_cursor_keys ? "\x1bOF" : "\x1b[F", 3); crt_audio_cursor_ping(); return;
    default: break;
    }

    switch (ks.sym) {
    case SDLK_RETURN:    term_write(t, "\r",      1); break;
    case SDLK_BACKSPACE: term_write(t, "\x7f",    1); break;
    case SDLK_TAB:
        if (shift) term_write(t, "\x1b[Z", 3);
        else       term_write(t, "\t",     1);
        break;
    case SDLK_ESCAPE:    term_write(t, "\x1b",    1); break;
    case SDLK_INSERT:    term_write(t, "\x1b[2~", 4); break;
    case SDLK_DELETE:    term_write(t, "\x1b[3~", 4); break;
    case SDLK_PAGEUP:    term_write(t, "\x1b[5~", 4); break;
    case SDLK_PAGEDOWN:  term_write(t, "\x1b[6~", 4); break;
    case SDLK_F1:   term_write(t, "\x1bOP",   3); break;
    case SDLK_F2:   term_write(t, "\x1bOQ",   3); break;
    case SDLK_F3:   term_write(t, "\x1bOR",   3); break;
    case SDLK_F4:   term_write(t, "\x1bOS",   3); break;
    case SDLK_F5:   term_write(t, "\x1b[15~", 5); break;
    case SDLK_F6:   term_write(t, "\x1b[17~", 5); break;
    case SDLK_F7:   term_write(t, "\x1b[18~", 5); break;
    case SDLK_F8:   term_write(t, "\x1b[19~", 5); break;
    case SDLK_F9:   term_write(t, "\x1b[20~", 5); break;
    case SDLK_F10:  term_write(t, "\x1b[21~", 5); break;
    case SDLK_F11:  term_write(t, "\x1b[23~", 5); break;
    case SDLK_F12:  term_write(t, "\x1b[24~", 5); break;
    default:
        if (ctrl && ks.sym >= SDLK_a && ks.sym <= SDLK_z) {
            char c = (char)(ks.sym - SDLK_a + 1);
            if (alt) term_write(t, "\x1b", 1);
            term_write(t, &c, 1);
        } else if (alt && text && text[0]) {
            term_write(t, "\x1b", 1);
            term_write(t, text, (int)strlen(text));
        } else if (text && text[0]) {
            term_write(t, text, (int)strlen(text));
        }
        break;
    }
}

// ============================================================================
// CONTEXT MENU
// ============================================================================

static void menu_layout(ContextMenu *m, int /*font_size*/) {
    m->item_h = (int)(MENU_FONT_SIZE * 1.8f);
    m->sep_h  = 8;
    m->pad_x  = (int)(MENU_FONT_SIZE * 0.8f);
    m->width  = (int)(MENU_FONT_SIZE * 14.0f);
}

static int menu_total_height(ContextMenu *m) {
    int h = 8;
    for (int i = 0; i < MENU_COUNT; i++)
        h += MENU_ITEMS[i].separator ? m->sep_h : m->item_h;
    return h;
}

void menu_open(ContextMenu *m, int x, int y, int win_w, int win_h) {
    menu_layout(m, 0);
    m->visible = true; m->hovered = -1; m->sub_open = -1; m->sub_hovered = -1;
    int th = menu_total_height(m);
    m->x = SDL_min(x, win_w - m->width - 2);
    m->y = SDL_min(y, win_h - th - 2);
    if (m->x < 0) m->x = 0;
    if (m->y < 0) m->y = 0;
}

int menu_hit(ContextMenu *m, int px, int py) {
    if (!m->visible) return -1;
    if (px < m->x || px > m->x + m->width) return -1;
    int y = m->y + 4;
    for (int i = 0; i < MENU_COUNT; i++) {
        int h = MENU_ITEMS[i].separator ? m->sep_h : m->item_h;
        if (!MENU_ITEMS[i].separator && py >= y && py < y + h) return i;
        y += h;
    }
    return -1;
}

int submenu_hit(ContextMenu *m, int px, int py) {
    if (m->sub_open < 0) return -1;
    if (px < m->sub_x || px > m->sub_x + m->sub_w) return -1;
    if (py < m->sub_y || py > m->sub_y + m->sub_h) return -1;
    int count = (m->sub_open == MENU_ID_THEMES)        ? THEME_COUNT :
                (m->sub_open == MENU_ID_RENDER_MODE)    ? RENDER_MODE_COUNT :
                (m->sub_open == MENU_ID_ENTERTAINMENT)  ? ENT_COUNT :
                (m->sub_open == MENU_ID_NEW_TERMINAL)   ? NEW_TERM_COUNT :
                (m->sub_open == MENU_ID_FONTS)          ? (int)g_font_list.size() :
                                                          OPACITY_COUNT;
    int idx = (py - m->sub_y) / m->item_h;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

static void draw_menu_panel(float mx, float my, float mw, float mh) {
    draw_rect(mx+3, my+3, mw, mh, 0,0,0, 0.35f);
    draw_rect(mx, my, mw, mh, 0.13f, 0.13f, 0.16f, 0.96f);
    draw_rect(mx, my, mw, 1, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx, my+mh-1, mw, 1, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx, my, 1, mh, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx+mw-1, my, 1, mh, 0.35f,0.35f,0.45f, 1.f);
}

void menu_render(ContextMenu *m) {
    if (!m->visible) return;
    menu_layout(m, 0);
    int th = menu_total_height(m);
    float mx = (float)m->x, my = (float)m->y, mw = (float)m->width;

    draw_menu_panel(mx, my, mw, (float)th);

    float y = my + 4;
    for (int i = 0; i < MENU_COUNT; i++) {
        if (MENU_ITEMS[i].separator) {
            draw_rect(mx+4, y + m->sep_h*0.5f, mw-8, 1, 0.35f,0.35f,0.45f, 1.f);
            y += m->sep_h; continue;
        }
        float ih = (float)m->item_h;
        bool hov = (i == m->hovered);
        bool sub_open = (m->sub_open == i);
        if (hov || sub_open) draw_rect(mx+2, y, mw-4, ih, 0.25f, 0.45f, 0.85f, 0.85f);
        float tr = (hov||sub_open)?1.f:0.88f, tg=tr, tb=(hov||sub_open)?1.f:0.92f;
        draw_text_menu(MENU_ITEMS[i].label, mx + m->pad_x, y + ih*0.72f, tr,tg,tb,1.f);
        y += ih;
    }

    if (m->sub_open == MENU_ID_THEMES || m->sub_open == MENU_ID_OPACITY ||
        m->sub_open == MENU_ID_RENDER_MODE || m->sub_open == MENU_ID_ENTERTAINMENT ||
        m->sub_open == MENU_ID_NEW_TERMINAL || m->sub_open == MENU_ID_FONTS) {
        int count = (m->sub_open == MENU_ID_THEMES)       ? THEME_COUNT :
                    (m->sub_open == MENU_ID_RENDER_MODE)   ? RENDER_MODE_COUNT :
                    (m->sub_open == MENU_ID_ENTERTAINMENT) ? ENT_COUNT :
                    (m->sub_open == MENU_ID_NEW_TERMINAL)  ? NEW_TERM_COUNT :
                    (m->sub_open == MENU_ID_FONTS)         ? (int)g_font_list.size() :
                                                             OPACITY_COUNT;
        float sw = (float)(m->width + (int)(MENU_FONT_SIZE * 2));
        float sh = (float)(count * m->item_h + 8);
        float sx = (float)m->sub_x, sy = (float)m->sub_y;
        m->sub_w = (int)sw; m->sub_h = (int)sh;
        draw_menu_panel(sx, sy, sw, sh);

        static const char *ENT_NAMES[]      = { "Fight Mode", "Bouncing Circle", "Sound" };
        static const char *NEW_TERM_NAMES[] = { "Local Terminal", "SSH Session" };

        for (int j = 0; j < count; j++) {
            const char *lbl = (m->sub_open == MENU_ID_THEMES)        ? THEMES[j].name :
                              (m->sub_open == MENU_ID_RENDER_MODE)    ? RENDER_MODE_NAMES[j] :
                              (m->sub_open == MENU_ID_ENTERTAINMENT)  ? ENT_NAMES[j] :
                              (m->sub_open == MENU_ID_NEW_TERMINAL)   ? NEW_TERM_NAMES[j] :
                              (m->sub_open == MENU_ID_FONTS)          ? g_font_list[j].display_name.c_str() :
                                                                        OPACITY_NAMES[j];
            float iy = sy + 4 + j * m->item_h, ih = (float)m->item_h;
            bool hov = (j == m->sub_hovered);

            bool active = false;
            if (m->sub_open == MENU_ID_THEMES)
                active = (j == g_theme_idx);
            else if (m->sub_open == MENU_ID_FONTS)
                active = (j == g_font_index);
            else if (m->sub_open == MENU_ID_RENDER_MODE)
                active = (j == RENDER_MODE_NORMAL) ? (g_render_mode == 0)
                                                   : ((g_render_mode & (1u << j)) != 0);
            else if (m->sub_open == MENU_ID_OPACITY)
                active = (fabsf(OPACITY_LEVELS[j] - g_opacity) < 0.01f);
            else if (m->sub_open == MENU_ID_ENTERTAINMENT)
                active = (j == ENT_IDX_FIGHT    && fight_get_enabled())   ||
                         (j == ENT_IDX_BOUNCING  && bc_get_enabled())      ||
                         (j == ENT_IDX_SOUND     && term_audio_get_enabled());

            if (hov)          draw_rect(sx+2,iy,sw-4,ih,0.25f,0.45f,0.85f,0.85f);
            if (active&&!hov) draw_rect(sx+2,iy,sw-4,ih,0.2f,0.35f,0.6f,0.6f);
            float tr=hov?1.f:(active?0.7f:0.88f), tg=hov?1.f:(active?0.9f:0.88f), tb=hov?1.f:(active?1.0f:0.92f);
            if (active) draw_text_menu("\xe2\x9c\x93", sx+4, iy+ih*0.72f, 0.4f,0.8f,0.4f,1.f);
            draw_text_menu(lbl, sx + m->pad_x + MENU_FONT_SIZE, iy+ih*0.72f, tr,tg,tb,1.f);
        }
    }

    // Menu is drawn after gl_end_frame (to stay outside post-process), so we
    // must flush the accumulator ourselves — there's no automatic flush after this.
    gl_flush_verts();
}

// ============================================================================
// ACTIONS
// ============================================================================

void action_new_terminal() {
#ifdef _WIN32
    char self[512] = {};
    if (!GetModuleFileNameA(nullptr, self, sizeof(self)-1)) return;
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    CreateProcessA(self, nullptr, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(self, self, nullptr); _exit(1); }
#endif
}

void action_new_ssh_session() {
#ifdef _WIN32
    char self[512] = {};
    if (!GetModuleFileNameA(nullptr, self, sizeof(self)-1)) return;
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "\"%s\" --ssh", self);
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(self, self, "--ssh", nullptr); _exit(1); }
#endif
}

// ============================================================================
// HELP OVERLAY
// ============================================================================

bool g_help_visible = false;

struct HelpRow {
    const char *header;  // non-null = section header row; key/desc/url ignored
    const char *key;
    const char *desc;
    const char *url;     // non-null = desc is a clickable hyperlink opening this URL
};

static const HelpRow HELP_ROWS[] = {
    { "── General ──",            nullptr, nullptr, nullptr },
    { nullptr, "F1",              "Toggle this help screen",                      nullptr },
    { nullptr, "F5",              "Eye of Felix image viewer (local or remote via SSH)", nullptr },
    { nullptr, "F11",             "Toggle fullscreen",                            nullptr },
    { nullptr, "Right-click",     "Open context menu",                            nullptr },
    { nullptr, "Ctrl+A",          "Select all",                                   nullptr },
    { nullptr, "Ctrl+C",          "Copy selection",                               nullptr },
    { nullptr, "Ctrl+Shift+C",    "Copy selection as HTML",                       nullptr },
    { nullptr, "Ctrl+V",          "Paste",                                        nullptr },
    { nullptr, "Ctrl+Scroll",     "Zoom font in / out",                           nullptr },
    { nullptr, "Shift+PgUp/Dn",   "Scroll through scrollback buffer",             nullptr },
    { nullptr, "Ctrl+click URL",  "Open hyperlink in browser",                    nullptr },

    { "── SSH ──",                nullptr, nullptr, nullptr },
    { nullptr, "--ssh [user@host]","Connect to a remote host via SSH",            nullptr },
#ifdef USESSH
    { nullptr, "F2",              "SFTP upload overlay",                          nullptr },
    { nullptr, "F3",              "SFTP download overlay",                        nullptr },
    { nullptr, "F4",              "Interactive SFTP console",                     nullptr },
#endif

    { "── Appearance ──",         nullptr, nullptr, nullptr },
    { nullptr, "Render Mode",     "Post-process visual effects (right-click menu)",nullptr },
    { nullptr, "Color Theme",     "Built-in colour schemes (right-click menu)",   nullptr },
    { nullptr, "Transparency",    "Adjust window opacity (right-click menu)",     nullptr },
    { nullptr, "Font",            "Switch monospace font (right-click menu)",     nullptr },

    { "── Entertainment ──",      nullptr, nullptr, nullptr },
    { nullptr, "Entertainment",   "Fight mode, bouncing ball, CRT audio (right-click menu)", nullptr },

    { "── Project ──",            nullptr, nullptr, nullptr },
    { nullptr, "Source & License","https://github.com/jasonbrianhall/glterminal  (MIT)",
                                   "https://github.com/jasonbrianhall/glterminal" },
    { nullptr, "Buy Me a Coffee", "https://buymeacoffee.com/jasonbrianhall",
                                   "https://buymeacoffee.com/jasonbrianhall"      },
};
static const int HELP_ROW_COUNT = (int)(sizeof(HELP_ROWS)/sizeof(HELP_ROWS[0]));

// Layout constants
#define HELP_KEY_COL_W  (MENU_FONT_SIZE * 15)
#define HELP_DESC_COL_W (MENU_FONT_SIZE * 30)
#define HELP_PAD        (MENU_FONT_SIZE)
#define HELP_ROW_H      (int)(MENU_FONT_SIZE * 1.7f)
#define HELP_HDR_H      (int)(MENU_FONT_SIZE * 2.0f)

// Hit-rects for clickable links — rebuilt each frame
struct HelpLinkRect { int x, y, w, h; const char *url; };
static std::vector<HelpLinkRect> s_help_links;
static int s_help_hovered_link = -1;  // index into s_help_links, or -1

void help_render(int win_w, int win_h) {
    if (!g_help_visible) return;

    ensure_menu_face();
    s_help_links.clear();

    // --- layout ---
    int total_w = HELP_KEY_COL_W + HELP_DESC_COL_W + HELP_PAD * 3;

    int total_h = HELP_PAD + HELP_HDR_H;
    for (int i = 0; i < HELP_ROW_COUNT; i++)
        total_h += HELP_ROWS[i].header ? HELP_HDR_H : HELP_ROW_H;
    total_h += HELP_PAD;

    int ox = (win_w - total_w) / 2;
    int oy = (win_h - total_h) / 2;
    if (ox < 4) ox = 4;
    if (oy < 4) oy = 4;

    // --- dim backdrop ---
    draw_rect(0, 0, (float)win_w, (float)win_h, 0.f, 0.f, 0.f, 0.55f);

    // --- panel shadow + body ---
    draw_rect((float)(ox+3), (float)(oy+3), (float)total_w, (float)total_h, 0,0,0, 0.4f);
    draw_rect((float)ox, (float)oy, (float)total_w, (float)total_h, 0.10f, 0.10f, 0.13f, 0.97f);
    draw_rect((float)ox,             (float)oy,             (float)total_w, 1,              0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)ox,             (float)(oy+total_h-1), (float)total_w, 1,              0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)ox,             (float)oy,             1,              (float)total_h, 0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)(ox+total_w-1), (float)oy,             1,              (float)total_h, 0.35f,0.35f,0.55f, 1.f);

    // --- title bar ---
    float ty = (float)oy + HELP_PAD;
    draw_rect((float)ox+1, ty, (float)total_w-2, (float)HELP_HDR_H, 0.18f, 0.25f, 0.45f, 1.f);

    const char *title = "GL Terminal — Help";
    draw_text_menu(title,
                   (float)(ox + total_w/2) - (float)(strlen(title) * MENU_FONT_SIZE * 0.3f),
                   ty + HELP_HDR_H * 0.72f,
                   1.f, 1.f, 1.f, 1.f, ATTR_BOLD);

    const char *hint = "Esc or F1 to close";
    float hint_x = (float)(ox + total_w) - (float)(strlen(hint) * MENU_FONT_SIZE * 0.56f) - HELP_PAD;
    draw_text_menu(hint, hint_x, ty + HELP_HDR_H * 0.72f, 0.50f, 0.50f, 0.62f, 1.f);

    ty += HELP_HDR_H;

    // --- rows ---
    for (int i = 0; i < HELP_ROW_COUNT; i++) {
        const HelpRow &row = HELP_ROWS[i];
        if (row.header) {
            float rh = (float)HELP_HDR_H;
            draw_rect((float)ox+1, ty, (float)total_w-2, rh, 0.14f, 0.14f, 0.20f, 1.f);
            draw_rect((float)ox+1, ty + rh - 1.f, (float)total_w-2, 1.f, 0.28f, 0.28f, 0.42f, 1.f);
            draw_text_menu(row.header, (float)(ox + HELP_PAD), ty + rh * 0.72f,
                           0.55f, 0.75f, 1.0f, 1.f, ATTR_BOLD);
            ty += rh;
        } else {
            float rh = (float)HELP_ROW_H;
            if (i % 2 == 0)
                draw_rect((float)ox+1, ty, (float)total_w-2, rh, 0.f,0.f,0.f, 0.12f);

            // key column
            draw_text_menu(row.key, (float)(ox + HELP_PAD), ty + rh * 0.72f,
                           0.85f, 0.95f, 1.0f, 1.f);

            // desc / link column
            float dx = (float)(ox + HELP_PAD + HELP_KEY_COL_W);
            if (row.url) {
                // Find if this link is hovered
                int link_idx = (int)s_help_links.size();
                bool hov = (s_help_hovered_link == link_idx);

                // Highlight bar on hover
                if (hov)
                    draw_rect(dx - 2.f, ty + 1.f, (float)HELP_DESC_COL_W, rh - 2.f,
                              0.25f, 0.45f, 0.85f, 0.25f);

                float lr = hov ? 0.55f : 0.40f;
                float lg = hov ? 0.85f : 0.65f;
                float lb = 1.0f;
                draw_text_menu(row.desc, dx, ty + rh * 0.72f, lr, lg, lb, 1.f, ATTR_UNDERLINE);

                // Record hit-rect
                HelpLinkRect lr2;
                lr2.x   = (int)dx - 2;
                lr2.y   = (int)ty;
                lr2.w   = HELP_DESC_COL_W;
                lr2.h   = (int)rh;
                lr2.url = row.url;
                s_help_links.push_back(lr2);
            } else {
                draw_text_menu(row.desc, dx, ty + rh * 0.72f, 0.78f, 0.78f, 0.82f, 1.f);
            }
            ty += rh;
        }
    }

    gl_flush_verts();
}

bool help_keydown(SDL_Keycode sym) {
    if (!g_help_visible) return false;
    if (sym == SDLK_ESCAPE || sym == SDLK_F1) {
        g_help_visible = false;
        return true;
    }
    return true;  // swallow all keys while help is open
}

// Returns true if the mouse event was consumed.
bool help_mousedown(int x, int y) {
    if (!g_help_visible) return false;
    for (auto &lr : s_help_links) {
        if (x >= lr.x && x < lr.x + lr.w && y >= lr.y && y < lr.y + lr.h) {
            open_url(lr.url);
            return true;
        }
    }
    // Click outside a link closes the overlay
    g_help_visible = false;
    return true;
}

// Call from SDL_MOUSEMOTION to update hover state. Returns true if redraw needed.
bool help_mousemotion(int x, int y) {
    if (!g_help_visible) return false;
    int prev = s_help_hovered_link;
    s_help_hovered_link = -1;
    for (int i = 0; i < (int)s_help_links.size(); i++) {
        auto &lr = s_help_links[i];
        if (x >= lr.x && x < lr.x + lr.w && y >= lr.y && y < lr.y + lr.h) {
            s_help_hovered_link = i;
            break;
        }
    }
    return s_help_hovered_link != prev;
}
