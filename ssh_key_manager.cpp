// ssh_key_manager.cpp — F8 SSH Key Manager overlay
//
// Lists keys found in ~/.ssh, shows type/bits/fingerprint/comment/filename.
// Generate pane: Ed25519 / RSA / ECDSA via OpenSSL — no ssh-keygen needed.
// Actions: copy public key to clipboard, delete key pair, add to authorized_keys.
//
// Remote panel (shown when USESSH + ssh_active()):
//   • Lists remote ~/.ssh/authorized_keys entries via SFTP
//   • Add (push selected local .pub) / Remove entries
//   • Copy Private Key — uploads local private key to remote ~/.ssh/
#include "ssh_key_manager.h"
#include "gl_terminal.h"
#include "gl_renderer.h"
#include "ft_font.h"

#include <SDL2/SDL.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

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
#  include <aclapi.h>
#endif

// ============================================================================
// EXTERNAL REFS
// ============================================================================

extern int  g_font_size;

static constexpr int MFONT = 14;

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
static constexpr int DIVIDER_H = 18;   // height of the draggable divider bar
static constexpr int REMOTE_MIN_H = 120; // minimum remote panel height
static constexpr int REMOTE_DEF_H = 280; // default remote panel height

// ============================================================================
// STATE
// ============================================================================

SshKeyMgr g_ssh_key_mgr;

// ============================================================================
// SSH session access — weak link; only used when USESSH is compiled in
// ============================================================================

#ifdef USESSH
#  include "ssh_session.h"
static bool  remote_available() { return ssh_active(); }
static LIBSSH2_SESSION *remote_session() { return ssh_get_session(); }
static void  remote_lock()   { ssh_session_lock(); }
static void  remote_unlock() { ssh_session_unlock(); }
#else
static bool  remote_available() { return false; }
static LIBSSH2_SESSION *remote_session() { return nullptr; }
static void  remote_lock()   {}
static void  remote_unlock() {}
#endif

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

static void rtrim(std::string &s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
}

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

static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

#include <openssl/sha.h>
static std::string sha256_b64(const std::vector<uint8_t> &data)
{
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
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

// Parse a pub-key line and fill type/bits/fingerprint/comment.
static void parse_pub_line(const std::string &line, RemoteAuthEntry &e)
{
    // format: <keytype> <base64blob> [comment]
    const char *p = line.c_str();
    while (*p == ' ') p++;
    if (*p == '#' || *p == '\0') return;

    const char *t1 = strchr(p, ' ');
    if (!t1) return;
    std::string key_type_str(p, t1 - p);
    p = t1 + 1;
    while (*p == ' ') p++;
    const char *t2 = strchr(p, ' ');
    std::string b64_blob = t2 ? std::string(p, t2 - p) : std::string(p);
    rtrim(b64_blob);
    if (t2) {
        p = t2 + 1;
        while (*p == ' ') p++;
        e.comment = p;
        rtrim(e.comment);
    }

    if      (key_type_str == "ssh-ed25519")          e.type = "ED25519";
    else if (key_type_str == "ssh-rsa")               e.type = "RSA";
    else if (key_type_str == "ecdsa-sha2-nistp256")   e.type = "ECDSA-256";
    else if (key_type_str == "ecdsa-sha2-nistp384")   e.type = "ECDSA-384";
    else if (key_type_str == "ecdsa-sha2-nistp521")   e.type = "ECDSA-521";
    else                                               e.type = key_type_str;

    std::vector<uint8_t> blob = b64_decode(b64_blob);
    if (!blob.empty())
        e.fingerprint = sha256_b64(blob);
}

static void parse_pub_file(const std::string &path, SshKeyEntry &e);  // forward decl

// Parse a pub-key line already in memory into a SshKeyEntry.
static void parse_pub_file_from_line(const char *line, SshKeyEntry &e)
{
    char *p = const_cast<char *>(line);
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
    if      (key_type_str == "ssh-ed25519")          { e.type = "ED25519"; e.bits = 256; }
    else if (key_type_str == "ssh-rsa")               { e.type = "RSA"; }
    else if (key_type_str == "ecdsa-sha2-nistp256")   { e.type = "ECDSA"; e.bits = 256; }
    else if (key_type_str == "ecdsa-sha2-nistp384")   { e.type = "ECDSA"; e.bits = 384; }
    else if (key_type_str == "ecdsa-sha2-nistp521")   { e.type = "ECDSA"; e.bits = 521; }
    else                                               { e.type = key_type_str; }
    std::vector<uint8_t> blob = b64_decode(b64_blob);
    if (!blob.empty()) e.fingerprint = sha256_b64(blob);
}

static void parse_pub_file(const std::string &path, SshKeyEntry &e)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[4096] = {};
    fgets(line, sizeof(line), f);
    fclose(f);

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

    if      (key_type_str == "ssh-ed25519")          { e.type = "ED25519"; e.bits = 256; }
    else if (key_type_str == "ssh-rsa")               { e.type = "RSA"; }
    else if (key_type_str == "ecdsa-sha2-nistp256")   { e.type = "ECDSA"; e.bits = 256; }
    else if (key_type_str == "ecdsa-sha2-nistp384")   { e.type = "ECDSA"; e.bits = 384; }
    else if (key_type_str == "ecdsa-sha2-nistp521")   { e.type = "ECDSA"; e.bits = 521; }
    else                                               { e.type = key_type_str; }

    std::vector<uint8_t> blob = b64_decode(b64_blob);
    if (blob.empty()) return;

    if (e.type == "RSA" && blob.size() > 4) {
        const uint8_t *bp = blob.data();
        const uint8_t *end = bp + blob.size();
        if (bp + 4 > end) goto done;
        { uint32_t len = read_u32(bp); bp += 4 + len; }
        if (bp + 4 > end) goto done;
        { uint32_t len = read_u32(bp); bp += 4 + len; }
        if (bp + 4 > end) goto done;
        { uint32_t len = read_u32(bp);
          e.bits = (int)(len * 8);
          if (len > 0 && *(bp+4) == 0x00) e.bits = (int)((len - 1) * 8);
        }
    }
    done:
    e.fingerprint = sha256_b64(blob);
}

// ============================================================================
// WIN32 CHMOD HELPER  (reused for both local files and text needed by callers)
// ============================================================================

#ifdef _WIN32
static void win32_lock_file(const std::string &path)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
    DWORD sz = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &sz);
    std::vector<uint8_t> tok_buf(sz);
    if (GetTokenInformation(token, TokenUser, tok_buf.data(), sz, &sz)) {
        PSID owner_sid = reinterpret_cast<TOKEN_USER *>(tok_buf.data())->User.Sid;
        EXPLICIT_ACCESS_A ea = {};
        ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
        ea.grfAccessMode        = SET_ACCESS;
        ea.grfInheritance       = NO_INHERITANCE;
        ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName    = reinterpret_cast<LPCH>(owner_sid);
        PACL acl = nullptr;
        if (SetEntriesInAclA(1, &ea, nullptr, &acl) == ERROR_SUCCESS) {
            SetNamedSecurityInfoA(
                const_cast<char *>(path.c_str()),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, acl, nullptr);
            LocalFree(acl);
        }
    }
    CloseHandle(token);
}
#endif

static void secure_file(const std::string &path)
{
#ifndef _WIN32
    chmod(path.c_str(), 0600);
#else
    win32_lock_file(path);
#endif
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
#ifndef _WIN32
        std::string pub  = sshdir + "/" + pf;
#else
        std::string pub  = sshdir + "\\" + pf;
#endif
        std::string priv = pub.substr(0, pub.size() - 4);

#ifndef _WIN32
        struct stat st;
        if (stat(priv.c_str(), &st) != 0) continue;
#else
        if (GetFileAttributesA(priv.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
#endif

        SshKeyEntry e;
        e.pub_path  = pub;
        e.priv_path = priv;
        e.filename  = pf.substr(0, pf.size() - 4);

        parse_pub_file(pub, e);
        if (e.type.empty()) e.type = "UNKNOWN";

        m.keys.push_back(e);
    }
}

// ============================================================================
// REMOTE SFTP HELPERS
// ============================================================================

// Read a remote file entirely into a string via SFTP.
// Returns true on success. Session must be locked by caller.
static bool sftp_read_file(LIBSSH2_SESSION *sess,
                           const std::string &remote_path,
                           std::string &out)
{
    LIBSSH2_SFTP *sftp = nullptr;
    do {
        sftp = libssh2_sftp_init(sess);
        if (!sftp && libssh2_session_last_errno(sess) != LIBSSH2_ERROR_EAGAIN)
            return false;
        SDL_Delay(2);
    } while (!sftp);

    LIBSSH2_SFTP_HANDLE *fh = nullptr;
    do {
        fh = libssh2_sftp_open(sftp, remote_path.c_str(), LIBSSH2_FXF_READ, 0);
        if (!fh) {
            unsigned long err = libssh2_sftp_last_error(sftp);
            if (err != LIBSSH2_FX_OK && err != (unsigned long)LIBSSH2_ERROR_EAGAIN) {
                libssh2_sftp_shutdown(sftp);
                return false;
            }
        }
        SDL_Delay(2);
    } while (!fh);

    out.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = libssh2_sftp_read(fh, buf, sizeof(buf));
        if (n > 0)  { out.append(buf, (size_t)n); }
        else if (n == LIBSSH2_ERROR_EAGAIN) { SDL_Delay(2); }
        else break;
    }

    libssh2_sftp_close(fh);
    libssh2_sftp_shutdown(sftp);
    return true;
}

