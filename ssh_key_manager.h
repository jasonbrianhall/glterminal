#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>

// ============================================================================
// SSH KEY MANAGER OVERLAY  (F8)
// ============================================================================
// Lists ~/.ssh keys, shows fingerprint/type/comment, lets the user generate
// new Ed25519 / RSA-4096 keys, copy the public key to clipboard, and delete
// key pairs — all without leaving the terminal.
//
// No libssh2 dependency; uses ssh-keygen via popen().

struct SshKeyEntry {
    std::string priv_path;   // e.g. /home/user/.ssh/id_ed25519
    std::string pub_path;    // priv_path + ".pub"
    std::string type;        // "ED25519", "RSA", "ECDSA", …
    std::string comment;     // last token of pub key
    std::string fingerprint; // SHA256:…
    int         bits = 0;
};

enum class KeyMgrPane { LIST, GENERATE, CONFIRM_DELETE };
enum class KeySortCol  { NONE, FINGERPRINT, COMMENT };
enum class KeySortDir  { ASC, DESC };

struct SshKeyMgr {
    bool    visible    = false;
    KeyMgrPane pane    = KeyMgrPane::LIST;

    std::vector<SshKeyEntry> keys;
    int  selected      = 0;   // index in keys
    int  scroll_top    = 0;

    // Column sorting
    KeySortCol sort_col = KeySortCol::NONE;
    KeySortDir sort_dir = KeySortDir::ASC;

    // Generate pane
    char  gen_name[128]    = "id_ed25519";   // filename (no path)
    char  gen_comment[256] = {};             // -C comment
    char  gen_passphrase[256] = {};          // may be empty
    int   gen_type     = 0;   // 0=Ed25519, 1=RSA-4096
    char  status[512]  = {};  // feedback line
    bool  status_ok    = true;

    // Which text field is focused in generate pane (0-3)
    int   gen_focus    = 0;
    bool  gen_pass_show = false;

    int visible_rows = 0;   // computed each frame
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
