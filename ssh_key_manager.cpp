// ssh_key_manager.cpp — F8 SSH Key Manager overlay
//
// Lists keys found in ~/.ssh, shows type/bits/fingerprint/comment.
// Generate pane: Ed25519 or RSA-4096 via ssh-keygen.
// Actions: copy public key to clipboard, delete key pair.
// No libssh2 dependency — shells out to ssh-keygen via popen().

#include "ssh_key_manager.h"
#include "gl_terminal.h"
#include "gl_renderer.h"
#include "ft_font.h"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

#ifndef _WIN32
#  include <unistd.h>
#  include <sys/stat.h>
#  include <dirent.h>
#  include <pwd.h>
#else
#  include <windows.h>
#  include <shlobj.h>
#endif

// ============================================================================
// EXTERNAL REFS  (same pattern as sftp_console / term_ui)
// ============================================================================

extern int  g_font_size;

// draw_text / draw_rect / gl_flush_verts come from gl_renderer
// draw_text_menu lives in term_ui.cpp; we replicate the same font-swap trick
// via a small local wrapper that just calls draw_text with MENU_FONT_SIZE.
static constexpr int MFONT = 14;   // pixels — matches MENU_FONT_SIZE in term_ui.cpp

static float dt(const char *s, float x, float y,
                float r, float g, float b, float a, int attrs = 0)
{
    return draw_text(s, x, y, MFONT, MFONT, r, g, b, a, attrs);
}

// ============================================================================
// LAYOUT CONSTANTS
// ============================================================================

static constexpr int PAD       = 12;
static constexpr int TITLE_H   = 28;
static constexpr int ROW_H     = 22;
static constexpr int BTN_H     = 24;
static constexpr int BTN_W     = 110;
static constexpr int FIELD_H   = 22;
static constexpr int MIN_W     = 680;
static constexpr int MIN_H     = 420;

// ============================================================================
// STATE
// ============================================================================

SshKeyMgr g_ssh_key_mgr;

// ============================================================================
// HELPERS
// ============================================================================

static std::string ssh_home_dir()
{
#ifndef _WIN32
    const char *h = getenv("HOME");
    if (!h) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) h = pw->pw_dir;
    }
    return h ? (std::string(h) + "/.ssh") : "~/.ssh";
#else
    char buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, buf)))
        return std::string(buf) + "\\.ssh";
    return "";
#endif
}

// Run a command with popen and return stdout as string.
static std::string run_cmd(const char *cmd)
{
    std::string out;
#ifndef _WIN32
    FILE *fp = popen(cmd, "r");
#else
    FILE *fp = _popen(cmd, "r");
#endif
    if (!fp) return out;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp))
        out += buf;
#ifndef _WIN32
    pclose(fp);
#else
    _pclose(fp);
#endif
    return out;
}

// Trim trailing whitespace in-place.
static void rtrim(std::string &s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
}

// Parse "ssh-keygen -l -f <pubkey>" output:
//   256 SHA256:xxxx comment (ED25519)
static void parse_keygen_line(const std::string &line, SshKeyEntry &e)
{
    // bits
    const char *p = line.c_str();
    char *end = nullptr;
    long bits = strtol(p, &end, 10);
    if (end && end != p) {
        e.bits = (int)bits;
        p = end;
        while (*p == ' ') p++;
    }
    // fingerprint
    const char *sp = strchr(p, ' ');
    if (sp) {
        e.fingerprint = std::string(p, sp - p);
        p = sp + 1;
    }
    // comment (up to the parenthesised type)
    const char *paren = strrchr(p, '(');
    if (paren) {
        std::string comment(p, paren - p);
        rtrim(comment);
        e.comment = comment;
        // type
        const char *cp = paren + 1;
        const char *ep = strchr(cp, ')');
        if (ep) e.type = std::string(cp, ep - cp);
    } else {
        e.comment = p;
        rtrim(e.comment);
    }
}

// ============================================================================
// KEY SCANNING
// ============================================================================

static void scan_keys(SshKeyMgr &m)
{
    m.keys.clear();
    std::string sshdir = ssh_home_dir();

#ifndef _WIN32
    DIR *d = opendir(sshdir.c_str());
    if (!d) return;
    struct dirent *de;
    std::vector<std::string> pub_files;
    while ((de = readdir(d))) {
        std::string name(de->d_name);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".pub")
            pub_files.push_back(name);
    }
    closedir(d);