// Write a string to a remote file via SFTP (create/truncate), chmod 0600.
static bool sftp_write_file(LIBSSH2_SESSION *sess,
                            const std::string &remote_path,
                            const std::string &content)
{
    LIBSSH2_SFTP *sftp = nullptr;
    do {
        sftp = libssh2_sftp_init(sess);
        if (!sftp && libssh2_session_last_errno(sess) != LIBSSH2_ERROR_EAGAIN)
            return false;
        SDL_Delay(2);
    } while (!sftp);

    unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC;
    LIBSSH2_SFTP_HANDLE *fh = nullptr;
    do {
        fh = libssh2_sftp_open(sftp, remote_path.c_str(), flags, 0600);
        if (!fh) {
            unsigned long err = libssh2_sftp_last_error(sftp);
            if (err != LIBSSH2_FX_OK && err != (unsigned long)LIBSSH2_ERROR_EAGAIN) {
                libssh2_sftp_shutdown(sftp);
                return false;
            }
        }
        SDL_Delay(2);
    } while (!fh);

    const char *p   = content.c_str();
    size_t      rem = content.size();
    bool        ok  = true;
    while (rem > 0) {
        ssize_t n = libssh2_sftp_write(fh, p, rem);
        if (n > 0)  { p += n; rem -= (size_t)n; }
        else if (n == LIBSSH2_ERROR_EAGAIN) { SDL_Delay(2); }
        else { ok = false; break; }
    }

    libssh2_sftp_close(fh);
    // Ensure correct permissions regardless of server umask
    LIBSSH2_SFTP_ATTRIBUTES attrs = {};
    attrs.flags       = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    attrs.permissions = 0100600;  // regular file, owner rw only
    libssh2_sftp_setstat(sftp, remote_path.c_str(), &attrs);
    libssh2_sftp_shutdown(sftp);
    return ok;
}

// Ensure remote ~/.ssh exists with mode 0700.
static void sftp_mkdir_ssh(LIBSSH2_SESSION *sess, const std::string &remote_ssh_dir)
{
    LIBSSH2_SFTP *sftp = nullptr;
    do {
        sftp = libssh2_sftp_init(sess);
        if (!sftp && libssh2_session_last_errno(sess) != LIBSSH2_ERROR_EAGAIN) return;
        SDL_Delay(2);
    } while (!sftp);
    libssh2_sftp_mkdir(sftp, remote_ssh_dir.c_str(), 0700);  // ok if exists
    libssh2_sftp_shutdown(sftp);
}

// Scan remote ~/.ssh/ for key pairs (*.pub that have a matching private key).
// Session must NOT be locked by caller; this function locks/unlocks itself.
static void sftp_scan_remote_keys(SshKeyMgr &m)
{
    if (!remote_available()) return;
    remote_lock();
    LIBSSH2_SESSION *sess = remote_session();

    LIBSSH2_SFTP *sftp = nullptr;
    do {
        sftp = libssh2_sftp_init(sess);
        if (!sftp && libssh2_session_last_errno(sess) != LIBSSH2_ERROR_EAGAIN) {
            remote_unlock();
            return;
        }
        SDL_Delay(2);
    } while (!sftp);

    LIBSSH2_SFTP_HANDLE *dh = nullptr;
    do {
        dh = libssh2_sftp_opendir(sftp, ".ssh");
        if (!dh) {
            unsigned long err = libssh2_sftp_last_error(sftp);
            if (err != LIBSSH2_FX_OK && err != (unsigned long)LIBSSH2_ERROR_EAGAIN) {
                libssh2_sftp_shutdown(sftp);
                remote_unlock();
                m.remote_keys.clear();
                m.remote_keys_loaded = true;
                return;
            }
        }
        SDL_Delay(2);
    } while (!dh);

    std::vector<std::string> pub_names;
    char fname[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (libssh2_sftp_readdir(dh, fname, sizeof(fname), &attrs) > 0) {
        std::string name(fname);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".pub")
            pub_names.push_back(name);
    }
    libssh2_sftp_closedir(dh);

    m.remote_keys.clear();
    for (auto &pf : pub_names) {
        // Check that private key also exists
        std::string priv_name = pf.substr(0, pf.size() - 4);
        std::string priv_path = ".ssh/" + priv_name;
        LIBSSH2_SFTP_ATTRIBUTES pa;
        if (libssh2_sftp_stat(sftp, priv_path.c_str(), &pa) != 0) continue;

        // Read .pub content to parse type/comment/fingerprint
        std::string pub_path = ".ssh/" + pf;
        LIBSSH2_SFTP_HANDLE *fh = libssh2_sftp_open(sftp, pub_path.c_str(), LIBSSH2_FXF_READ, 0);
        if (!fh) continue;
        char line[4096] = {};
        libssh2_sftp_read(fh, line, sizeof(line) - 1);
        libssh2_sftp_close(fh);

        SshKeyEntry e;
        e.filename  = priv_name;
        e.pub_path  = pub_path;   // remote relative path, for display only
        e.priv_path = priv_path;
        parse_pub_file_from_line(line, e);
        if (e.type.empty()) e.type = "UNKNOWN";
        m.remote_keys.push_back(e);
    }

    std::sort(m.remote_keys.begin(), m.remote_keys.end(),
              [](const SshKeyEntry &a, const SshKeyEntry &b){ return a.filename < b.filename; });

    libssh2_sftp_shutdown(sftp);
    remote_unlock();

    m.remote_keys_loaded = true;
    if (m.remote_keys_selected >= (int)m.remote_keys.size())
        m.remote_keys_selected = (int)m.remote_keys.size() - 1;
    if (m.remote_keys_selected < 0) m.remote_keys_selected = 0;
}

// Parse authorized_keys content into RemoteAuthEntry list.
static void parse_authorized_keys(const std::string &content,
                                  std::vector<RemoteAuthEntry> &out)
{
    out.clear();
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, nl == std::string::npos ? nl : nl - pos);
        rtrim(line);
        pos = (nl == std::string::npos) ? content.size() : nl + 1;
        if (line.empty() || line[0] == '#') continue;
        // Remove any \r that survived (CRLF files or Windows-originated keys)
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        if (line.empty()) continue;
        RemoteAuthEntry e;
        e.line = line;
        parse_pub_line(line, e);
        out.push_back(e);
    }
}

// Reload remote authorized_keys into m.remote_auth.
static void remote_reload(SshKeyMgr &m)
{
    if (!remote_available()) return;
    remote_lock();
    LIBSSH2_SESSION *sess = remote_session();
    std::string content;
    bool ok = sftp_read_file(sess, ".ssh/authorized_keys", content);
    remote_unlock();

    if (ok) {
        parse_authorized_keys(content, m.remote_auth);
        m.remote_loaded = true;
        if (m.remote_selected >= (int)m.remote_auth.size())
            m.remote_selected = (int)m.remote_auth.size() - 1;
        if (m.remote_selected < 0) m.remote_selected = 0;
        m.remote_status[0] = '\0';
    } else {
        m.remote_auth.clear();
        m.remote_loaded = true;  // treat as empty file (will create on first write)
        m.remote_status[0] = '\0';
    }
}

// Write m.remote_auth back to remote authorized_keys.
static bool remote_save(SshKeyMgr &m)
{
    std::string content;
    for (auto &e : m.remote_auth) {
        // Ensure no \r sneaks into the remote file regardless of where the line came from
        std::string line = e.line;
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        content += line + "\n";
    }
    remote_lock();
    bool ok = sftp_write_file(remote_session(), ".ssh/authorized_keys", content);
    remote_unlock();
    return ok;
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
// OPEN / CLOSE
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

    // Invalidate remote cache so it reloads fresh each open
    if (remote_available()) {
        if (g_ssh_key_mgr.remote_split < REMOTE_MIN_H)
            g_ssh_key_mgr.remote_split = REMOTE_DEF_H;
        g_ssh_key_mgr.remote_loaded = false;
        g_ssh_key_mgr.remote_auth.clear();
        g_ssh_key_mgr.remote_status[0] = '\0';
        g_ssh_key_mgr.remote_keys_loaded = false;
        g_ssh_key_mgr.remote_keys.clear();
        remote_reload(g_ssh_key_mgr);
        sftp_scan_remote_keys(g_ssh_key_mgr);
    }
}

void ssh_key_mgr_close()
{
    g_ssh_key_mgr.visible = false;
}

// ============================================================================
// RENDER HELPERS
// ============================================================================

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
    if (password) disp = std::string(strlen(value), '*');
    else          disp = value;
    dt(disp.c_str(), x + 6, y + h * 0.70f, 0.90f, 0.92f, 0.95f, 1.f);

    if (focused) {
        float cx = x + 6 + (float)(disp.size() * MFONT) * 0.56f;
        draw_rect(cx, y + 3, 1, h - 6, 0.70f, 0.85f, 1.0f, 0.80f);
    }
}

// ============================================================================
// RENDER — REMOTE PANEL  (bottom section of list pane when SSH active)
// ============================================================================

