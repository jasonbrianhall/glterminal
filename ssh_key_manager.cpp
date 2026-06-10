// ssh_key_manager.cpp — F8 SSH Key Manager overlay
//
// Lists keys found in ~/.ssh, shows type/bits/fingerprint/comment/filename.
// Generate pane: Ed25519 / RSA / ECDSA via libssh2 — no ssh-keygen needed.
// Actions: copy public key to clipboard, delete key pair.
#include "ssh_key_manager.h"
#include "gl_terminal.h"
#include "gl_renderer.h"
#include "ft_font.h"

#include <SDL2/SDL.h>
#include <libssh2.h>

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

// Trim trailing whitespace in-place.
static void rtrim(std::string &s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
}

// Base64-decode a string. Returns decoded bytes.
static std::vector<uint8_t> b64_decode(const std::string &in)
{
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) | T[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

// Read big-endian uint32 from wire blob.
static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

// SHA-256 a buffer, return 32 bytes. Uses libssh2's bundled crypto.
// We call the system SHA-256 directly via libssh2_hostkey_hash approach
// but for .pub fingerprints we just need raw SHA-256 of the wire blob.
// libssh2 doesn't expose a raw SHA-256, so we use the C standard library
// approach: link against whatever crypto libssh2 itself uses (openssl/mbedtls/gcrypt).
// Since the project already links libssh2, we can call the underlying SHA via a
// simple inline using the POSIX <sys/sha2.h> or, most portably, include openssl/sha.h
// which is always present when libssh2 is built against OpenSSL.
#include <openssl/sha.h>
static std::string sha256_b64(const std::vector<uint8_t> &data)
{
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    // Base64 encode (no padding, as OpenSSH does it)
    static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i += 3) {
        uint32_t v = (uint32_t)hash[i] << 16;
        if (i+1 < SHA256_DIGEST_LENGTH) v |= (uint32_t)hash[i+1] << 8;
        if (i+2 < SHA256_DIGEST_LENGTH) v |= (uint32_t)hash[i+2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        if (i+1 < SHA256_DIGEST_LENGTH) out += B64[(v >> 6) & 63];
        if (i+2 < SHA256_DIGEST_LENGTH) out += B64[v & 63];
    }
    return "SHA256:" + out;
}

// Parse a .pub file line: "<keytype> <base64blob> [comment]"
// Fills type, bits, fingerprint, comment in e.
static void parse_pub_file(const std::string &path, SshKeyEntry &e)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[4096] = {};
    fgets(line, sizeof(line), f);
    fclose(f);

    // Split into tokens
    char *p = line;
    while (*p == ' ') p++;
    char *tok1_end = strchr(p, ' ');
    if (!tok1_end) return;
    std::string key_type_str(p, tok1_end - p);
    p = tok1_end + 1;
    while (*p == ' ') p++;
    char *tok2_end = strchr(p, ' ');
    std::string b64_blob = tok2_end ? std::string(p, tok2_end - p) : std::string(p);
    rtrim(b64_blob);
    if (tok2_end) {
        p = tok2_end + 1;
        while (*p == ' ') p++;
        e.comment = p;
        rtrim(e.comment);
    }

    // Type string
    if      (key_type_str == "ssh-ed25519")          { e.type = "ED25519"; e.bits = 256; }
    else if (key_type_str == "ssh-rsa")               { e.type = "RSA"; }
    else if (key_type_str == "ecdsa-sha2-nistp256")   { e.type = "ECDSA"; e.bits = 256; }
    else if (key_type_str == "ecdsa-sha2-nistp384")   { e.type = "ECDSA"; e.bits = 384; }
    else if (key_type_str == "ecdsa-sha2-nistp521")   { e.type = "ECDSA"; e.bits = 521; }
    else                                               { e.type = key_type_str; }

    // Decode wire blob
    std::vector<uint8_t> blob = b64_decode(b64_blob);
    if (blob.empty()) return;

    // For RSA, extract bits from modulus length in wire format:
    // string "ssh-rsa" | mpint e | mpint n
    if (e.type == "RSA" && blob.size() > 4) {
        const uint8_t *bp = blob.data();
        const uint8_t *end = bp + blob.size();
        // skip key-type string
        if (bp + 4 > end) goto done;
        { uint32_t len = read_u32(bp); bp += 4 + len; }
        // skip exponent
        if (bp + 4 > end) goto done;
        { uint32_t len = read_u32(bp); bp += 4 + len; }
        // modulus length in bits
        if (bp + 4 > end) goto done;
        { uint32_t len = read_u32(bp);
          e.bits = (int)(len * 8); // approximate; leading zero byte may be present
          // strip sign byte if present
          if (len > 0 && *(bp+4) == 0x00) e.bits = (int)((len - 1) * 8);
        }
    }
    done:
    // SHA-256 fingerprint of the raw wire blob
    e.fingerprint = sha256_b64(blob);
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
        e.filename  = pf.substr(0, pf.size() - 4);  // strip ".pub"

        parse_pub_file(pub, e);
        if (e.type.empty()) e.type = "UNKNOWN";

        m.keys.push_back(e);
    }
}

