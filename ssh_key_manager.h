#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>

// ============================================================================
// SSH KEY MANAGER OVERLAY  (F8)
// ============================================================================
// Lists ~/.ssh keys, shows fingerprint/type/comment, lets the user generate
// new Ed25519 / RSA keys, copy the public key to clipboard, and delete
// key pairs — all without leaving the terminal.
//
// When an SSH session is active (USESSH + ssh_active()), a remote panel is
// shown beneath the local list:
//   • Reads ~/.ssh/authorized_keys from the remote via SFTP
//   • Add / Remove entries from remote authorized_keys
//   • Copy Private Key — uploads selected local private key to remote ~/.ssh/
//
// No libssh2 or ssh-keygen dependency for local ops; uses OpenSSL for key
// generation and writes standard OpenSSH wire format directly.

struct SshKeyEntry {
    std::string priv_path;   // e.g. /home/user/.ssh/id_ed25519
    std::string pub_path;    // priv_path + ".pub"
    std::string filename;    // basename of priv_path, e.g. "id_ed25519"
    std::string type;        // "ED25519", "RSA", "ECDSA", …
    std::string comment;     // last token of pub key
    std::string fingerprint; // SHA256:…
    int         bits = 0;
};

// One line from remote ~/.ssh/authorized_keys (non-blank, non-comment)
struct RemoteAuthEntry {
    std::string line;        // raw line as stored in the file
    std::string type;        // key type token
    std::string comment;     // comment token (last field)
    std::string fingerprint; // SHA256:… computed from blob
};

enum class KeyMgrPane {
    LIST,
    GENERATE,
    CONFIRM_DELETE,
    CONFIRM_OVERWRITE,
    CONFIRM_REMOVE_REMOTE,  // confirm removing an authorized_keys entry
    SHOW_CONFIG             // read-only ~/.ssh/config snippet for selected key
};
enum class KeySortCol  { NONE, FILENAME, FINGERPRINT, COMMENT };
enum class KeySortDir  { ASC, DESC };

// Which half of the split list pane has keyboard focus
enum class ListFocus { LOCAL, REMOTE };

// Which tab is active in the remote panel
enum class RemoteTab { AUTH_KEYS, KEYS };

struct SshKeyMgr {
    bool    visible    = false;
    KeyMgrPane pane    = KeyMgrPane::LIST;

    // ---- local keys ----
    std::vector<SshKeyEntry> keys;
    int  selected      = 0;   // index in keys
    int  scroll_top    = 0;

    // Column sorting
    KeySortCol sort_col = KeySortCol::NONE;
    KeySortDir sort_dir = KeySortDir::ASC;

    // ---- remote authorized_keys ----
    // Populated only when ssh_active(); empty otherwise.
    std::vector<RemoteAuthEntry> remote_auth;
    int  remote_selected  = 0;
    int  remote_scroll    = 0;
    bool remote_loaded    = false;   // have we fetched the remote file this session?
    char remote_status[256] = {};    // separate feedback for remote ops
    bool remote_status_ok  = true;

    // ---- remote keys (~/.ssh/*.pub on the server) ----
    std::vector<SshKeyEntry> remote_keys;   // pub_path/priv_path unused; filename/type/comment/fingerprint/bits filled
    int  remote_keys_selected = 0;
    int  remote_keys_scroll   = 0;
    bool remote_keys_loaded   = false;

    // ---- remote panel layout ----
    RemoteTab remote_tab      = RemoteTab::AUTH_KEYS;
    int  remote_split         = 280;   // pixels from bottom for remote panel; user-draggable
    bool remote_dragging      = false;
    int  remote_drag_start_y  = 0;
    int  remote_drag_start_split = 0;

    // ---- generate pane ----
    char  gen_name[128]    = "id_ed25519";
    char  gen_comment[256] = {};
    char  gen_passphrase[256] = {};
    int   gen_type     = 0;   // 0=Ed25519, 1=ECDSA-256, 2=ECDSA-384, 3=ECDSA-521, 4=RSA
    int   gen_rsa_size = 1;   // index into rsa_sizes[] = {2048,3072,4096}
    bool  gen_dropdown_open = false;
    char  status[512]  = {};
    bool  status_ok    = true;

    int   gen_focus    = 0;
    bool  gen_pass_show = false;

    // ---- layout (computed each frame) ----
    int visible_rows        = 0;   // local list
    int remote_visible_rows = 0;   // remote auth/keys list
    ListFocus list_focus    = ListFocus::LOCAL;
};

extern SshKeyMgr g_ssh_key_mgr;

void ssh_key_mgr_open(int win_w, int win_h);
void ssh_key_mgr_close();

void ssh_key_mgr_render(int win_w, int win_h);

// Returns true if consumed.
bool ssh_key_mgr_keydown(SDL_Keysym ks, const char *text_input);
bool ssh_key_mgr_mousedown(int x, int y, int button);
bool ssh_key_mgr_mousemotion(int x, int y, bool lbutton);
void ssh_key_mgr_scroll(int delta_y);