static void render_remote_panel(SshKeyMgr &m, float ox, float oy,
                                float pw, float ph)
{
    float panel_start = oy + ph - (float)m.remote_split;

    // ---- Draggable divider ----
    bool drag_hover = m.remote_dragging;
    float dh = (float)DIVIDER_H;
    draw_rect(ox + 1, panel_start, pw - 2, dh,
              drag_hover ? 0.18f : 0.12f,
              drag_hover ? 0.28f : 0.18f,
              drag_hover ? 0.50f : 0.32f, 1.f);
    draw_rect(ox + 1, panel_start, pw - 2, 1, 0.30f, 0.48f, 0.80f, 0.80f);
    // Drag grip dots
    float grip_cx = ox + pw * 0.5f;
    for (int i = -2; i <= 2; i++)
        draw_rect(grip_cx + i * 8 - 1, panel_start + dh * 0.5f - 1, 3, 3,
                  0.45f, 0.60f, 0.85f, 0.70f);

    // Remote label + tabs in divider
    dt("Remote:", ox + PAD, panel_start + dh * 0.72f, 0.45f, 0.55f, 0.75f, 1.f);

    // Tabs: "authorized_keys"  "Keys (~/.ssh)"
    float tab_x = ox + PAD + 70;
    auto draw_tab = [&](const char *label, float tx, bool active) -> float {
        float tw = (float)(strlen(label) * MFONT) * 0.56f + 16;
        if (active) draw_rect(tx, panel_start + 2, tw, dh - 2, 0.20f, 0.40f, 0.72f, 1.f);
        dt(label, tx + 8, panel_start + dh * 0.72f,
           active ? 0.95f : 0.55f, active ? 1.0f : 0.65f, 1.0f, 1.f,
           active ? ATTR_BOLD : 0);
        return tw;
    };
    float tw1 = draw_tab("authorized_keys", tab_x, m.remote_tab == RemoteTab::AUTH_KEYS);
    draw_tab("Keys (~/.ssh)", tab_x + tw1 + 4, m.remote_tab == RemoteTab::KEYS);

    dt("[R] refresh", ox + pw - 95, panel_start + dh * 0.72f, 0.28f, 0.30f, 0.42f, 1.f);

    float ry = panel_start + dh;

    // ---- Content area ----
    float row_area_bottom = oy + ph - BTN_H - PAD * 2 - (m.remote_status[0] ? MFONT + 4 : 0);
    bool has_focus = (m.list_focus == ListFocus::REMOTE);

    auto truncate_col = [&](const std::string &s, float max_w) -> std::string {
        if ((float)(s.size() * MFONT) * 0.56f <= max_w) return s;
        int mc = (int)(max_w / (MFONT * 0.56f));
        if (mc < 4) mc = 4;
        return s.substr(0, (size_t)mc - 3) + "...";
    };

    if (m.remote_tab == RemoteTab::AUTH_KEYS) {
        // Header
        float col_rtype    = ox + PAD;
        float col_rcomment = col_rtype + 90;
        float col_rfp      = col_rcomment + 180;
        draw_rect(ox + 1, ry, pw - 2, ROW_H, 0.12f, 0.16f, 0.26f, 1.f);
        dt("Type",        col_rtype,    ry + ROW_H * 0.68f, 0.45f, 0.55f, 0.75f, 1.f);
        dt("Comment",     col_rcomment, ry + ROW_H * 0.68f, 0.45f, 0.55f, 0.75f, 1.f);
        dt("Fingerprint", col_rfp,      ry + ROW_H * 0.68f, 0.45f, 0.55f, 0.75f, 1.f);
        ry += ROW_H;

        int max_rows = (int)((row_area_bottom - ry) / ROW_H);
        m.remote_visible_rows = max_rows > 0 ? max_rows : 1;

        if (m.remote_scroll > (int)m.remote_auth.size() - m.remote_visible_rows)
            m.remote_scroll = std::max(0, (int)m.remote_auth.size() - m.remote_visible_rows);
        int show = std::min((int)m.remote_auth.size() - m.remote_scroll, m.remote_visible_rows);

        if (m.remote_auth.empty())
            dt("No entries in remote authorized_keys", ox + PAD, ry + ROW_H * 0.68f, 0.40f, 0.40f, 0.50f, 1.f);

        for (int i = 0; i < show; i++) {
            int idx = m.remote_scroll + i;
            const RemoteAuthEntry &e = m.remote_auth[idx];
            bool sel = has_focus && (idx == m.remote_selected);
            if (sel)       draw_rect(ox + 1, ry, pw - 2, ROW_H, 0.18f, 0.35f, 0.65f, 0.55f);
            else if (i%2)  draw_rect(ox + 1, ry, pw - 2, ROW_H, 0.f,   0.f,   0.f,   0.08f);
            float fr = sel?0.92f:0.72f, fg = sel?0.95f:0.80f, fb = sel?1.00f:0.88f;
            dt(truncate_col(e.type,        col_rcomment-col_rtype-4).c_str(),   col_rtype,    ry+ROW_H*0.68f, fr,       fg*0.85f, fb*0.75f, 1.f);
            dt(truncate_col(e.comment,     col_rfp-col_rcomment-4).c_str(),     col_rcomment, ry+ROW_H*0.68f, fr*0.70f, fg*0.80f, fb*0.90f, 1.f);
            dt(truncate_col(e.fingerprint, ox+pw-PAD-col_rfp-4).c_str(),        col_rfp,      ry+ROW_H*0.68f, fr*0.75f, fg*0.85f, fb,       1.f);
            ry += ROW_H;
        }
    } else {
        // KEYS tab — same columns as local list
        float col_fn  = ox + PAD;
        float col_ty  = col_fn + 150;
        float col_bi  = col_ty + 70;
        float col_cm  = col_bi + 44;
        float col_fp  = col_cm + 150;
        draw_rect(ox + 1, ry, pw - 2, ROW_H, 0.12f, 0.16f, 0.26f, 1.f);
        dt("Filename",    col_fn,  ry + ROW_H * 0.68f, 0.45f, 0.55f, 0.75f, 1.f);
        dt("Type",        col_ty,  ry + ROW_H * 0.68f, 0.45f, 0.55f, 0.75f, 1.f);
        dt("Bits",        col_bi,  ry + ROW_H * 0.68f, 0.45f, 0.55f, 0.75f, 1.f);
        dt("Comment",     col_cm,  ry + ROW_H * 0.68f, 0.45f, 0.55f, 0.75f, 1.f);
        dt("Fingerprint", col_fp,  ry + ROW_H * 0.68f, 0.45f, 0.55f, 0.75f, 1.f);
        ry += ROW_H;

        int max_rows = (int)((row_area_bottom - ry) / ROW_H);
        m.remote_visible_rows = max_rows > 0 ? max_rows : 1;

        if (m.remote_keys_scroll > (int)m.remote_keys.size() - m.remote_visible_rows)
            m.remote_keys_scroll = std::max(0, (int)m.remote_keys.size() - m.remote_visible_rows);
        int show = std::min((int)m.remote_keys.size() - m.remote_keys_scroll, m.remote_visible_rows);

        if (m.remote_keys.empty())
            dt("No key pairs found in remote ~/.ssh", ox + PAD, ry + ROW_H * 0.68f, 0.40f, 0.40f, 0.50f, 1.f);

        for (int i = 0; i < show; i++) {
            int idx = m.remote_keys_scroll + i;
            const SshKeyEntry &e = m.remote_keys[idx];
            bool sel = has_focus && (idx == m.remote_keys_selected);
            if (sel)       draw_rect(ox + 1, ry, pw - 2, ROW_H, 0.18f, 0.35f, 0.65f, 0.55f);
            else if (i%2)  draw_rect(ox + 1, ry, pw - 2, ROW_H, 0.f,   0.f,   0.f,   0.08f);
            float fr = sel?0.92f:0.72f, fg = sel?0.95f:0.80f, fb = sel?1.00f:0.88f;
            char bits[16] = "-";
            if (e.bits > 0) snprintf(bits, sizeof(bits), "%d", e.bits);
            dt(truncate_col(e.filename,    col_ty-col_fn-4).c_str(),   col_fn, ry+ROW_H*0.68f, fr,       fg*0.90f, fb*0.80f, 1.f);
            dt(e.type.c_str(),                                          col_ty, ry+ROW_H*0.68f, fr,       fg,       fb,       1.f);
            dt(bits,                                                     col_bi, ry+ROW_H*0.68f, fr*0.85f, fg*0.85f, fb,       1.f);
            dt(truncate_col(e.comment,     col_fp-col_cm-4).c_str(),   col_cm, ry+ROW_H*0.68f, fr*0.70f, fg*0.80f, fb*0.90f, 1.f);
            dt(truncate_col(e.fingerprint, ox+pw-PAD-col_fp-4).c_str(),col_fp, ry+ROW_H*0.68f, fr*0.75f, fg*0.85f, fb,       1.f);
            ry += ROW_H;
        }
    }

    // ---- Remote buttons row ----
    float bby = oy + ph - BTN_H - PAD;
    float bbx = ox + PAD;
    bool has_local  = !m.keys.empty() && m.selected < (int)m.keys.size();
    bool has_rentry = (m.remote_tab == RemoteTab::AUTH_KEYS)
                      ? (!m.remote_auth.empty() && m.remote_selected < (int)m.remote_auth.size())
                      : (!m.remote_keys.empty() && m.remote_keys_selected < (int)m.remote_keys.size());

    if (m.remote_tab == RemoteTab::AUTH_KEYS) {
        if (has_local)  draw_btn("→ Auth Keys",  bbx,                   bby, BTN_W, BTN_H, false, true);
        if (has_rentry) draw_btn("Remove Entry", bbx + BTN_W + 8,        bby, BTN_W, BTN_H, false);
        if (has_rentry) draw_btn("Copy Pub Key", bbx + (BTN_W + 8) * 2, bby, BTN_W, BTN_H, false);
    } else {
        // Keys tab: download selected remote key to local ~/.ssh
        if (has_rentry) draw_btn("↓ Download Key", bbx,              bby, BTN_W, BTN_H, false, true);
        if (has_rentry) draw_btn("Copy Pub Key",   bbx + BTN_W + 8,  bby, BTN_W, BTN_H, false);
    }

    // Remote status
    if (m.remote_status[0]) {
        float sr = m.remote_status_ok ? 0.30f : 0.85f;
        float sg = m.remote_status_ok ? 0.85f : 0.30f;
        dt(m.remote_status, ox + PAD, bby - MFONT - 2, sr, sg, 0.30f, 1.f);
    }
}