#else
    std::vector<std::string> pub_files;
    std::string pat = sshdir + "\\*.pub";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { pub_files.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif

    std::sort(pub_files.begin(), pub_files.end());

    for (auto &pf : pub_files) {
        std::string pub  = sshdir + "/" + pf;
        std::string priv = pub.substr(0, pub.size() - 4);

        // Check private key exists
#ifndef _WIN32
        struct stat st;
        if (stat(priv.c_str(), &st) != 0) continue;
#else
        if (GetFileAttributesA(priv.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
#endif

        SshKeyEntry e;
        e.pub_path  = pub;
        e.priv_path = priv;

        // Fingerprint + type via ssh-keygen -l
        std::string cmd = "ssh-keygen -l -f \"" + pub + "\" 2>/dev/null";
        std::string line = run_cmd(cmd.c_str());
        rtrim(line);
        if (!line.empty()) parse_keygen_line(line, e);
        if (e.type.empty()) e.type = "UNKNOWN";

        m.keys.push_back(e);
    }
}

// ============================================================================
// OPEN / CLOSE
// ============================================================================

void ssh_key_mgr_open(int /*win_w*/, int /*win_h*/)
{
    g_ssh_key_mgr.visible    = true;
    g_ssh_key_mgr.pane       = KeyMgrPane::LIST;
    g_ssh_key_mgr.status[0]  = '\0';
    scan_keys(g_ssh_key_mgr);
    if (g_ssh_key_mgr.selected >= (int)g_ssh_key_mgr.keys.size())
        g_ssh_key_mgr.selected = 0;
}

void ssh_key_mgr_close()
{
    g_ssh_key_mgr.visible = false;
}

// ============================================================================
// RENDER HELPERS
// ============================================================================

// Draw a bordered button; returns its rect for hit-testing.
struct Btn { float x, y, w, h; };
static Btn draw_btn(const char *label, float x, float y, float w, float h,
                    bool hovered, bool accent = false)
{
    float br = accent ? 0.20f : 0.15f,
          bg = accent ? 0.55f : 0.20f,
          bb = accent ? 0.25f : 0.55f;
    if (hovered) { br += 0.10f; bg += 0.10f; bb += 0.15f; }
    draw_rect(x, y, w, h, br, bg, bb, 0.90f);
    draw_rect(x, y, w, 1, br+0.15f, bg+0.15f, bb+0.20f, 1.f);
    draw_rect(x, y+h-1, w, 1, br*0.5f, bg*0.5f, bb*0.6f, 1.f);
    float tw = (float)(strlen(label) * MFONT) * 0.56f;
    dt(label, x + (w - tw) * 0.5f, y + h * 0.72f, 0.92f, 0.95f, 1.0f, 1.f, ATTR_BOLD);
    return { x, y, w, h };
}

static bool btn_hit(const Btn &b, int mx, int my)
{
    return mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h;
}

// Draw a labelled text input field; cursor shown when focused.
static void draw_field(const char *value, float x, float y, float w, float h,
                       bool focused, bool password = false)
{
    float br = focused ? 0.18f : 0.10f,
          bg = focused ? 0.22f : 0.12f,
          bb = focused ? 0.32f : 0.18f;
    draw_rect(x, y, w, h, br, bg, bb, 1.f);
    draw_rect(x, y,   w, 1, focused ? 0.40f : 0.25f,
                              focused ? 0.60f : 0.35f,
                              focused ? 0.90f : 0.55f, 1.f);
    draw_rect(x, y+h-1, w, 1, 0.10f, 0.12f, 0.18f, 1.f);

    std::string disp;
    if (password) {
        disp = std::string(strlen(value), '*');
    } else {
        disp = value;
    }
    dt(disp.c_str(), x + 6, y + h * 0.70f, 0.90f, 0.92f, 0.95f, 1.f);

    if (focused) {
        float cx = x + 6 + (float)(disp.size() * MFONT) * 0.56f;
        draw_rect(cx, y + 3, 1, h - 6, 0.70f, 0.85f, 1.0f, 0.80f);
    }
}

// ============================================================================
// RENDER — LIST PANE
// ============================================================================

static void render_list_pane(SshKeyMgr &m, float ox, float oy,
                              float pw, float ph, int win_w, int win_h)
{
    (void)win_w; (void)win_h;

    float y = oy + PAD;

    // Section title
    dt("SSH Keys  (~/.ssh)", ox + PAD, y + MFONT * 0.85f, 0.55f, 0.75f, 1.0f, 1.f, ATTR_BOLD);
    y += MFONT + PAD / 2;

    // Column headers
    float col_type = ox + PAD;
    float col_bits = col_type + 80;
    float col_fp   = col_bits + 52;
    float col_comment = col_fp + 250;

    draw_rect(ox + 1, y, pw - 2, ROW_H, 0.14f, 0.18f, 0.28f, 1.f);
    dt("Type",        col_type,    y + ROW_H * 0.68f, 0.55f, 0.65f, 0.85f, 1.f);
    dt("Bits",        col_bits,    y + ROW_H * 0.68f, 0.55f, 0.65f, 0.85f, 1.f);
    dt("Fingerprint", col_fp,      y + ROW_H * 0.68f, 0.55f, 0.65f, 0.85f, 1.f);
    dt("Comment",     col_comment, y + ROW_H * 0.68f, 0.55f, 0.65f, 0.85f, 1.f);
    y += ROW_H;

    // Compute visible rows
    float list_bottom = oy + ph - BTN_H - PAD * 3 - (m.status[0] ? MFONT + 4 : 0);
    int max_rows = (int)((list_bottom - y) / ROW_H);
    m.visible_rows = max_rows > 0 ? max_rows : 1;

    if (m.scroll_top > (int)m.keys.size() - m.visible_rows)
        m.scroll_top = std::max(0, (int)m.keys.size() - m.visible_rows);

    int show = std::min((int)m.keys.size() - m.scroll_top, m.visible_rows);

    if (m.keys.empty()) {
        dt("No SSH key pairs found in ~/.ssh",
           ox + PAD, y + ROW_H * 0.68f, 0.55f, 0.55f, 0.60f, 1.f);
    }

    for (int i = 0; i < show; i++) {
        int idx = m.scroll_top + i;
        const SshKeyEntry &e = m.keys[idx];
        bool sel = (idx == m.selected);

        if (sel)
            draw_rect(ox + 1, y, pw - 2, ROW_H, 0.22f, 0.40f, 0.78f, 0.55f);
        else if (i % 2 == 0)
            draw_rect(ox + 1, y, pw - 2, ROW_H, 0.f, 0.f, 0.f, 0.10f);

        float fr = sel ? 0.95f : 0.82f,
              fg = sel ? 0.97f : 0.88f,
              fb = sel ? 1.00f : 0.92f;

        dt(e.type.c_str(), col_type, y + ROW_H * 0.68f, fr, fg, fb, 1.f);

        char bits[16] = "-";
        if (e.bits > 0) snprintf(bits, sizeof(bits), "%d", e.bits);
        dt(bits, col_bits, y + ROW_H * 0.68f, fr * 0.85f, fg * 0.85f, fb, 1.f);

        // Fingerprint — truncate to fit column
        std::string fp = e.fingerprint;
        if (fp.size() > 38) fp = fp.substr(0, 38) + "…";
        dt(fp.c_str(), col_fp, y + ROW_H * 0.68f, fr * 0.75f, fg * 0.85f, fb, 1.f);

        std::string cm = e.comment;
        float max_cm = pw - (col_comment - ox) - PAD;
        while (cm.size() > 4 && (float)(cm.size() * MFONT) * 0.56f > max_cm)
            cm = cm.substr(0, cm.size() - 2) + "…";
        dt(cm.c_str(), col_comment, y + ROW_H * 0.68f, fr * 0.70f, fg * 0.80f, fb * 0.90f, 1.f);

        y += ROW_H;
    }

    // Scrollbar
    if ((int)m.keys.size() > m.visible_rows) {
        float sb_x  = ox + pw - 5;
        float sb_y0 = oy + PAD + MFONT + PAD / 2 + ROW_H;
        float sb_h  = (float)(m.visible_rows * ROW_H);
        float bar_h = sb_h * m.visible_rows / (float)m.keys.size();
        float bar_y = sb_y0 + (sb_h - bar_h) * m.scroll_top / (float)(m.keys.size() - m.visible_rows + 1);
        draw_rect(sb_x, sb_y0, 4, sb_h, 0.f, 0.f, 0.f, 0.25f);
        draw_rect(sb_x, bar_y, 4, bar_h, 0.40f, 0.60f, 0.90f, 0.75f);
    }

    // Action buttons (bottom row)
    float bx = ox + PAD;
    float by = oy + ph - BTN_H - PAD;

    bool has_key = !m.keys.empty() && m.selected < (int)m.keys.size();

    draw_btn("+ Generate",   bx,                  by, BTN_W, BTN_H, false, true);
    if (has_key) {
        draw_btn("Copy Pub Key", bx + BTN_W + 8,  by, BTN_W, BTN_H, false);
        draw_btn("Delete",       bx + (BTN_W+8)*2, by, BTN_W, BTN_H, false);
    }
    draw_btn("Close (F8)",   ox + pw - BTN_W - PAD, by, BTN_W, BTN_H, false);

    // Status line
    if (m.status[0]) {
        float sr = m.status_ok ? 0.30f : 0.85f;
        float sg = m.status_ok ? 0.85f : 0.30f;
        float sb2= m.status_ok ? 0.30f : 0.25f;
        dt(m.status, ox + PAD, by - MFONT - 2, sr, sg, sb2, 1.f);
    }
}

// ============================================================================
// RENDER — GENERATE PANE
// ============================================================================

static void render_generate_pane(SshKeyMgr &m, float ox, float oy,
                                 float pw, float ph, int /*win_w*/, int /*win_h*/)
{
    float y = oy + PAD;
    dt("Generate New SSH Key", ox + PAD, y + MFONT * 0.85f, 0.55f, 0.75f, 1.0f, 1.f, ATTR_BOLD);
    y += MFONT + PAD;

    // Key type radio
    dt("Key type:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    float rx = ox + PAD + 90;
    for (int t = 0; t < 2; t++) {
        bool sel = (m.gen_type == t);
        const char *lbl = (t == 0) ? "Ed25519 (recommended)" : "RSA-4096";
        draw_rect(rx, y + 2, 12, 12, 0.15f, 0.20f, 0.30f, 1.f);
        if (sel)
            draw_rect(rx + 3, y + 5, 6, 6, 0.40f, 0.75f, 1.0f, 1.f);
        dt(lbl, rx + 16, y + MFONT * 0.85f, sel ? 0.92f : 0.68f, sel ? 0.95f : 0.72f, 1.0f, 1.f);
        rx += (t == 0) ? 200 : 0;
    }
    y += ROW_H + 4;

    // Filename
    dt("Filename:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    draw_field(m.gen_name, ox + PAD + 90, y, pw - PAD * 2 - 90, FIELD_H, m.gen_focus == 0);
    y += FIELD_H + PAD;

    // Comment
    dt("Comment:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    draw_field(m.gen_comment, ox + PAD + 90, y, pw - PAD * 2 - 90, FIELD_H, m.gen_focus == 1);
    y += FIELD_H + PAD;

    // Passphrase
    dt("Passphrase:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    draw_field(m.gen_passphrase, ox + PAD + 90, y, pw - PAD * 2 - 90 - BTN_W/2 - 4, FIELD_H,
               m.gen_focus == 2, !m.gen_pass_show);
    draw_btn(m.gen_pass_show ? "Hide" : "Show",
             ox + pw - PAD - BTN_W / 2, y, BTN_W / 2, FIELD_H, false);
    y += FIELD_H + PAD;

    // Note about no-passphrase
    if (m.gen_passphrase[0] == '\0')
        dt("(empty = no passphrase)", ox + PAD + 90, y, 0.50f, 0.52f, 0.56f, 1.f);
    y += MFONT + PAD;

    // Buttons
    float bx = ox + PAD;
    float by = oy + ph - BTN_H - PAD;
    draw_btn("Generate", bx, by, BTN_W, BTN_H, false, true);
    draw_btn("← Back",  bx + BTN_W + 8, by, BTN_W, BTN_H, false);

    // Status
    if (m.status[0]) {
        float sr = m.status_ok ? 0.30f : 0.85f;
        float sg = m.status_ok ? 0.85f : 0.30f;
        dt(m.status, ox + PAD, by - MFONT - 2, sr, sg, 0.30f, 1.f);
    }
}

// ============================================================================
// RENDER — CONFIRM DELETE PANE
// ============================================================================

static void render_confirm_pane(SshKeyMgr &m, float ox, float oy,
                                float pw, float ph, int /*win_w*/, int /*win_h*/)
{
    float cy = oy + ph * 0.35f;
    dt("Delete key pair?", ox + pw / 2 - 80, cy, 0.90f, 0.35f, 0.30f, 1.f, ATTR_BOLD);
    cy += MFONT + PAD;

    if (!m.keys.empty() && m.selected < (int)m.keys.size()) {
        const auto &e = m.keys[m.selected];
        // Show just the filename
        std::string fn = e.priv_path;
        size_t sl = fn.rfind('/');
        if (sl != std::string::npos) fn = fn.substr(sl + 1);
        std::string msg = fn + "  +  " + fn + ".pub";
        float tw = (float)(msg.size() * MFONT) * 0.56f;
        dt(msg.c_str(), ox + (pw - tw) / 2, cy, 0.85f, 0.85f, 0.88f, 1.f);
    }

    float bx = ox + pw / 2 - BTN_W - 6;
    float by = oy + ph - BTN_H - PAD;
    draw_btn("Yes, Delete", bx,              by, BTN_W, BTN_H, false);
    draw_btn("Cancel",      bx + BTN_W + 12, by, BTN_W, BTN_H, false, true);
}

// ============================================================================
// MAIN RENDER
// ============================================================================

void ssh_key_mgr_render(int win_w, int win_h)
{
    SshKeyMgr &m = g_ssh_key_mgr;
    if (!m.visible) return;

    // Panel size
    float pw = (float)std::max(win_w - 120, MIN_W);
    float ph = (float)std::max(win_h - 120, MIN_H);
    float ox = (float)((win_w - (int)pw) / 2);
    float oy = (float)((win_h - (int)ph) / 2);

    // Dim backdrop
    draw_rect(0, 0, (float)win_w, (float)win_h, 0.f, 0.f, 0.f, 0.60f);

    // Shadow + body
    draw_rect(ox + 3, oy + 3, pw, ph, 0.f, 0.f, 0.f, 0.45f);
    draw_rect(ox, oy, pw, ph, 0.07f, 0.09f, 0.13f, 0.97f);

    // Border
    draw_rect(ox,      oy,      pw, 1,  0.30f, 0.45f, 0.75f, 1.f);
    draw_rect(ox,      oy+ph-1, pw, 1,  0.30f, 0.45f, 0.75f, 1.f);
    draw_rect(ox,      oy,      1,  ph, 0.30f, 0.45f, 0.75f, 1.f);
    draw_rect(ox+pw-1, oy,      1,  ph, 0.30f, 0.45f, 0.75f, 1.f);

    // Title bar
    draw_rect(ox + 1, oy + 1, pw - 2, TITLE_H, 0.12f, 0.18f, 0.35f, 1.f);
    draw_rect(ox + 1, oy + TITLE_H, pw - 2, 1, 0.25f, 0.40f, 0.70f, 1.f);
    dt("SSH Key Manager",
       ox + PAD, oy + TITLE_H * 0.72f,
       0.85f, 0.90f, 1.0f, 1.f, ATTR_BOLD);
    dt("F8 to close",
       ox + pw - 100, oy + TITLE_H * 0.72f,
       0.40f, 0.42f, 0.50f, 1.f);

    // Content starts below title bar
    float content_oy = oy + TITLE_H + 1;
    float content_ph = ph - TITLE_H - 1;

    switch (m.pane) {
    case KeyMgrPane::LIST:
        render_list_pane(m, ox, content_oy, pw, content_ph, win_w, win_h);
        break;
    case KeyMgrPane::GENERATE:
        render_generate_pane(m, ox, content_oy, pw, content_ph, win_w, win_h);
        break;
    case KeyMgrPane::CONFIRM_DELETE:
        render_confirm_pane(m, ox, content_oy, pw, content_ph, win_w, win_h);
        break;
    }

    gl_flush_verts();
}

// ============================================================================
// ACTIONS
// ============================================================================

static void action_copy_pubkey(SshKeyMgr &m)
{
    if (m.keys.empty() || m.selected >= (int)m.keys.size()) return;
    const SshKeyEntry &e = m.keys[m.selected];
    FILE *f = fopen(e.pub_path.c_str(), "r");
    if (!f) {
        snprintf(m.status, sizeof(m.status), "Error: cannot open %s", e.pub_path.c_str());
        m.status_ok = false;
        return;
    }
    char buf[4096] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    // Strip trailing newline for cleaner copy
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    SDL_SetClipboardText(buf);
    snprintf(m.status, sizeof(m.status), "Public key copied to clipboard.");
    m.status_ok = true;
}

static void action_delete_key(SshKeyMgr &m)
{
    if (m.keys.empty() || m.selected >= (int)m.keys.size()) return;
    const SshKeyEntry e = m.keys[m.selected];
    int r1 = remove(e.priv_path.c_str());
    int r2 = remove(e.pub_path.c_str());
    if (r1 == 0 || r2 == 0) {
        snprintf(m.status, sizeof(m.status), "Deleted %s", e.priv_path.c_str());
        m.status_ok = true;
    } else {
        snprintf(m.status, sizeof(m.status), "Error deleting key pair.");
        m.status_ok = false;
    }
    m.pane = KeyMgrPane::LIST;
    scan_keys(m);
    if (m.selected >= (int)m.keys.size())
        m.selected = (int)m.keys.size() - 1;
    if (m.selected < 0) m.selected = 0;
}

static void action_generate(SshKeyMgr &m)
{
    // Validate filename
    if (m.gen_name[0] == '\0') {
        snprintf(m.status, sizeof(m.status), "Error: filename is empty.");
        m.status_ok = false;
        return;
    }
    std::string sshdir = ssh_home_dir();
    std::string outpath = sshdir + "/" + m.gen_name;

    // Check if file already exists
    FILE *chk = fopen(outpath.c_str(), "r");
    if (chk) {
        fclose(chk);
        snprintf(m.status, sizeof(m.status),
                 "Error: %s already exists. Choose a different name.", m.gen_name);
        m.status_ok = false;
        return;
    }

    // Ensure ~/.ssh exists with correct permissions
#ifndef _WIN32
    mkdir(sshdir.c_str(), 0700);
#else
    CreateDirectoryA(sshdir.c_str(), nullptr);
#endif

    // Build ssh-keygen command
    // -t type -b bits (for RSA) -f path -C comment -N passphrase -q
    std::string type_flag  = (m.gen_type == 0) ? "-t ed25519" : "-t rsa -b 4096";
    std::string comment    = m.gen_comment[0] ? m.gen_comment : (std::string(m.gen_name));
    std::string passphrase = m.gen_passphrase;

    // Escape single quotes in passphrase
    std::string pp_esc;
    for (char c : passphrase) {
        if (c == '\'') pp_esc += "'\\''";
        else           pp_esc += c;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ssh-keygen %s -f '%s' -C '%s' -N '%s' -q 2>&1",
             type_flag.c_str(), outpath.c_str(), comment.c_str(), pp_esc.c_str());

    std::string out = run_cmd(cmd);
    rtrim(out);

    // Check success: private key file should now exist
    FILE *fchk = fopen(outpath.c_str(), "r");
    if (fchk) {
        fclose(fchk);
        snprintf(m.status, sizeof(m.status),
                 "Generated: ~/.ssh/%s", m.gen_name);
        m.status_ok = true;
        // Reset generate fields
        if (m.gen_type == 0) strncpy(m.gen_name, "id_ed25519", sizeof(m.gen_name) - 1);
        else                 strncpy(m.gen_name, "id_rsa",     sizeof(m.gen_name) - 1);
        m.gen_comment[0]    = '\0';
        m.gen_passphrase[0] = '\0';
        m.pane = KeyMgrPane::LIST;
        scan_keys(m);
    } else {
        // Show ssh-keygen error (first line)
        size_t nl = out.find('\n');
        std::string err = (nl != std::string::npos) ? out.substr(0, nl) : out;
        if (err.empty()) err = "ssh-keygen failed (is it installed?)";
        snprintf(m.status, sizeof(m.status), "Error: %s", err.c_str());
        m.status_ok = false;
    }
}

// ============================================================================
// KEYBOARD
// ============================================================================

// Append text to a char buffer (bounded).
static void field_append(char *buf, size_t cap, const char *text)
{
    size_t len = strlen(buf);
    size_t tlen = strlen(text);
    if (len + tlen < cap - 1) {
        memcpy(buf + len, text, tlen);
        buf[len + tlen] = '\0';
    }
}

static void field_backspace(char *buf)
{
    size_t len = strlen(buf);
    if (len > 0) buf[len - 1] = '\0';
}

bool ssh_key_mgr_keydown(SDL_Keysym ks, const char *text_input)
{
    SshKeyMgr &m = g_ssh_key_mgr;
    if (!m.visible) return false;

    SDL_Keycode sym = ks.sym;

    // F8 or Escape always closes / backs up
    if (sym == SDLK_F8 || sym == SDLK_ESCAPE) {
        if (m.pane != KeyMgrPane::LIST) {
            m.pane = KeyMgrPane::LIST;
            m.status[0] = '\0';
        } else {
            ssh_key_mgr_close();
        }
        return true;
    }

    // ----------------------------------------------------------------
    if (m.pane == KeyMgrPane::LIST) {
        if (sym == SDLK_UP)   { if (m.selected > 0) m.selected--; return true; }
        if (sym == SDLK_DOWN) {
            if (m.selected < (int)m.keys.size() - 1) m.selected++;
            return true;
        }
        if (sym == SDLK_RETURN) {
            // Copy pub key on enter
            action_copy_pubkey(m);
            return true;
        }
        if (sym == SDLK_DELETE || sym == SDLK_BACKSPACE) {
            if (!m.keys.empty()) {
                m.pane = KeyMgrPane::CONFIRM_DELETE;
                m.status[0] = '\0';
            }
            return true;
        }
        if (sym == SDLK_n || sym == SDLK_INSERT) {
            m.pane = KeyMgrPane::GENERATE;
            m.status[0] = '\0';
            m.gen_focus = 0;
            // Pre-fill filename based on type
            if (m.gen_type == 0) strncpy(m.gen_name, "id_ed25519", sizeof(m.gen_name) - 1);
            else                 strncpy(m.gen_name, "id_rsa",     sizeof(m.gen_name) - 1);
            return true;
        }
        if (sym == SDLK_r) {
            scan_keys(m);
            snprintf(m.status, sizeof(m.status), "Refreshed.");
            m.status_ok = true;
            return true;
        }
    }

    // ----------------------------------------------------------------
    else if (m.pane == KeyMgrPane::GENERATE) {
        if (sym == SDLK_TAB) {
            m.gen_focus = (m.gen_focus + 1) % 3;
            return true;
        }
        if (sym == SDLK_RETURN) {
            action_generate(m);
            return true;
        }
        if (sym == SDLK_BACKSPACE) {
            switch (m.gen_focus) {
            case 0: field_backspace(m.gen_name);       break;
            case 1: field_backspace(m.gen_comment);    break;
            case 2: field_backspace(m.gen_passphrase); break;
            }
            return true;
        }
        // Ctrl+A / Ctrl+K clear field
        if ((ks.mod & KMOD_CTRL) && sym == SDLK_a) {
            switch (m.gen_focus) {
            case 0: m.gen_name[0]       = '\0'; break;
            case 1: m.gen_comment[0]    = '\0'; break;
            case 2: m.gen_passphrase[0] = '\0'; break;
            }
            return true;
        }
        // Toggle key type with left/right when no field is focused (focus==3)
        if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
            m.gen_type ^= 1;
            if (m.gen_type == 0) strncpy(m.gen_name, "id_ed25519", sizeof(m.gen_name) - 1);
            else                 strncpy(m.gen_name, "id_rsa",     sizeof(m.gen_name) - 1);
            return true;
        }
        if (text_input && text_input[0]) {
            switch (m.gen_focus) {
            case 0: field_append(m.gen_name,       sizeof(m.gen_name),       text_input); break;
            case 1: field_append(m.gen_comment,    sizeof(m.gen_comment),    text_input); break;
            case 2: field_append(m.gen_passphrase, sizeof(m.gen_passphrase), text_input); break;
            }
            return true;
        }
    }

    // ----------------------------------------------------------------
    else if (m.pane == KeyMgrPane::CONFIRM_DELETE) {
        if (sym == SDLK_RETURN || sym == SDLK_y) {
            action_delete_key(m);
            return true;
        }
        if (sym == SDLK_n) {
            m.pane = KeyMgrPane::LIST;
            m.status[0] = '\0';
            return true;
        }
    }

    return true; // swallow all keys while overlay is open
}

// ============================================================================
// MOUSE
// ============================================================================

// Recompute button layout helpers (mirrors render logic positions).
static float content_oy_of(float oy, float /*ph*/) { return oy + TITLE_H + 1; }
static float content_ph_of(float ph)               { return ph - TITLE_H - 1; }

bool ssh_key_mgr_mousedown(int mx, int my, int /*button*/)
{
    SshKeyMgr &m = g_ssh_key_mgr;
    if (!m.visible) return false;

    int win_w, win_h;
    SDL_GetWindowSize(SDL_GL_GetCurrentWindow(), &win_w, &win_h);
    float pw = (float)std::max(win_w - 120, MIN_W);
    float ph = (float)std::max(win_h - 120, MIN_H);
    float ox = (float)((win_w - (int)pw) / 2);
    float oy = (float)((win_h - (int)ph) / 2);
    float coy = content_oy_of(oy, ph);
    float cph = content_ph_of(ph);

    // Click outside → close (only on LIST pane)
    if ((mx < ox || mx > ox + pw || my < oy || my > oy + ph) &&
        m.pane == KeyMgrPane::LIST) {
        ssh_key_mgr_close();
        return true;
    }

    if (m.pane == KeyMgrPane::LIST) {
        // Row clicks
        float list_y = coy + PAD + MFONT + PAD / 2 + ROW_H; // after header
        for (int i = 0; i < m.visible_rows && m.scroll_top + i < (int)m.keys.size(); i++) {
            float ry = list_y + i * ROW_H;
            if (my >= ry && my < ry + ROW_H && mx >= ox && mx < ox + pw) {
                m.selected = m.scroll_top + i;
                return true;
            }
        }

        // Buttons
        float by = coy + cph - BTN_H - PAD;
        float bx = ox + PAD;
        bool has_key = !m.keys.empty();

        Btn b_gen  = { bx,                   by, BTN_W, BTN_H };
        Btn b_copy = { bx + BTN_W + 8,        by, BTN_W, BTN_H };
        Btn b_del  = { bx + (BTN_W + 8) * 2, by, BTN_W, BTN_H };
        Btn b_close= { ox + pw - BTN_W - PAD, by, BTN_W, BTN_H };

        if (btn_hit(b_gen, mx, my)) {
            m.pane = KeyMgrPane::GENERATE;
            m.status[0] = '\0';
            m.gen_focus = 0;
            return true;
        }
        if (has_key && btn_hit(b_copy, mx, my)) { action_copy_pubkey(m); return true; }
        if (has_key && btn_hit(b_del,  mx, my)) {
            m.pane = KeyMgrPane::CONFIRM_DELETE;
            m.status[0] = '\0';
            return true;
        }
        if (btn_hit(b_close, mx, my)) { ssh_key_mgr_close(); return true; }
    }
    else if (m.pane == KeyMgrPane::GENERATE) {
        float y = coy + PAD + MFONT + PAD;

        // Key type radio buttons (approximate positions)
        float ry = y + 2;
        float rx0 = ox + PAD + 90;
        float rx1 = rx0 + 200;
        if (my >= ry && my < ry + 16) {
            if (mx >= rx0 && mx < rx0 + 200) { m.gen_type = 0; strncpy(m.gen_name,"id_ed25519",sizeof(m.gen_name)-1); }
            else if (mx >= rx1 && mx < rx1 + 120) { m.gen_type = 1; strncpy(m.gen_name,"id_rsa",sizeof(m.gen_name)-1); }
            return true;
        }
        y += ROW_H + 4;

        // Field click-to-focus
        float fw = pw - PAD * 2 - 90;
        float fx = ox + PAD + 90;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 0; return true; }
        y += FIELD_H + PAD;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 1; return true; }
        y += FIELD_H + PAD;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 2; return true; }

        // Show/hide passphrase button
        float show_x = ox + pw - PAD - BTN_W / 2;
        Btn b_show = { show_x, y - FIELD_H - PAD, (float)(BTN_W/2), FIELD_H };
        // Recompute (y was incremented past the passphrase row)
        float pass_y = coy + PAD + MFONT + PAD + (ROW_H + 4) + (FIELD_H + PAD) * 2;
        b_show.y = pass_y;
        if (btn_hit(b_show, mx, my)) { m.gen_pass_show = !m.gen_pass_show; return true; }

        float by = coy + cph - BTN_H - PAD;
        float bx = ox + PAD;
        Btn b_gen = { bx, by, BTN_W, BTN_H };
        Btn b_back = { bx + BTN_W + 8, by, BTN_W, BTN_H };
        if (btn_hit(b_gen,  mx, my)) { action_generate(m); return true; }
        if (btn_hit(b_back, mx, my)) { m.pane = KeyMgrPane::LIST; m.status[0] = '\0'; return true; }
    }
    else if (m.pane == KeyMgrPane::CONFIRM_DELETE) {
        float by = coy + cph - BTN_H - PAD;
        float bx = ox + pw / 2 - BTN_W - 6;
        Btn b_yes = { bx,              by, BTN_W, BTN_H };
        Btn b_no  = { bx + BTN_W + 12, by, BTN_W, BTN_H };
        if (btn_hit(b_yes, mx, my)) { action_delete_key(m); return true; }
        if (btn_hit(b_no,  mx, my)) { m.pane = KeyMgrPane::LIST; m.status[0] = '\0'; return true; }
    }

    return true; // always consume while open
}

bool ssh_key_mgr_mousemotion(int /*x*/, int /*y*/, bool /*lbutton*/)
{
    // Could add hover highlighting — skipped for now, returns false (no redraw forced).
    return false;
}

void ssh_key_mgr_scroll(int delta_y)
{
    SshKeyMgr &m = g_ssh_key_mgr;
    if (!m.visible || m.pane != KeyMgrPane::LIST) return;
    m.scroll_top -= delta_y;
    int max_top = (int)m.keys.size() - m.visible_rows;
    if (m.scroll_top > max_top) m.scroll_top = max_top;
    if (m.scroll_top < 0)       m.scroll_top = 0;
}