// ============================================================================
// SORTING
// ============================================================================

static void sort_keys(SshKeyMgr &m)
{
    if (m.sort_col == KeySortCol::NONE) return;
    std::sort(m.keys.begin(), m.keys.end(),
        [&](const SshKeyEntry &a, const SshKeyEntry &b) {
            const std::string *va, *vb;
            switch (m.sort_col) {
            case KeySortCol::FILENAME:    va = &a.filename;    vb = &b.filename;    break;
            case KeySortCol::FINGERPRINT: va = &a.fingerprint; vb = &b.fingerprint; break;
            case KeySortCol::COMMENT:     va = &a.comment;     vb = &b.comment;     break;
            default:                      va = &a.filename;    vb = &b.filename;    break;
            }
            return (m.sort_dir == KeySortDir::ASC) ? (*va < *vb) : (*va > *vb);
        });
}

static void toggle_sort(SshKeyMgr &m, KeySortCol col)
{
    if (m.sort_col == col)
        m.sort_dir = (m.sort_dir == KeySortDir::ASC) ? KeySortDir::DESC : KeySortDir::ASC;
    else {
        m.sort_col = col;
        m.sort_dir = KeySortDir::ASC;
    }
    sort_keys(m);
}
// ============================================================================

void ssh_key_mgr_open(int /*win_w*/, int /*win_h*/)
{
    g_ssh_key_mgr.visible    = true;
    g_ssh_key_mgr.pane       = KeyMgrPane::LIST;
    g_ssh_key_mgr.status[0]  = '\0';
    scan_keys(g_ssh_key_mgr);
    sort_keys(g_ssh_key_mgr);
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
    // Filename (sortable) | Type | Bits | Comment (sortable) | Fingerprint (sortable)
    float col_filename = ox + PAD;
    float col_type     = col_filename + 150;
    float col_bits     = col_type + 70;
    float col_comment  = col_bits + 44;
    float col_fp       = col_comment + 150;

    draw_rect(ox + 1, y, pw - 2, ROW_H, 0.14f, 0.18f, 0.28f, 1.f);
    dt("Type",     col_type,  y + ROW_H * 0.68f, 0.55f, 0.65f, 0.85f, 1.f);
    dt("Bits",     col_bits,  y + ROW_H * 0.68f, 0.55f, 0.65f, 0.85f, 1.f);

    auto draw_sortable_hdr = [&](const char *label, float cx, float cw, KeySortCol col) {
        bool active = (m.sort_col == col);
        float hr = active ? 0.70f : 0.55f, hg = active ? 0.85f : 0.65f, hb = active ? 1.0f : 0.85f;
        if (active) draw_rect(cx, y, cw, ROW_H, 0.20f, 0.28f, 0.45f, 0.60f);
        dt(label, cx, y + ROW_H * 0.68f, hr, hg, hb, 1.f, active ? ATTR_BOLD : 0);
        if (active) {
            const char *arrow = (m.sort_dir == KeySortDir::ASC) ? " ▲" : " ▼";
            dt(arrow, cx + (float)(strlen(label) * MFONT) * 0.56f, y + ROW_H * 0.68f, hr, hg, hb, 1.f);
        }
    };

    draw_sortable_hdr("Filename",    col_filename, col_type    - col_filename, KeySortCol::FILENAME);
    draw_sortable_hdr("Comment",     col_comment,  col_fp      - col_comment,  KeySortCol::COMMENT);
    draw_sortable_hdr("Fingerprint", col_fp,       ox + pw - PAD - col_fp,     KeySortCol::FINGERPRINT);
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

        auto truncate_col = [&](const std::string &s, float max_w) -> std::string {
            if ((float)(s.size() * MFONT) * 0.56f <= max_w) return s;
            int max_chars = (int)(max_w / (MFONT * 0.56f));
            if (max_chars < 4) max_chars = 4;
            return s.substr(0, (size_t)max_chars - 3) + "...";
        };

        // Filename (first column)
        dt(truncate_col(e.filename, col_type - col_filename - 4).c_str(),
           col_filename, y + ROW_H * 0.68f, fr, fg * 0.90f, fb * 0.80f, 1.f);

        // Type / Bits
        dt(e.type.c_str(), col_type, y + ROW_H * 0.68f, fr, fg, fb, 1.f);

        char bits[16] = "-";
        if (e.bits > 0) snprintf(bits, sizeof(bits), "%d", e.bits);
        dt(bits, col_bits, y + ROW_H * 0.68f, fr * 0.85f, fg * 0.85f, fb, 1.f);

        // Comment
        dt(truncate_col(e.comment, col_fp - col_comment - 4).c_str(),
           col_comment, y + ROW_H * 0.68f, fr * 0.70f, fg * 0.80f, fb * 0.90f, 1.f);

        // Fingerprint
        dt(truncate_col(e.fingerprint, ox + pw - PAD - col_fp - 4).c_str(),
           col_fp, y + ROW_H * 0.68f, fr * 0.75f, fg * 0.85f, fb, 1.f);

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

static constexpr int RSA_SIZE_COUNT = 3;
static constexpr int rsa_sizes[RSA_SIZE_COUNT] = { 2048, 3072, 4096 };
static constexpr const char *rsa_size_labels[RSA_SIZE_COUNT] = { "2048", "3072", "4096" };

struct KeyTypeDef { int id; const char *label; const char *default_name; };
static constexpr KeyTypeDef key_types[] = {
    { 0, "Ed25519 (recommended)", "id_ed25519" },
    { 1, "ECDSA-256",             "id_ecdsa"   },
    { 2, "ECDSA-384",             "id_ecdsa384"},
    { 3, "ECDSA-521",             "id_ecdsa521"},
    { 4, "RSA",                   "id_rsa"     },
};
static constexpr int KEY_TYPE_COUNT = 5;

static void render_generate_pane(SshKeyMgr &m, float ox, float oy,
                                 float pw, float ph, int /*win_w*/, int /*win_h*/)
{
    float y = oy + PAD;
    dt("Generate New SSH Key", ox + PAD, y + MFONT * 0.85f, 0.55f, 0.75f, 1.0f, 1.f, ATTR_BOLD);
    y += MFONT + PAD;

    // Key type radios — two rows of ~3
    dt("Key type:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    float rx = ox + PAD + 90;
    float row1_y = y;
    for (int t = 0; t < KEY_TYPE_COUNT; t++) {
        if (t == 3) { rx = ox + PAD + 90; y += ROW_H; } // wrap to second row
        bool sel = (m.gen_type == t);
        draw_rect(rx, y + 2, 12, 12, 0.15f, 0.20f, 0.30f, 1.f);
        if (sel) draw_rect(rx + 3, y + 5, 6, 6, 0.40f, 0.75f, 1.0f, 1.f);
        float lw = (float)(strlen(key_types[t].label) * MFONT) * 0.56f + 24;
        dt(key_types[t].label, rx + 16, y + MFONT * 0.85f,
           sel ? 0.92f : 0.68f, sel ? 0.95f : 0.72f, 1.0f, 1.f);
        rx += lw + 8;
    }
    (void)row1_y;
    y += ROW_H + 8;

    // RSA size buttons (only when RSA selected)
    if (m.gen_type == 4) {
        dt("Key size:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
        float bx2 = ox + PAD + 90;
        for (int i = 0; i < RSA_SIZE_COUNT; i++) {
            bool sel = (i == m.gen_rsa_size);
            draw_btn(rsa_size_labels[i], bx2, y, 90, BTN_H, false, sel);
            bx2 += 98;
        }
        y += BTN_H + PAD;
    }

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
// RENDER — CONFIRM OVERWRITE PANE
// ============================================================================

static void render_overwrite_pane(SshKeyMgr &m, float ox, float oy,
                                  float pw, float ph, int /*win_w*/, int /*win_h*/)
{
    float cy = oy + ph * 0.35f;
    dt("File already exists — overwrite?", ox + pw / 2 - 140, cy, 0.90f, 0.70f, 0.20f, 1.f, ATTR_BOLD);
    cy += MFONT + PAD;

    char msg[320];
    snprintf(msg, sizeof(msg), "~/.ssh/%s  +  ~/.ssh/%s.pub", m.gen_name, m.gen_name);
    float tw = (float)(strlen(msg) * MFONT) * 0.56f;
    dt(msg, ox + (pw - tw) / 2, cy, 0.85f, 0.82f, 0.75f, 1.f);
    cy += MFONT + PAD / 2;

    float hint_w = (float)(strlen("Existing keys will be permanently replaced.") * MFONT) * 0.56f;
    dt("Existing keys will be permanently replaced.",
       ox + (pw - hint_w) / 2, cy, 0.65f, 0.55f, 0.40f, 1.f);

    float bx = ox + pw / 2 - BTN_W - 6;
    float by = oy + ph - BTN_H - PAD;
    draw_btn("Overwrite", bx,              by, BTN_W, BTN_H, false);
    draw_btn("Cancel",    bx + BTN_W + 12, by, BTN_W, BTN_H, false, true);
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
    case KeyMgrPane::CONFIRM_OVERWRITE:
        render_overwrite_pane(m, ox, content_oy, pw, content_ph, win_w, win_h);
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
    sort_keys(m);
    if (m.selected >= (int)m.keys.size())
        m.selected = (int)m.keys.size() - 1;
    if (m.selected < 0) m.selected = 0;
}

// ============================================================================
// KEY GENERATION  (via OpenSSL, which is already linked through libssh2)
// ============================================================================
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>

static bool write_openssh_pubkey(EVP_PKEY *pkey, const char *comment, const std::string &path)
{
    int key_type = EVP_PKEY_base_id(pkey);
    std::string type_str;
    std::vector<uint8_t> blob;

    auto push_u32 = [&](uint32_t v) {
        blob.push_back((v>>24)&0xFF); blob.push_back((v>>16)&0xFF);
        blob.push_back((v>> 8)&0xFF); blob.push_back( v     &0xFF);
    };
    auto push_bytes = [&](const uint8_t *d, size_t n) {
        push_u32((uint32_t)n);
        blob.insert(blob.end(), d, d + n);
    };
    auto push_str_raw = [&](const char *s) { push_bytes((const uint8_t*)s, strlen(s)); };
    auto push_bn = [&](const BIGNUM *bn) {
        int n = BN_num_bytes(bn);
        std::vector<uint8_t> tmp(n);
        BN_bn2bin(bn, tmp.data());
        if (n > 0 && (tmp[0] & 0x80)) {
            push_u32((uint32_t)(n+1)); blob.push_back(0x00);
        } else { push_u32((uint32_t)n); }
        blob.insert(blob.end(), tmp.begin(), tmp.end());
    };

    if (key_type == EVP_PKEY_ED25519) {
        type_str = "ssh-ed25519";
        push_str_raw("ssh-ed25519");
        uint8_t pub[32] = {}; size_t pub_len = 32;
        EVP_PKEY_get_raw_public_key(pkey, pub, &pub_len);
        push_bytes(pub, 32);
    } else if (key_type == EVP_PKEY_RSA) {
        type_str = "ssh-rsa";
        push_str_raw("ssh-rsa");
        const RSA *rsa = EVP_PKEY_get0_RSA(pkey);
        const BIGNUM *n2 = nullptr, *e2 = nullptr;
        RSA_get0_key(rsa, &n2, &e2, nullptr);
        push_bn(e2); push_bn(n2);
    } else if (key_type == EVP_PKEY_EC) {
        const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
        int deg = EC_GROUP_get_degree(EC_KEY_get0_group(ec));
        if      (deg <= 256) type_str = "ecdsa-sha2-nistp256";
        else if (deg <= 384) type_str = "ecdsa-sha2-nistp384";
        else                 type_str = "ecdsa-sha2-nistp521";
        const char *curve = type_str.c_str() + 10;
        push_str_raw(type_str.c_str()); push_str_raw(curve);
        const EC_POINT *pt = EC_KEY_get0_public_key(ec);
        size_t pt_len = EC_POINT_point2oct(EC_KEY_get0_group(ec), pt, POINT_CONVERSION_UNCOMPRESSED, nullptr, 0, nullptr);
        std::vector<uint8_t> pt_buf(pt_len);
        EC_POINT_point2oct(EC_KEY_get0_group(ec), pt, POINT_CONVERSION_UNCOMPRESSED, pt_buf.data(), pt_len, nullptr);
        push_bytes(pt_buf.data(), pt_len);
    } else { return false; }

    static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (size_t i = 0; i < blob.size(); i += 3) {
        uint32_t v = (uint32_t)blob[i] << 16;
        if (i+1 < blob.size()) v |= (uint32_t)blob[i+1] << 8;
        if (i+2 < blob.size()) v |= (uint32_t)blob[i+2];
        b64 += B64[(v>>18)&63]; b64 += B64[(v>>12)&63];
        b64 += (i+1 < blob.size()) ? B64[(v>>6)&63] : '=';
        b64 += (i+2 < blob.size()) ? B64[v&63]      : '=';
    }

    FILE *f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "%s %s %s\n", type_str.c_str(), b64.c_str(), comment);
    fclose(f);
    return true;
}

static void action_generate_do(SshKeyMgr &m)
{
    std::string sshdir  = ssh_home_dir();
    std::string outpath = sshdir + "/" + m.gen_name;
    std::string pubpath = outpath + ".pub";
    std::string comment = m.gen_comment[0] ? m.gen_comment : m.gen_name;

#ifndef _WIN32
    mkdir(sshdir.c_str(), 0700);
#else
    CreateDirectoryA(sshdir.c_str(), nullptr);
#endif

    EVP_PKEY *pkey = nullptr;
    EVP_PKEY_CTX *ctx = nullptr;
    bool gen_ok = false;

    switch (m.gen_type) {
    case 0: // Ed25519
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        gen_ok = ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0;
        break;
    case 1: case 2: case 3: { // ECDSA
        int nid = (m.gen_type == 1) ? NID_X9_62_prime256v1 :
                  (m.gen_type == 2) ? NID_secp384r1 : NID_secp521r1;
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        gen_ok = ctx && EVP_PKEY_keygen_init(ctx) > 0 &&
                 EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid) > 0 &&
                 EVP_PKEY_keygen(ctx, &pkey) > 0;
        break; }
    case 4: { // RSA
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        gen_ok = ctx && EVP_PKEY_keygen_init(ctx) > 0 &&
                 EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, rsa_sizes[m.gen_rsa_size]) > 0 &&
                 EVP_PKEY_keygen(ctx, &pkey) > 0;
        break; }
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);

    if (!gen_ok || !pkey) {
        snprintf(m.status, sizeof(m.status), "Error: key generation failed.");
        m.status_ok = false;
        if (pkey) EVP_PKEY_free(pkey);
        return;
    }

    // Write private key — encrypt with AES-256-CBC if a passphrase was given
    const EVP_CIPHER *cipher = nullptr;
    const char *pp = m.gen_passphrase;
    if (pp[0] != '\0')
        cipher = EVP_aes_256_cbc();

    FILE *fpriv = fopen(outpath.c_str(), "wb");
    bool ok = fpriv && PEM_write_PrivateKey(fpriv, pkey, cipher,
                           pp[0] ? (const unsigned char *)pp : nullptr,
                           pp[0] ? (int)strlen(pp) : 0,
                           nullptr, nullptr);
    if (fpriv) {
        fclose(fpriv);
#ifndef _WIN32
        chmod(outpath.c_str(), 0600);
#endif
    }
    ok = ok && write_openssh_pubkey(pkey, comment.c_str(), pubpath);
    EVP_PKEY_free(pkey);

    if (ok) {
        snprintf(m.status, sizeof(m.status), "Generated: ~/.ssh/%s", m.gen_name);
        m.status_ok = true;
        strncpy(m.gen_name, key_types[m.gen_type].default_name, sizeof(m.gen_name) - 1);
        m.gen_comment[0] = '\0'; m.gen_passphrase[0] = '\0';
        m.pane = KeyMgrPane::LIST;
        scan_keys(m); sort_keys(m);
    } else {
        snprintf(m.status, sizeof(m.status), "Error: could not write key files.");
        m.status_ok = false;
    }
}

static void action_generate(SshKeyMgr &m)
{
    if (m.gen_name[0] == '\0') {
        snprintf(m.status, sizeof(m.status), "Error: filename is empty.");
        m.status_ok = false;
        return;
    }
    // If file already exists, route to overwrite confirmation pane
    std::string outpath = ssh_home_dir() + "/" + m.gen_name;
    FILE *chk = fopen(outpath.c_str(), "r");
    if (chk) {
        fclose(chk);
        m.pane = KeyMgrPane::CONFIRM_OVERWRITE;
        m.status[0] = '\0';
        return;
    }
    action_generate_do(m);
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
        if (m.pane == KeyMgrPane::CONFIRM_OVERWRITE) {
            m.pane = KeyMgrPane::GENERATE;
            m.status[0] = '\0';
        } else if (m.pane != KeyMgrPane::LIST) {
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
            strncpy(m.gen_name, key_types[m.gen_type].default_name, sizeof(m.gen_name) - 1);
            return true;
        }
        if (sym == SDLK_r) {
            scan_keys(m);
            sort_keys(m);
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
            if (sym == SDLK_RIGHT) m.gen_type = (m.gen_type + 1) % KEY_TYPE_COUNT;
            else                   m.gen_type = (m.gen_type + KEY_TYPE_COUNT - 1) % KEY_TYPE_COUNT;
            strncpy(m.gen_name, key_types[m.gen_type].default_name, sizeof(m.gen_name) - 1);
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
    else if (m.pane == KeyMgrPane::CONFIRM_OVERWRITE) {
        if (sym == SDLK_RETURN || sym == SDLK_y) {
            action_generate_do(m);
            return true;
        }
        if (sym == SDLK_n || sym == SDLK_ESCAPE) {
            m.pane = KeyMgrPane::GENERATE;
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
        // Column header clicks (sort) — must match render_list_pane column layout
        float hdr_y        = coy + PAD + MFONT + PAD / 2;
        float col_filename = ox + PAD;                     // first column
        float col_type     = col_filename + 150;
        float col_bits     = col_type + 70;
        float col_comment  = col_bits + 44;
        float col_fp       = col_comment + 150;
        if (my >= hdr_y && my < hdr_y + ROW_H) {
            if      (mx >= col_filename && mx < col_comment) toggle_sort(m, KeySortCol::FILENAME);
            else if (mx >= col_comment  && mx < col_fp)      toggle_sort(m, KeySortCol::COMMENT);
            else if (mx >= col_fp       && mx < ox + pw - PAD) toggle_sort(m, KeySortCol::FINGERPRINT);
            return true;
        }

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

        // Key type radio buttons — mirrors render layout (two rows, wraps at t==3)
        float rx = ox + PAD + 90;
        float row_y = y;
        for (int t = 0; t < KEY_TYPE_COUNT; t++) {
            if (t == 3) { rx = ox + PAD + 90; row_y += ROW_H; }
            float lw = (float)(strlen(key_types[t].label) * MFONT) * 0.56f + 24 + 8;
            if (my >= row_y && my < row_y + ROW_H && mx >= rx && mx < rx + lw) {
                m.gen_type = t;
                m.gen_dropdown_open = false;
                strncpy(m.gen_name, key_types[t].default_name, sizeof(m.gen_name) - 1);
                return true;
            }
            rx += lw;
        }
        y += ROW_H * 2 + 8;  // two radio rows + gap

        // RSA size buttons
        if (m.gen_type == 4) {
            float bx2 = ox + PAD + 90;
            for (int i = 0; i < RSA_SIZE_COUNT; i++) {
                Btn b = { bx2, y, 90, BTN_H };
                if (btn_hit(b, mx, my)) { m.gen_rsa_size = i; return true; }
                bx2 += 98;
            }
            y += BTN_H + PAD;
        }

        // Field click-to-focus (filename, comment, passphrase)
        float fw = pw - PAD * 2 - 90;
        float fx = ox + PAD + 90;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 0; return true; }
        y += FIELD_H + PAD;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 1; return true; }
        y += FIELD_H + PAD;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 2; return true; }

        // Show/hide passphrase button
        Btn b_show = { ox + pw - PAD - (float)(BTN_W/2), y, (float)(BTN_W/2), FIELD_H };
        if (btn_hit(b_show, mx, my)) { m.gen_pass_show = !m.gen_pass_show; return true; }

        float by = coy + cph - BTN_H - PAD;
        float bx = ox + PAD;
        Btn b_gen  = { bx,              by, BTN_W, BTN_H };
        Btn b_back = { bx + BTN_W + 8,  by, BTN_W, BTN_H };
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
    else if (m.pane == KeyMgrPane::CONFIRM_OVERWRITE) {
        float by = coy + cph - BTN_H - PAD;
        float bx = ox + pw / 2 - BTN_W - 6;
        Btn b_yes = { bx,              by, BTN_W, BTN_H };
        Btn b_no  = { bx + BTN_W + 12, by, BTN_W, BTN_H };
        if (btn_hit(b_yes, mx, my)) { action_generate_do(m); return true; }
        if (btn_hit(b_no,  mx, my)) { m.pane = KeyMgrPane::GENERATE; m.status[0] = '\0'; return true; }
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