// ============================================================================
// RENDER — LIST PANE
// ============================================================================

static void render_list_pane(SshKeyMgr &m, float ox, float oy,
                              float pw, float ph, int win_w, int win_h)
{
    (void)win_w; (void)win_h;

    bool has_remote = remote_available();

    // The local list section shrinks when the remote panel is showing
    float local_ph = has_remote ? ph - (float)m.remote_split : ph;

    float y = oy + PAD;

    // Section title
    dt("SSH Keys  (~/.ssh)", ox + PAD, y + MFONT * 0.85f, 0.55f, 0.75f, 1.0f, 1.f, ATTR_BOLD);
    y += MFONT + PAD / 2;

    // Column headers
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

    // Compute visible rows within the local section
    float list_bottom = oy + local_ph - BTN_H - PAD * 3 - (m.status[0] ? MFONT + 4 : 0);
    int max_rows = (int)((list_bottom - y) / ROW_H);
    m.visible_rows = max_rows > 0 ? max_rows : 1;

    if (m.scroll_top > (int)m.keys.size() - m.visible_rows)
        m.scroll_top = std::max(0, (int)m.keys.size() - m.visible_rows);

    int show = std::min((int)m.keys.size() - m.scroll_top, m.visible_rows);

    bool local_focus = (m.list_focus == ListFocus::LOCAL);

    if (m.keys.empty()) {
        dt("No SSH key pairs found in ~/.ssh",
           ox + PAD, y + ROW_H * 0.68f, 0.55f, 0.55f, 0.60f, 1.f);
    }

    for (int i = 0; i < show; i++) {
        int idx = m.scroll_top + i;
        const SshKeyEntry &e = m.keys[idx];
        bool sel = local_focus && (idx == m.selected);

        if (sel)
            draw_rect(ox + 1, y, pw - 2, ROW_H, 0.22f, 0.40f, 0.78f, 0.55f);
        else if (i % 2 == 0)
            draw_rect(ox + 1, y, pw - 2, ROW_H, 0.f, 0.f, 0.f, 0.10f);

        float fr = sel ? 0.95f : 0.82f,
              fg = sel ? 0.97f : 0.88f,
              fb = sel ? 1.00f : 0.92f;

        auto truncate_col = [&](const std::string &s, float max_w) -> std::string {
            if ((float)(s.size() * MFONT) * 0.56f <= max_w) return s;
            int mc = (int)(max_w / (MFONT * 0.56f));
            if (mc < 4) mc = 4;
            return s.substr(0, (size_t)mc - 3) + "...";
        };

        dt(truncate_col(e.filename, col_type - col_filename - 4).c_str(),
           col_filename, y + ROW_H * 0.68f, fr, fg * 0.90f, fb * 0.80f, 1.f);
        dt(e.type.c_str(), col_type, y + ROW_H * 0.68f, fr, fg, fb, 1.f);
        char bits[16] = "-";
        if (e.bits > 0) snprintf(bits, sizeof(bits), "%d", e.bits);
        dt(bits, col_bits, y + ROW_H * 0.68f, fr * 0.85f, fg * 0.85f, fb, 1.f);
        dt(truncate_col(e.comment, col_fp - col_comment - 4).c_str(),
           col_comment, y + ROW_H * 0.68f, fr * 0.70f, fg * 0.80f, fb * 0.90f, 1.f);
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

    // Local action buttons
    float bx = ox + PAD;
    float by = oy + local_ph - BTN_H - PAD;
    bool has_key = !m.keys.empty() && m.selected < (int)m.keys.size();

    draw_btn("+ Generate",   bx,                   by, BTN_W, BTN_H, false, true);
    if (has_key) {
        draw_btn("Copy Pub Key",  bx + BTN_W + 8,        by, BTN_W, BTN_H, false);
        draw_btn("Copy Priv Key", bx + (BTN_W + 8) * 2,  by, BTN_W, BTN_H, false);
        draw_btn("Auth Keys",     bx + (BTN_W + 8) * 3,  by, BTN_W, BTN_H, false);
        draw_btn("SSH Config",    bx + (BTN_W + 8) * 4,  by, BTN_W, BTN_H, false);
        draw_btn("Delete",        bx + (BTN_W + 8) * 5,  by, BTN_W, BTN_H, false);
    }
    draw_btn("Close (F8)",   ox + pw - BTN_W - PAD, by, BTN_W, BTN_H, false);

    // Local status line
    if (m.status[0]) {
        float sr = m.status_ok ? 0.30f : 0.85f;
        float sg = m.status_ok ? 0.85f : 0.30f;
        float sb2= m.status_ok ? 0.30f : 0.25f;
        dt(m.status, ox + PAD, by - MFONT - 2, sr, sg, sb2, 1.f);
    }

    // Remote panel
    if (has_remote)
        render_remote_panel(m, ox, oy, pw, ph);
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

    dt("Key type:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    float rx = ox + PAD + 90;
    float row1_y = y;
    for (int t = 0; t < KEY_TYPE_COUNT; t++) {
        if (t == 3) { rx = ox + PAD + 90; y += ROW_H; }
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

    dt("Filename:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    draw_field(m.gen_name, ox + PAD + 90, y, pw - PAD * 2 - 90, FIELD_H, m.gen_focus == 0);
    y += FIELD_H + PAD;

    dt("Comment:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    draw_field(m.gen_comment, ox + PAD + 90, y, pw - PAD * 2 - 90, FIELD_H, m.gen_focus == 1);
    y += FIELD_H + PAD;

    dt("Passphrase:", ox + PAD, y + MFONT * 0.85f, 0.70f, 0.75f, 0.82f, 1.f);
    draw_field(m.gen_passphrase, ox + PAD + 90, y, pw - PAD * 2 - 90 - BTN_W/2 - 4, FIELD_H,
               m.gen_focus == 2, !m.gen_pass_show);
    draw_btn(m.gen_pass_show ? "Hide" : "Show",
             ox + pw - PAD - BTN_W / 2, y, BTN_W / 2, FIELD_H, false);
    y += FIELD_H + PAD;

    if (m.gen_passphrase[0] == '\0')
        dt("(empty = no passphrase)", ox + PAD + 90, y, 0.50f, 0.52f, 0.56f, 1.f);
    y += MFONT + PAD;

    float bx = ox + PAD;
    float by = oy + ph - BTN_H - PAD;
    draw_btn("Generate", bx, by, BTN_W, BTN_H, false, true);
    draw_btn("← Back",  bx + BTN_W + 8, by, BTN_W, BTN_H, false);

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
// RENDER — CONFIRM REMOVE REMOTE ENTRY PANE
// ============================================================================

static void render_confirm_remove_remote_pane(SshKeyMgr &m, float ox, float oy,
                                              float pw, float ph, int, int)
{
    float cy = oy + ph * 0.35f;
    dt("Remove from remote authorized_keys?",
       ox + pw / 2 - 160, cy, 0.90f, 0.65f, 0.20f, 1.f, ATTR_BOLD);
    cy += MFONT + PAD;

    if (!m.remote_auth.empty() && m.remote_selected < (int)m.remote_auth.size()) {
        const auto &e = m.remote_auth[m.remote_selected];
        std::string label = e.type + "  " + (e.comment.empty() ? e.fingerprint : e.comment);
        float tw = (float)(label.size() * MFONT) * 0.56f;
        dt(label.c_str(), ox + (pw - tw) / 2, cy, 0.85f, 0.82f, 0.75f, 1.f);
    }

    float bx = ox + pw / 2 - BTN_W - 6;
    float by = oy + ph - BTN_H - PAD;
    draw_btn("Yes, Remove", bx,              by, BTN_W, BTN_H, false);
    draw_btn("Cancel",      bx + BTN_W + 12, by, BTN_W, BTN_H, false, true);
}

// ============================================================================
// RENDER — SSH CONFIG SNIPPET PANE
// ============================================================================

static void render_show_config_pane(SshKeyMgr &m, float ox, float oy,
                                    float pw, float ph, int, int)
{
    if (m.keys.empty() || m.selected >= (int)m.keys.size()) return;
    const SshKeyEntry &e = m.keys[m.selected];

    float y = oy + PAD;
    dt("~/.ssh/config  —  Host block for this key",
       ox + PAD, y + MFONT * 0.85f, 0.55f, 0.75f, 1.0f, 1.f, ATTR_BOLD);
    y += MFONT + PAD;

    // Disclaimer
    static const char *warn =
        "This snippet is for reference only.  Felix will not modify your config file.";
    static const char *warn2 =
        "Edit ~/.ssh/config manually — make a backup first.";
    draw_rect(ox + PAD - 4, y - 4, pw - PAD * 2 + 8, MFONT * 2 + PAD + 8,
              0.35f, 0.22f, 0.08f, 0.45f);
    draw_rect(ox + PAD - 4, y - 4, pw - PAD * 2 + 8, 1, 0.70f, 0.50f, 0.15f, 0.80f);
    dt(warn,  ox + PAD, y + MFONT * 0.85f,            0.90f, 0.70f, 0.30f, 1.f);
    y += MFONT + 4;
    dt(warn2, ox + PAD, y + MFONT * 0.85f,            0.75f, 0.58f, 0.25f, 1.f);
    y += MFONT + PAD + 8;

    // Build config snippet
    // Use the key comment as a suggested hostname if it looks like user@host,
    // otherwise fall back to a placeholder.
    std::string host_alias  = "myserver";
    std::string host_addr   = "myserver.example.com";
    std::string remote_user = "user";
    if (!e.comment.empty()) {
        // comment is often "user@hostname" — split on @
        size_t at = e.comment.find('@');
        if (at != std::string::npos) {
            remote_user = e.comment.substr(0, at);
            host_alias  = e.comment.substr(at + 1);
            host_addr   = host_alias;
        }
    }
#ifndef _WIN32
    std::string key_path = "~/.ssh/" + e.filename;
#else
    std::string key_path = "~/.ssh/" + e.filename;  // SSH on Windows still uses forward slashes here
#endif

    char snippet[1024];
    snprintf(snippet, sizeof(snippet),
             "Host %s\n"
             "    HostName %s\n"
             "    User %s\n"
             "    IdentityFile %s\n"
             "    IdentitiesOnly yes\n",
             host_alias.c_str(), host_addr.c_str(),
             remote_user.c_str(), key_path.c_str());

    // Render snippet in a code-style box
    float box_h = 5 * (MFONT + 4) + PAD;
    draw_rect(ox + PAD, y, pw - PAD * 2, box_h, 0.05f, 0.07f, 0.11f, 1.f);
    draw_rect(ox + PAD, y, pw - PAD * 2, 1, 0.25f, 0.40f, 0.70f, 0.60f);

    static const char *lines[] = {
        nullptr, nullptr, nullptr, nullptr, nullptr
    };
    // Split snippet into lines for rendering
    char tmp[1024];
    strncpy(tmp, snippet, sizeof(tmp) - 1);
    int li = 0;
    static char rendered[5][256];
    char *tok = tmp, *nl;
    while (li < 5 && (nl = strchr(tok, '\n'))) {
        *nl = '\0';
        strncpy(rendered[li], tok, 255);
        lines[li] = rendered[li];
        tok = nl + 1;
        li++;
    }

    float ly = y + PAD / 2;
    for (int i = 0; i < 5 && lines[i]; i++) {
        // Dim "Host" line differently from indented values
        bool is_host = (i == 0);
        dt(lines[i], ox + PAD + 8, ly + MFONT * 0.85f,
           is_host ? 0.55f : 0.45f,
           is_host ? 0.82f : 0.72f,
           is_host ? 1.00f : 0.90f,
           1.f, is_host ? ATTR_BOLD : 0);
        ly += MFONT + 4;
    }
    y += box_h + PAD;

    // Hint about editing
    dt("Replace Host / HostName / User with your actual values before saving.",
       ox + PAD, y + MFONT * 0.85f, 0.45f, 0.48f, 0.55f, 1.f);
    y += MFONT + PAD / 2;
    dt("IdentitiesOnly yes  prevents SSH from trying other keys first.",
       ox + PAD, y + MFONT * 0.85f, 0.40f, 0.42f, 0.50f, 1.f);

    // Buttons
    float bx = ox + PAD;
    float by = oy + ph - BTN_H - PAD;
    draw_btn("Copy Snippet", bx,             by, BTN_W, BTN_H, false, true);
    draw_btn("← Back",      bx + BTN_W + 8, by, BTN_W, BTN_H, false);
}

// ============================================================================
// MAIN RENDER
// ============================================================================

void ssh_key_mgr_render(int win_w, int win_h)
{
    SshKeyMgr &m = g_ssh_key_mgr;
    if (!m.visible) return;

    float pw = (float)std::max(win_w - 120, MIN_W);
    float ph = (float)std::max(win_h - 120, MIN_H);
    float ox = (float)((win_w - (int)pw) / 2);
    float oy = (float)((win_h - (int)ph) / 2);

    draw_rect(0, 0, (float)win_w, (float)win_h, 0.f, 0.f, 0.f, 0.60f);
    draw_rect(ox + 3, oy + 3, pw, ph, 0.f, 0.f, 0.f, 0.45f);
    draw_rect(ox, oy, pw, ph, 0.07f, 0.09f, 0.13f, 0.97f);

    draw_rect(ox,      oy,      pw, 1,  0.30f, 0.45f, 0.75f, 1.f);
    draw_rect(ox,      oy+ph-1, pw, 1,  0.30f, 0.45f, 0.75f, 1.f);
    draw_rect(ox,      oy,      1,  ph, 0.30f, 0.45f, 0.75f, 1.f);
    draw_rect(ox+pw-1, oy,      1,  ph, 0.30f, 0.45f, 0.75f, 1.f);

    draw_rect(ox + 1, oy + 1, pw - 2, TITLE_H, 0.12f, 0.18f, 0.35f, 1.f);
    draw_rect(ox + 1, oy + TITLE_H, pw - 2, 1, 0.25f, 0.40f, 0.70f, 1.f);
    dt("SSH Key Manager",
       ox + PAD, oy + TITLE_H * 0.72f,
       0.85f, 0.90f, 1.0f, 1.f, ATTR_BOLD);
    dt("F8 to close",
       ox + pw - 100, oy + TITLE_H * 0.72f,
       0.40f, 0.42f, 0.50f, 1.f);

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
    case KeyMgrPane::CONFIRM_REMOVE_REMOTE:
        render_confirm_remove_remote_pane(m, ox, content_oy, pw, content_ph, win_w, win_h);
        break;
    case KeyMgrPane::SHOW_CONFIG:
        render_show_config_pane(m, ox, content_oy, pw, content_ph, win_w, win_h);
        break;
    }

    gl_flush_verts();
}

// ============================================================================
// LOCAL ACTIONS
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
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    SDL_SetClipboardText(buf);
    snprintf(m.status, sizeof(m.status), "Public key copied to clipboard.");
    m.status_ok = true;
}

static void action_copy_privkey(SshKeyMgr &m)
{
    if (m.keys.empty() || m.selected >= (int)m.keys.size()) return;
    const SshKeyEntry &e = m.keys[m.selected];
    FILE *f = fopen(e.priv_path.c_str(), "r");
    if (!f) {
        snprintf(m.status, sizeof(m.status), "Error: cannot open private key.");
        m.status_ok = false;
        return;
    }
    char buf[16384] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    SDL_SetClipboardText(buf);
    snprintf(m.status, sizeof(m.status), "Private key copied to clipboard.");
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

// Extract the base64 blob token (second field) from a pub-key line.
static std::string pub_blob_token(const char *line)
{
    const char *p = strchr(line, ' ');
    if (!p) return {};
    p++; while (*p == ' ') p++;
    const char *e = strchr(p, ' ');
    std::string blob = e ? std::string(p, e - p) : std::string(p);
    rtrim(blob);
    return blob;
}

static void action_add_authorized_key(SshKeyMgr &m)
{
    if (m.keys.empty() || m.selected >= (int)m.keys.size()) return;
    const SshKeyEntry &e = m.keys[m.selected];

    FILE *fp = fopen(e.pub_path.c_str(), "r");
    if (!fp) {
        snprintf(m.status, sizeof(m.status), "Error: cannot open %s", e.pub_path.c_str());
        m.status_ok = false;
        return;
    }
    char pub_line[4096] = {};
    fread(pub_line, 1, sizeof(pub_line) - 1, fp);
    fclose(fp);
    size_t plen = strlen(pub_line);
    while (plen > 0 && (pub_line[plen-1] == '\n' || pub_line[plen-1] == '\r'))
        pub_line[--plen] = '\0';
    if (plen == 0) {
        snprintf(m.status, sizeof(m.status), "Error: public key file is empty.");
        m.status_ok = false;
        return;
    }

    std::string sshdir   = ssh_home_dir();
#ifndef _WIN32
    std::string auth_path = sshdir + "/authorized_keys";
#else
    std::string auth_path = sshdir + "\\authorized_keys";
#endif

    std::string new_blob = pub_blob_token(pub_line);
    if (!new_blob.empty()) {
        FILE *chk = fopen(auth_path.c_str(), "r");
        if (chk) {
            char line[4096];
            while (fgets(line, sizeof(line), chk)) {
                std::string existing = pub_blob_token(line);
                while (!existing.empty() && (existing.back() == '\n' || existing.back() == '\r'))
                    existing.pop_back();
                if (existing == new_blob) {
                    fclose(chk);
                    snprintf(m.status, sizeof(m.status), "Key already in authorized_keys.");
                    m.status_ok = false;
                    return;
                }
            }
            fclose(chk);
        }
    }

    FILE *af = fopen(auth_path.c_str(), "a");
    if (!af) {
        snprintf(m.status, sizeof(m.status), "Error: cannot write authorized_keys.");
        m.status_ok = false;
        return;
    }
    fprintf(af, "%s\n", pub_line);
    fclose(af);
    secure_file(auth_path);

    snprintf(m.status, sizeof(m.status), "Added %s to authorized_keys.", e.filename.c_str());
    m.status_ok = true;
}

// ============================================================================
// REMOTE ACTIONS
// ============================================================================

// Push selected local .pub → remote authorized_keys
static void action_remote_add_auth(SshKeyMgr &m)
{
    if (!remote_available()) return;
    if (m.keys.empty() || m.selected >= (int)m.keys.size()) return;
    const SshKeyEntry &e = m.keys[m.selected];

    FILE *fp = fopen(e.pub_path.c_str(), "rb");  // binary: we strip \r ourselves
    if (!fp) {
        snprintf(m.remote_status, sizeof(m.remote_status),
                 "Error: cannot open %s", e.pub_path.c_str());
        m.remote_status_ok = false;
        return;
    }
    char pub_line[4096] = {};
    fread(pub_line, 1, sizeof(pub_line) - 1, fp);
    fclose(fp);
    size_t plen = strlen(pub_line);
    while (plen > 0 && (pub_line[plen-1] == '\n' || pub_line[plen-1] == '\r' || pub_line[plen-1] == ' '))
        pub_line[--plen] = '\0';
    // Remove any embedded \r (Windows CRLF .pub files)
    { char *w = pub_line; for (char *r2 = pub_line; *r2; r2++) if (*r2 != '\r') *w++ = *r2; *w = '\0'; plen = strlen(pub_line); }
    if (plen == 0) {
        snprintf(m.remote_status, sizeof(m.remote_status), "Error: pub key file empty.");
        m.remote_status_ok = false;
        return;
    }

    // Duplicate check against already-loaded remote list
    std::string new_blob = pub_blob_token(pub_line);
    if (!new_blob.empty()) {
        for (auto &re : m.remote_auth) {
            if (pub_blob_token(re.line.c_str()) == new_blob) {
                snprintf(m.remote_status, sizeof(m.remote_status),
                         "Key already in remote authorized_keys.");
                m.remote_status_ok = false;
                return;
            }
        }
    }

    // Ensure remote ~/.ssh exists
    remote_lock();
    sftp_mkdir_ssh(remote_session(), ".ssh");
    remote_unlock();

    // Append to in-memory list and save
    RemoteAuthEntry re;
    re.line = pub_line;
    parse_pub_line(pub_line, re);
    m.remote_auth.push_back(re);

    if (remote_save(m)) {
        snprintf(m.remote_status, sizeof(m.remote_status),
                 "Added %s to remote authorized_keys.", e.filename.c_str());
        m.remote_status_ok = true;
    } else {
        m.remote_auth.pop_back();  // roll back
        snprintf(m.remote_status, sizeof(m.remote_status),
                 "Error: SFTP write failed.");
        m.remote_status_ok = false;
    }
}

// Remove selected remote authorized_keys entry
static void action_remote_remove_auth(SshKeyMgr &m)
{
    if (!remote_available()) return;
    if (m.remote_auth.empty() || m.remote_selected >= (int)m.remote_auth.size()) return;

    m.remote_auth.erase(m.remote_auth.begin() + m.remote_selected);
    if (m.remote_selected >= (int)m.remote_auth.size())
        m.remote_selected = (int)m.remote_auth.size() - 1;
    if (m.remote_selected < 0) m.remote_selected = 0;

    m.pane = KeyMgrPane::LIST;
    if (remote_save(m)) {
        snprintf(m.remote_status, sizeof(m.remote_status), "Entry removed.");
        m.remote_status_ok = true;
    } else {
        snprintf(m.remote_status, sizeof(m.remote_status), "Error: SFTP write failed.");
        m.remote_status_ok = false;
        remote_reload(m);  // re-sync from server
    }
}

// Upload selected local private key (and .pub) to remote ~/.ssh/
static void action_remote_copy_privkey(SshKeyMgr &m)
{
    if (!remote_available()) return;
    if (m.keys.empty() || m.selected >= (int)m.keys.size()) return;
    const SshKeyEntry &e = m.keys[m.selected];

    // Read local private key
    FILE *fp = fopen(e.priv_path.c_str(), "rb");
    if (!fp) {
        snprintf(m.remote_status, sizeof(m.remote_status),
                 "Error: cannot read local private key.");
        m.remote_status_ok = false;
        return;
    }
    std::string priv_content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        priv_content.append(buf, n);
    fclose(fp);

    // Read local public key
    FILE *fp2 = fopen(e.pub_path.c_str(), "r");
    if (!fp2) {
        snprintf(m.remote_status, sizeof(m.remote_status),
                 "Error: cannot read local public key.");
        m.remote_status_ok = false;
        return;
    }
    std::string pub_content;
    while ((n = fread(buf, 1, sizeof(buf), fp2)) > 0)
        pub_content.append(buf, n);
    fclose(fp2);

    remote_lock();
    sftp_mkdir_ssh(remote_session(), ".ssh");
    bool ok1 = sftp_write_file(remote_session(),
                               ".ssh/" + e.filename, priv_content);
    bool ok2 = ok1 && sftp_write_file(remote_session(),
                               ".ssh/" + e.filename + ".pub", pub_content);
    remote_unlock();

    if (ok2) {
        snprintf(m.remote_status, sizeof(m.remote_status),
                 "Uploaded %s to remote ~/.ssh/", e.filename.c_str());
        m.remote_status_ok = true;
    } else {
        snprintf(m.remote_status, sizeof(m.remote_status),
                 "Error: SFTP upload failed.");
        m.remote_status_ok = false;
    }
}

// Download selected remote key pair to local ~/.ssh/
static void action_remote_download_key(SshKeyMgr &m)
{
    if (!remote_available()) return;
    if (m.remote_keys.empty() || m.remote_keys_selected >= (int)m.remote_keys.size()) return;
    const SshKeyEntry &rk = m.remote_keys[m.remote_keys_selected];

    // Read both files from remote
    remote_lock();
    std::string priv_content, pub_content;
    bool ok1 = sftp_read_file(remote_session(), rk.priv_path, priv_content);
    bool ok2 = ok1 && sftp_read_file(remote_session(), rk.pub_path, pub_content);
    remote_unlock();

    if (!ok2) {
        snprintf(m.remote_status, sizeof(m.remote_status), "Error: SFTP read failed.");
        m.remote_status_ok = false;
        return;
    }

    // Write to local ~/.ssh/
    std::string sshdir = ssh_home_dir();
#ifndef _WIN32
    std::string local_priv = sshdir + "/" + rk.filename;
    std::string local_pub  = sshdir + "/" + rk.filename + ".pub";
    mkdir(sshdir.c_str(), 0700);
#else
    std::string local_priv = sshdir + "\\" + rk.filename;
    std::string local_pub  = sshdir + "\\" + rk.filename + ".pub";
    CreateDirectoryA(sshdir.c_str(), nullptr);
#endif

    FILE *fp = fopen(local_priv.c_str(), "wb");
    bool wok = fp != nullptr;
    if (fp) { fwrite(priv_content.c_str(), 1, priv_content.size(), fp); fclose(fp); secure_file(local_priv); }

    FILE *fp2 = fopen(local_pub.c_str(), "w");
    wok = wok && fp2 != nullptr;
    if (fp2) { fwrite(pub_content.c_str(), 1, pub_content.size(), fp2); fclose(fp2); }

    if (wok) {
        snprintf(m.remote_status, sizeof(m.remote_status),
                 "Downloaded %s to local ~/.ssh/", rk.filename.c_str());
        m.remote_status_ok = true;
        scan_keys(m); sort_keys(m);  // refresh local list
    } else {
        snprintf(m.remote_status, sizeof(m.remote_status), "Error: local write failed.");
        m.remote_status_ok = false;
    }
}

// Copy selected remote .pub key to clipboard
static void action_remote_copy_remote_pubkey(SshKeyMgr &m)
{
    if (!remote_available()) return;
    if (m.remote_keys.empty() || m.remote_keys_selected >= (int)m.remote_keys.size()) return;
    const SshKeyEntry &rk = m.remote_keys[m.remote_keys_selected];

    remote_lock();
    std::string content;
    bool ok = sftp_read_file(remote_session(), rk.pub_path, content);
    remote_unlock();

    if (!ok || content.empty()) {
        snprintf(m.remote_status, sizeof(m.remote_status), "Error: could not read remote pub key.");
        m.remote_status_ok = false;
        return;
    }
    // Strip trailing newline for clean clipboard
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
        content.pop_back();
    SDL_SetClipboardText(content.c_str());
    snprintf(m.remote_status, sizeof(m.remote_status),
             "Remote pub key %s copied to clipboard.", rk.filename.c_str());
    m.remote_status_ok = true;
}
// ============================================================================
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>

static bool write_openssh_pubkey(EVP_PKEY *pkey, const char *comment, const std::string &path)
{
    // RSA/EC wire-format serialization uses the pre-3.0 low-level API.
    // These are deprecated in OpenSSL 3.0 but still present; suppress the noise.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
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
#pragma GCC diagnostic pop
}

static void action_generate_do(SshKeyMgr &m)
{
    std::string sshdir  = ssh_home_dir();
#ifndef _WIN32
    std::string outpath = sshdir + "/" + m.gen_name;
#else
    std::string outpath = sshdir + "\\" + m.gen_name;
#endif
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
    case 0:
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        gen_ok = ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0;
        break;
    case 1: case 2: case 3: {
        int nid = (m.gen_type == 1) ? NID_X9_62_prime256v1 :
                  (m.gen_type == 2) ? NID_secp384r1 : NID_secp521r1;
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        gen_ok = ctx && EVP_PKEY_keygen_init(ctx) > 0 &&
                 EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid) > 0 &&
                 EVP_PKEY_keygen(ctx, &pkey) > 0;
        break; }
    case 4: {
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

    const EVP_CIPHER *cipher = nullptr;
    const char *pp = m.gen_passphrase;
    if (pp[0] != '\0') cipher = EVP_aes_256_cbc();

    FILE *fpriv = fopen(outpath.c_str(), "wb");
    bool ok = fpriv && PEM_write_PrivateKey(fpriv, pkey, cipher,
                           pp[0] ? (const unsigned char *)pp : nullptr,
                           pp[0] ? (int)strlen(pp) : 0,
                           nullptr, nullptr);
    if (fpriv) {
        fclose(fpriv);
        secure_file(outpath);
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
#ifndef _WIN32
    std::string outpath = ssh_home_dir() + "/" + m.gen_name;
#else
    std::string outpath = ssh_home_dir() + "\\" + m.gen_name;
#endif
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

    if (m.pane == KeyMgrPane::LIST) {
        // Tab switches focus between local and remote panels
        if (sym == SDLK_TAB && remote_available()) {
            m.list_focus = (m.list_focus == ListFocus::LOCAL)
                         ? ListFocus::REMOTE : ListFocus::LOCAL;
            return true;
        }

        if (m.list_focus == ListFocus::LOCAL) {
            if (sym == SDLK_UP)   { if (m.selected > 0) m.selected--; return true; }
            if (sym == SDLK_DOWN) {
                if (m.selected < (int)m.keys.size() - 1) m.selected++;
                return true;
            }
            if (sym == SDLK_RETURN) { action_copy_pubkey(m); return true; }
            if (sym == SDLK_DELETE || sym == SDLK_BACKSPACE) {
                if (!m.keys.empty()) { m.pane = KeyMgrPane::CONFIRM_DELETE; m.status[0] = '\0'; }
                return true;
            }
            if (sym == SDLK_n || sym == SDLK_INSERT) {
                m.pane = KeyMgrPane::GENERATE;
                m.status[0] = '\0';
                m.gen_focus = 0;
                strncpy(m.gen_name, key_types[m.gen_type].default_name, sizeof(m.gen_name) - 1);
                return true;
            }
        } else {
            // Remote panel focus
            if (sym == SDLK_UP) {
                if (m.remote_tab == RemoteTab::AUTH_KEYS) { if (m.remote_selected > 0) m.remote_selected--; }
                else                                       { if (m.remote_keys_selected > 0) m.remote_keys_selected--; }
                return true;
            }
            if (sym == SDLK_DOWN) {
                if (m.remote_tab == RemoteTab::AUTH_KEYS) {
                    if (m.remote_selected < (int)m.remote_auth.size() - 1) m.remote_selected++;
                } else {
                    if (m.remote_keys_selected < (int)m.remote_keys.size() - 1) m.remote_keys_selected++;
                }
                return true;
            }
            if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
                m.remote_tab = (m.remote_tab == RemoteTab::AUTH_KEYS) ? RemoteTab::KEYS : RemoteTab::AUTH_KEYS;
                if (m.remote_tab == RemoteTab::KEYS && !m.remote_keys_loaded)
                    sftp_scan_remote_keys(m);
                return true;
            }
            if (sym == SDLK_DELETE || sym == SDLK_BACKSPACE) {
                if (m.remote_tab == RemoteTab::AUTH_KEYS && !m.remote_auth.empty()) {
                    m.pane = KeyMgrPane::CONFIRM_REMOVE_REMOTE;
                    m.remote_status[0] = '\0';
                }
                return true;
            }
        }

        if (sym == SDLK_r) {
            scan_keys(m);
            sort_keys(m);
            if (remote_available()) {
                remote_reload(m);
                sftp_scan_remote_keys(m);
            }
            snprintf(m.status, sizeof(m.status), "Refreshed.");
            m.status_ok = true;
            return true;
        }
    }

    else if (m.pane == KeyMgrPane::GENERATE) {
        if (sym == SDLK_TAB) { m.gen_focus = (m.gen_focus + 1) % 3; return true; }
        if (sym == SDLK_RETURN) { action_generate(m); return true; }
        if (sym == SDLK_BACKSPACE) {
            switch (m.gen_focus) {
            case 0: field_backspace(m.gen_name);       break;
            case 1: field_backspace(m.gen_comment);    break;
            case 2: field_backspace(m.gen_passphrase); break;
            }
            return true;
        }
        if ((ks.mod & KMOD_CTRL) && sym == SDLK_a) {
            switch (m.gen_focus) {
            case 0: m.gen_name[0]       = '\0'; break;
            case 1: m.gen_comment[0]    = '\0'; break;
            case 2: m.gen_passphrase[0] = '\0'; break;
            }
            return true;
        }
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

    else if (m.pane == KeyMgrPane::CONFIRM_DELETE) {
        if (sym == SDLK_RETURN || sym == SDLK_y) { action_delete_key(m); return true; }
        if (sym == SDLK_n) { m.pane = KeyMgrPane::LIST; m.status[0] = '\0'; return true; }
    }
    else if (m.pane == KeyMgrPane::CONFIRM_OVERWRITE) {
        if (sym == SDLK_RETURN || sym == SDLK_y) { action_generate_do(m); return true; }
        if (sym == SDLK_n || sym == SDLK_ESCAPE) { m.pane = KeyMgrPane::GENERATE; m.status[0] = '\0'; return true; }
    }
    else if (m.pane == KeyMgrPane::CONFIRM_REMOVE_REMOTE) {
        if (sym == SDLK_RETURN || sym == SDLK_y) { action_remote_remove_auth(m); return true; }
        if (sym == SDLK_n) { m.pane = KeyMgrPane::LIST; m.remote_status[0] = '\0'; return true; }
    }
    else if (m.pane == KeyMgrPane::SHOW_CONFIG) {
        // Enter / C copies snippet; any other key or Escape handled by the generic F8/Esc block above
        if (sym == SDLK_RETURN || sym == SDLK_c) {
            if (!m.keys.empty() && m.selected < (int)m.keys.size()) {
                const SshKeyEntry &e = m.keys[m.selected];
                std::string host_alias = "myserver", host_addr = "myserver.example.com", remote_user = "user";
                if (!e.comment.empty()) {
                    size_t at = e.comment.find('@');
                    if (at != std::string::npos) {
                        remote_user = e.comment.substr(0, at);
                        host_alias  = e.comment.substr(at + 1);
                        host_addr   = host_alias;
                    }
                }
                char snippet[1024];
                snprintf(snippet, sizeof(snippet),
                         "Host %s\n    HostName %s\n    User %s\n"
                         "    IdentityFile ~/.ssh/%s\n    IdentitiesOnly yes\n",
                         host_alias.c_str(), host_addr.c_str(),
                         remote_user.c_str(), e.filename.c_str());
                SDL_SetClipboardText(snippet);
                snprintf(m.status, sizeof(m.status), "Config snippet copied to clipboard.");
                m.status_ok = true;
            }
            return true;
        }
    }

    return true;
}

// ============================================================================
// MOUSE
// ============================================================================

static float content_oy_of(float oy, float /*ph*/) { return oy + TITLE_H + 1; }
static float content_ph_of(float ph)               { return ph - TITLE_H - 1; }

bool ssh_key_mgr_mousedown(int mx, int my, int /*button*/)
{
    SshKeyMgr &m = g_ssh_key_mgr;
    if (!m.visible) return false;
    m.remote_dragging = false;  // any click cancels an in-progress drag

    int win_w, win_h;
    SDL_GetWindowSize(SDL_GL_GetCurrentWindow(), &win_w, &win_h);
    float pw = (float)std::max(win_w - 120, MIN_W);
    float ph = (float)std::max(win_h - 120, MIN_H);
    float ox = (float)((win_w - (int)pw) / 2);
    float oy = (float)((win_h - (int)ph) / 2);
    float coy = content_oy_of(oy, ph);
    float cph = content_ph_of(ph);

    if ((mx < ox || mx > ox + pw || my < oy || my > oy + ph) &&
        m.pane == KeyMgrPane::LIST) {
        ssh_key_mgr_close();
        return true;
    }

    if (m.pane == KeyMgrPane::LIST) {
        bool has_remote = remote_available();
        float local_ph = has_remote ? cph - (float)m.remote_split : cph;

        // Remote panel area
        if (has_remote) {
            float rp_top = coy + cph - (float)m.remote_split;

            // Divider drag start
            if (my >= rp_top && my < rp_top + DIVIDER_H && mx >= ox && mx < ox + pw) {
                // Tab clicks (within divider)
                float tab_x = ox + PAD + 70;
                float tw1 = (float)(strlen("authorized_keys") * MFONT) * 0.56f + 16;
                float tw2 = (float)(strlen("Keys (~/.ssh)") * MFONT) * 0.56f + 16;
                if (mx >= tab_x && mx < tab_x + tw1) {
                    m.remote_tab = RemoteTab::AUTH_KEYS;
                    m.list_focus = ListFocus::REMOTE;
                    return true;
                }
                if (mx >= tab_x + tw1 + 4 && mx < tab_x + tw1 + 4 + tw2) {
                    m.remote_tab = RemoteTab::KEYS;
                    m.list_focus = ListFocus::REMOTE;
                    if (!m.remote_keys_loaded) sftp_scan_remote_keys(m);
                    return true;
                }
                // Start drag
                m.remote_dragging     = true;
                m.remote_drag_start_y = my;
                m.remote_drag_start_split = m.remote_split;
                return true;
            }

            // Remote row clicks
            float rrow_y = rp_top + DIVIDER_H + ROW_H;
            float rrow_bottom = coy + cph - BTN_H - PAD * 2 - (m.remote_status[0] ? MFONT + 4 : 0);

            if (m.remote_tab == RemoteTab::AUTH_KEYS) {
                for (int i = 0; i < m.remote_visible_rows && m.remote_scroll + i < (int)m.remote_auth.size(); i++) {
                    float ry = rrow_y + i * ROW_H;
                    if (ry >= rrow_bottom) break;
                    if (my >= ry && my < ry + ROW_H && mx >= ox && mx < ox + pw) {
                        m.remote_selected = m.remote_scroll + i;
                        m.list_focus = ListFocus::REMOTE;
                        return true;
                    }
                }
            } else {
                for (int i = 0; i < m.remote_visible_rows && m.remote_keys_scroll + i < (int)m.remote_keys.size(); i++) {
                    float ry = rrow_y + i * ROW_H;
                    if (ry >= rrow_bottom) break;
                    if (my >= ry && my < ry + ROW_H && mx >= ox && mx < ox + pw) {
                        m.remote_keys_selected = m.remote_keys_scroll + i;
                        m.list_focus = ListFocus::REMOTE;
                        return true;
                    }
                }
            }

            // Remote buttons
            float bby = coy + cph - BTN_H - PAD;
            float bbx = ox + PAD;
            bool has_local  = !m.keys.empty() && m.selected < (int)m.keys.size();
            bool has_rentry_auth = !m.remote_auth.empty() && m.remote_selected < (int)m.remote_auth.size();
            bool has_rentry_keys = !m.remote_keys.empty() && m.remote_keys_selected < (int)m.remote_keys.size();

            if (m.remote_tab == RemoteTab::AUTH_KEYS) {
                if (has_local) {
                    Btn b_radd = { bbx, bby, BTN_W, BTN_H };
                    if (btn_hit(b_radd, mx, my)) { action_remote_add_auth(m); return true; }
                }
                if (has_rentry_auth) {
                    Btn b_rrem = { bbx + BTN_W + 8,        bby, BTN_W, BTN_H };
                    Btn b_cpub = { bbx + (BTN_W + 8) * 2,  bby, BTN_W, BTN_H };
                    if (btn_hit(b_rrem, mx, my)) {
                        m.pane = KeyMgrPane::CONFIRM_REMOVE_REMOTE;
                        m.remote_status[0] = '\0';
                        return true;
                    }
                    if (btn_hit(b_cpub, mx, my)) {
                        // Copy the raw authorized_keys line to clipboard
                        SDL_SetClipboardText(m.remote_auth[m.remote_selected].line.c_str());
                        snprintf(m.remote_status, sizeof(m.remote_status), "Public key copied to clipboard.");
                        m.remote_status_ok = true;
                        return true;
                    }
                }
            } else {
                if (has_rentry_keys) {
                    Btn b_dl  = { bbx,             bby, BTN_W, BTN_H };
                    Btn b_cpb = { bbx + BTN_W + 8, bby, BTN_W, BTN_H };
                    if (btn_hit(b_dl,  mx, my)) { action_remote_download_key(m); return true; }
                    if (btn_hit(b_cpb, mx, my)) { action_remote_copy_remote_pubkey(m); return true; }
                }
            }
        }

        // Local column header clicks
        float hdr_y        = coy + PAD + MFONT + PAD / 2;
        float col_filename = ox + PAD;
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

        // Local row clicks
        float list_y = coy + PAD + MFONT + PAD / 2 + ROW_H;
        float list_bottom = coy + local_ph - BTN_H - PAD * 3;
        for (int i = 0; i < m.visible_rows && m.scroll_top + i < (int)m.keys.size(); i++) {
            float ry = list_y + i * ROW_H;
            if (ry >= list_bottom) break;
            if (my >= ry && my < ry + ROW_H && mx >= ox && mx < ox + pw) {
                m.selected = m.scroll_top + i;
                m.list_focus = ListFocus::LOCAL;
                return true;
            }
        }

        // Local buttons
        float by = coy + local_ph - BTN_H - PAD;
        float bx = ox + PAD;
        bool has_key = !m.keys.empty();

        Btn b_gen   = { bx,                   by, BTN_W, BTN_H };
        Btn b_copy  = { bx + BTN_W + 8,        by, BTN_W, BTN_H };
        Btn b_cpriv = { bx + (BTN_W + 8) * 2, by, BTN_W, BTN_H };
        Btn b_auth  = { bx + (BTN_W + 8) * 3, by, BTN_W, BTN_H };
        Btn b_cfg   = { bx + (BTN_W + 8) * 4, by, BTN_W, BTN_H };
        Btn b_del   = { bx + (BTN_W + 8) * 5, by, BTN_W, BTN_H };
        Btn b_close = { ox + pw - BTN_W - PAD, by, BTN_W, BTN_H };

        if (btn_hit(b_gen, mx, my)) {
            m.pane = KeyMgrPane::GENERATE;
            m.status[0] = '\0';
            m.gen_focus = 0;
            return true;
        }
        if (has_key && btn_hit(b_copy,  mx, my)) { action_copy_pubkey(m);  return true; }
        if (has_key && btn_hit(b_cpriv, mx, my)) { action_copy_privkey(m); return true; }
        if (has_key && btn_hit(b_auth,  mx, my)) { action_add_authorized_key(m); return true; }
        if (has_key && btn_hit(b_cfg,   mx, my)) {
            m.pane = KeyMgrPane::SHOW_CONFIG;
            m.status[0] = '\0';
            return true;
        }
        if (has_key && btn_hit(b_del,   mx, my)) {
            m.pane = KeyMgrPane::CONFIRM_DELETE;
            m.status[0] = '\0';
            return true;
        }
        if (btn_hit(b_close, mx, my)) { ssh_key_mgr_close(); return true; }
    }
    else if (m.pane == KeyMgrPane::GENERATE) {
        float y = coy + PAD + MFONT + PAD;

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
        y += ROW_H * 2 + 8;

        if (m.gen_type == 4) {
            float bx2 = ox + PAD + 90;
            for (int i = 0; i < RSA_SIZE_COUNT; i++) {
                Btn b = { bx2, y, 90, BTN_H };
                if (btn_hit(b, mx, my)) { m.gen_rsa_size = i; return true; }
                bx2 += 98;
            }
            y += BTN_H + PAD;
        }

        float fw = pw - PAD * 2 - 90;
        float fx = ox + PAD + 90;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 0; return true; }
        y += FIELD_H + PAD;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 1; return true; }
        y += FIELD_H + PAD;
        if (my >= y && my < y + FIELD_H && mx >= fx && mx < fx + fw) { m.gen_focus = 2; return true; }

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
    else if (m.pane == KeyMgrPane::SHOW_CONFIG) {
        float by = coy + cph - BTN_H - PAD;
        float bx = ox + PAD;
        Btn b_copy = { bx,             by, BTN_W, BTN_H };
        Btn b_back = { bx + BTN_W + 8, by, BTN_W, BTN_H };
        if (btn_hit(b_copy, mx, my)) {
            // Rebuild snippet and copy — same logic as render
            if (!m.keys.empty() && m.selected < (int)m.keys.size()) {
                const SshKeyEntry &e = m.keys[m.selected];
                std::string host_alias = "myserver", host_addr = "myserver.example.com", remote_user = "user";
                if (!e.comment.empty()) {
                    size_t at = e.comment.find('@');
                    if (at != std::string::npos) {
                        remote_user = e.comment.substr(0, at);
                        host_alias  = e.comment.substr(at + 1);
                        host_addr   = host_alias;
                    }
                }
                char snippet[1024];
                snprintf(snippet, sizeof(snippet),
                         "Host %s\n    HostName %s\n    User %s\n"
                         "    IdentityFile ~/.ssh/%s\n    IdentitiesOnly yes\n",
                         host_alias.c_str(), host_addr.c_str(),
                         remote_user.c_str(), e.filename.c_str());
                SDL_SetClipboardText(snippet);
                snprintf(m.status, sizeof(m.status), "Config snippet copied to clipboard.");
                m.status_ok = true;
            }
            return true;
        }
        if (btn_hit(b_back, mx, my)) { m.pane = KeyMgrPane::LIST; m.status[0] = '\0'; return true; }
    }
    else if (m.pane == KeyMgrPane::CONFIRM_REMOVE_REMOTE) {
        float by = coy + cph - BTN_H - PAD;
        float bx = ox + pw / 2 - BTN_W - 6;
        Btn b_yes = { bx,              by, BTN_W, BTN_H };
        Btn b_no  = { bx + BTN_W + 12, by, BTN_W, BTN_H };
        if (btn_hit(b_yes, mx, my)) { action_remote_remove_auth(m); return true; }
        if (btn_hit(b_no,  mx, my)) { m.pane = KeyMgrPane::LIST; m.remote_status[0] = '\0'; return true; }
    }

    return true;
}

bool ssh_key_mgr_mousemotion(int /*x*/, int y, bool lbutton)
{
    SshKeyMgr &m = g_ssh_key_mgr;
    if (!m.visible || !m.remote_dragging) return false;
    if (!lbutton) { m.remote_dragging = false; return false; }

    int win_w, win_h;
    SDL_GetWindowSize(SDL_GL_GetCurrentWindow(), &win_w, &win_h);
    float ph = (float)std::max(win_h - 120, MIN_H) - TITLE_H - 1;

    int delta = m.remote_drag_start_y - y;
    int new_split = m.remote_drag_start_split + delta;
    int max_split = (int)ph - REMOTE_MIN_H;
    if (new_split < REMOTE_MIN_H) new_split = REMOTE_MIN_H;
    if (new_split > max_split)    new_split = max_split;
    m.remote_split = new_split;
    return true;
}

void ssh_key_mgr_scroll(int delta_y)
{
    SshKeyMgr &m = g_ssh_key_mgr;
    if (!m.visible || m.pane != KeyMgrPane::LIST) return;

    if (m.list_focus == ListFocus::REMOTE) {
        if (m.remote_tab == RemoteTab::AUTH_KEYS) {
            m.remote_scroll -= delta_y;
            int max_top = (int)m.remote_auth.size() - m.remote_visible_rows;
            if (m.remote_scroll > max_top) m.remote_scroll = max_top;
            if (m.remote_scroll < 0)       m.remote_scroll = 0;
        } else {
            m.remote_keys_scroll -= delta_y;
            int max_top = (int)m.remote_keys.size() - m.remote_visible_rows;
            if (m.remote_keys_scroll > max_top) m.remote_keys_scroll = max_top;
            if (m.remote_keys_scroll < 0)       m.remote_keys_scroll = 0;
        }
    } else {
        m.scroll_top -= delta_y;
        int max_top = (int)m.keys.size() - m.visible_rows;
        if (m.scroll_top > max_top) m.scroll_top = max_top;
        if (m.scroll_top < 0)       m.scroll_top = 0;
    }
}
