// gl_terminal_main.cpp  — entry point only
// Build (local shell):
//   g++ gl_terminal_main.cpp gl_renderer.cpp ft_font.cpp term_color.cpp
//       terminal.cpp term_pty.cpp term_ui.cpp gl_bouncingcircle.cpp
//       font_manager.cpp
//       -lGL -lGLEW -lSDL2 -lfreetype -o gl_terminal
//
// Build (with SSH support):
//   g++ ... ssh_session.cpp ... -lssh2 -lcrypto -lssl -DUSESSH -o gl_terminal

#include "gl_terminal.h"
#include "gl_renderer.h"
#include "ft_font.h"
#include "term_color.h"
#include "terminal.h"
#include "term_pty.h"
#include "term_ui.h"
#include "gl_bouncingcircle.h"
#include "crt_audio.h"
#include "felix_settings.h"
#include "kitty_graphics.h"
#include "font_manager.h"
#ifdef USESSH
#  include "ssh_session.h"
#  include "sftp_overlay.h"
#  include "sftp_console.h"
#endif

#include <SDL2/SDL.h>
#include "icon.h"
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

// ============================================================================
// GLOBALS referenced across modules
// ============================================================================

int         g_font_size   = FONT_SIZE_DEFAULT;
float       g_opacity     = 1.0f;
bool        g_blink_text_on = true;
SDL_Window *g_sdl_window  = nullptr;
std::vector<FontEntry> g_font_list;

// ============================================================================
// SSH / PTY dispatch helpers — used throughout the main loop.
//
// TERM_WRITE always goes through term_write(), which internally checks
// g_term_write_override (set by ssh_connect) to route to SSH when active.
// This means handle_key(), term_paste() etc. also work transparently.
//
// TERM_READ must explicitly pick ssh_read vs term_read since they pull from
// different sources (SSH channel vs pty_fd).
// ============================================================================

#ifdef USESSH
static inline bool _term_read_dispatch(bool ssh, Terminal *t)
    { return ssh ? ssh_read(t) : term_read(t); }
#  define TERM_READ()  _term_read_dispatch(use_ssh, &term)
#else
#  define TERM_READ()  term_read(&term)
#endif
#define TERM_WRITE(buf, n)  term_write(&term, (buf), (n))

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    // ---- Command-line parsing ----
#ifdef WIN32
    const char *shell = "cmd.exe";
#else
    const char *shell = "/bin/bash";
#endif

#ifdef USESSH
    SshConfig ssh_cfg;
#endif
    bool use_ssh = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

#ifdef USESSH
        // --ssh [user@host[:port]]  — argument is optional; missing parts are
        // prompted inside the GL window.
        if ((strcmp(arg, "--ssh") == 0 || strcmp(arg, "-ssh") == 0)) {
            use_ssh = true;
            g_kitty_enabled = false;  // tmux/multiplexers send APC; disable kitty to avoid crashes
            if (i + 1 < argc && argv[i+1][0] != '-') {
                const char *target = argv[++i];
                const char *at = strchr(target, '@');
                if (at) {
                    ssh_cfg.user = std::string(target, at - target);
                    const char *host_start = at + 1;
                    const char *colon = strrchr(host_start, ':');
                    if (colon) {
                        ssh_cfg.host = std::string(host_start, colon - host_start);
                        ssh_cfg.port = atoi(colon + 1);
                        if (ssh_cfg.port <= 0 || ssh_cfg.port > 65535) ssh_cfg.port = 22;
                    } else {
                        ssh_cfg.host = host_start;
                    }
                } else {
                    // No '@' — treat entire token as host, prompt for user
                    ssh_cfg.host = target;
                }
            }
            // Missing user/host will be prompted in the GL window
            continue;
        }
        if ((strcmp(arg, "--ssh-key") == 0 || strcmp(arg, "-ssh-key") == 0 || strcmp(arg, "-i") == 0) && i + 1 < argc) {
            ssh_cfg.key_path = argv[++i];
            continue;
        }
        if ((strcmp(arg, "--ssh-key-pub") == 0 || strcmp(arg, "-ssh-key-pub") == 0) && i + 1 < argc) {
            ssh_cfg.key_path_pub = argv[++i];
            continue;
        }
        if ((strcmp(arg, "--ssh-password") == 0 || strcmp(arg, "-ssh-password") == 0) && i + 1 < argc) {
            ssh_cfg.password = argv[++i];
            continue;
        }
        if ((strcmp(arg, "--ssh-known-hosts") == 0 || strcmp(arg, "-ssh-known-hosts") == 0) && i + 1 < argc) {
            ssh_cfg.known_hosts_path = argv[++i];
            continue;
        }
#endif // USESSH

        // Positional: shell command (local mode only)
        if (arg[0] != '-' && !use_ssh) {
            shell = arg;
            continue;
        }

        // --help / -h
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printf("Usage: flt [shell]\n");
            printf("       flt [options]\n\n");
            printf("Options:\n");
            printf("  [shell]                     Command to run instead of default shell\n");
#ifdef USESSH
            printf("\nSSH options:\n");
            printf("  --ssh [user@host[:port]]    Connect via SSH (prompts for missing fields)\n");
            printf("  -i <path>                   Private key file (alias: --ssh-key)\n");
            printf("  --ssh-key <path>            Private key file for public key auth\n");
            printf("  --ssh-key-pub <path>        Public key file (derived from key path if omitted)\n");
            printf("  --ssh-password <pass>       Password auth (prefer agent or key)\n");
            printf("  --ssh-known-hosts <path>    Known hosts file (default: ~/.ssh/known_hosts)\n");
#endif
            printf("\nKeyboard shortcuts:\n");
            printf("  F2                          SFTP upload browser (SSH sessions only)\n");
            printf("  F3                          SFTP download browser (SSH sessions only)\n");
            printf("  F4                          SFTP interactive console (SSH sessions only)\n");
            printf("  F11                         Toggle full screen\n");
            printf("  Ctrl+Scroll                 Resize font\n");
            printf("  Shift+PageUp/Down           Scroll scrollback buffer\n");
            printf("  Ctrl+Click                  Open URL in browser\n");
            return 0;
        }
    }

    // Disable X11 input method composition (IBus/fcitx intercept Ctrl+Shift+U
    // for Unicode entry before SDL sees it with correct mod state).
    // A terminal handles its own input — we don't want IME interference.
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "0");
    SDL_SetHint("SDL_IM_MODULE", "");       // empty = disable input method module

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);

    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    int win_w = 800, win_h = 480;
    SDL_Window *window = SDL_CreateWindow(
        WIN_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    g_sdl_window = window;

    // Set embedded window icon
    SDL_Surface *icon_surf = SDL_CreateRGBSurfaceFrom(
        (void*)icon_pixels, icon_w, icon_h, 32, icon_w * 4,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (icon_surf) {
        SDL_SetWindowIcon(window, icon_surf);
        SDL_FreeSurface(icon_surf);
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, ctx);
    SDL_GL_SetSwapInterval(1);

    apply_theme(0);
    ft_init();
    crt_audio_init();

    Terminal term;
    term_init(&term);

    settings_load();  // applies font size, theme, sound — after ft_init and term_init
    SDL_Log("[DEBUG] settings_load done\n");

    // Scan system fonts and restore saved font choice
    g_font_list = font_scan();
    {
        std::string saved = font_load_config();
        if (!saved.empty()) {
            for (const auto &fe : g_font_list) {
                if (fe.display_name == saved) {
                    font_apply(fe, g_font_list, nullptr, 0, 0);
                    break;
                }
            }
        }
    }

    win_w = (int)(term.cell_w * term.cols) + 4;
    win_h = (int)(term.cell_h * term.rows) + 4;
    SDL_SetWindowSize(window, win_w, win_h);
    SDL_GetWindowSize(window, &win_w, &win_h);

    gl_init_renderer(win_w, win_h);
    kitty_init();
    glViewport(0, 0, win_w, win_h);

    term_resize(&term, win_w, win_h);

    // ---- SSH: async connection state ----------------------------------------
    // ssh_connect() blocks on network I/O (DNS, TCP, SSH handshake, auth).
    // We run it on a background thread so the GL window stays alive and
    // responsive.  The password prompt is handled via a shared request/response
    // struct: the background thread posts a prompt request, the main loop
    // renders it, collects the password, and posts the response back.
#ifdef USESSH
    enum class SshPhase { IDLE, SETUP, CONNECTING, PROMPTING, ACTIVE, FAILED };
    // Start in SETUP if any required fields are missing, otherwise go straight to CONNECTING
    SshPhase ssh_phase = use_ssh
        ? (ssh_cfg.host.empty() || ssh_cfg.user.empty()
               ? SshPhase::SETUP : SshPhase::CONNECTING)
        : SshPhase::IDLE;

    struct PromptReq {
        std::string prompt;
        std::string response;
        bool        pending  = false;
        bool        answered = false;
        bool        secret   = false;  // true = echo *, false = echo chars
        SDL_mutex  *mtx      = nullptr;
    } prompt_req;

    std::thread   ssh_thread;
    std::string   ssh_field_input;  // text being typed for any in-window prompt
    bool          ssh_conn_ok = false;
    std::atomic<bool> ssh_thread_done{false};
    std::atomic<bool> ssh_abort{false};  // set on quit to unblock prompt callback

    // Helper: post a prompt request from the main thread to itself (SETUP phase).
    // SETUP prompts are driven directly by the main loop — no background thread yet.
    // We reuse prompt_req so the event handler can feed keystrokes into ssh_field_input.
    auto ssh_begin_prompt = [&](const char *text, bool secret) {
        term_feed(&term, text, (int)strlen(text));
        ssh_field_input.clear();
        prompt_req.secret   = secret;
        prompt_req.pending  = true;
        prompt_req.answered = false;
    };

    if (use_ssh) {
        prompt_req.mtx = SDL_CreateMutex();
        SDL_StartTextInput();

        if (ssh_phase == SshPhase::SETUP) {
            term_feed(&term, "SSH connection setup\r\n", 21);
            if (ssh_cfg.host.empty())
                ssh_begin_prompt("\r\nHost: ", false);
            else if (ssh_cfg.user.empty()) {
                char p[128];
                snprintf(p, sizeof(p), "User (%s): ", ssh_cfg.host.c_str());
                ssh_begin_prompt(p, false);
            }
        } else {
            char connecting_msg[256];
            snprintf(connecting_msg, sizeof(connecting_msg),
                     "Connecting to %s@%s:%d …\r\n",
                     ssh_cfg.user.c_str(), ssh_cfg.host.c_str(), ssh_cfg.port);
            term_feed(&term, connecting_msg, (int)strlen(connecting_msg));

            ssh_cfg.prompt_password = [&](const char *prompt_str) -> std::string {
                SDL_LockMutex(prompt_req.mtx);
                prompt_req.prompt   = prompt_str;
                prompt_req.response.clear();
                prompt_req.answered = false;
                prompt_req.pending  = true;
                prompt_req.secret   = true;
                SDL_UnlockMutex(prompt_req.mtx);
                while (true) {
                    if (ssh_abort.load()) return std::string{};
                    SDL_LockMutex(prompt_req.mtx);
                    bool done = prompt_req.answered;
                    std::string resp = prompt_req.response;
                    SDL_UnlockMutex(prompt_req.mtx);
                    if (done) return resp;
                    SDL_Delay(10);
                }
            };

            ssh_thread = std::thread([&]() {
                ssh_conn_ok = ssh_connect(ssh_cfg, &term);
                ssh_thread_done.store(true);
            });
        }
    }
#endif
    // ---- local shell (non-SSH) -----------------------------------------------
#ifdef USESSH
    if (!use_ssh)
#endif
    {
        if (!term_spawn(&term, shell)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to spawn shell: %s\r\n"
                     "Press any key to quit.\r\n", shell);
            term_feed(&term, msg, (int)strlen(msg));
        }
    }

#ifdef _WIN32
    SDL_Delay(300);
    TERM_READ();
#else
#  ifdef USESSH
    if (!use_ssh)
#  endif
    {
        SDL_Delay(200);
        term_read(&term);
        for (int i = 0; i < term.rows * term.cols; i++)
            term.cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
        term.cur_row = term.cur_col = 0;
        term.scroll_top = 0; term.scroll_bot = term.rows - 1;
        term.state = PS_NORMAL;
        term_write(&term, "\n", 1);
        SDL_Delay(100);
        term_read(&term);
    }
#endif

    SDL_Cursor *cursor_ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    SDL_Cursor *cursor_hand  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    SDL_SetCursor(cursor_ibeam);

#ifdef USESSH
    // Get the remote cwd by running 'pwd' on a temporary exec channel.
    // Falls back to "/" on any failure.
    auto guess_remote_cwd = [&]() -> std::string {
        LIBSSH2_SESSION *sess = ssh_get_session();
        if (!sess) return "/";

        libssh2_session_set_blocking(sess, 1);

        LIBSSH2_CHANNEL *ch = nullptr;
        while (!(ch = libssh2_channel_open_session(sess))) {
            if (libssh2_session_last_errno(sess) != LIBSSH2_ERROR_EAGAIN) break;
            SDL_Delay(5);
        }
        if (!ch) {
            libssh2_session_set_blocking(sess, 0);
            SDL_Log("[SFTP] guess_remote_cwd: could not open channel, using /\n");
            return "/";
        }

        int rc;
        while ((rc = libssh2_channel_exec(ch, "pwd")) == LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);

        std::string result;
        if (rc == 0) {
            char buf[4096] = {};
            ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf) - 1);
            if (n > 0) {
                result = buf;
                // Strip trailing newline/whitespace
                while (!result.empty() &&
                       (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
                    result.pop_back();
            }
        }

        libssh2_channel_send_eof(ch);
        libssh2_channel_wait_eof(ch);
        libssh2_channel_close(ch);
        libssh2_channel_wait_closed(ch);
        libssh2_channel_free(ch);
        libssh2_session_set_blocking(sess, 0);

        if (result.empty() || result[0] != '/') {
            SDL_Log("[SFTP] guess_remote_cwd: got '%s', falling back to /\n", result.c_str());
            return "/";
        }
        SDL_Log("[SFTP] remote cwd: %s\n", result.c_str());
        return result;
    };
#endif

    uint32_t last_ticks = SDL_GetTicks();
    bool running = true;
    int  debug_frame = 0;

    // Auto-scroll state for selection drag
    int  autoscroll_mouse_x = 0, autoscroll_mouse_y = 0;
    double autoscroll_accum = 0.0;

    while (running) {
        uint32_t now = SDL_GetTicks();
        double dt = (now - last_ticks) / 1000.0;
        last_ticks = now;
        bool needs_render = false;

        // Text blink (500ms) — mark all rows with ATTR_BLINK cells dirty.
        // Simplest safe approach: mark all dirty; blink is rare anyway.
        term.blink += dt;
        if (term.blink >= 0.5) {
            term.blink = 0;
            g_blink_text_on = !g_blink_text_on;
            term_dirty_all(&term);
            needs_render = true;
        }

        // Cursor blink (600ms) — only the cursor row needs to change
        if (term.cursor_blink_enabled) {
            term.cursor_blink += dt;
            if (term.cursor_blink >= 0.6) {
                term.cursor_blink = 0;
                term.cursor_on = !term.cursor_on;
                term_dirty_row(&term, term.cur_row);
                needs_render = true;
            }
        }

        // Kitty animation frames
        if (kitty_tick(dt))
            needs_render = true;

        // Auto-scroll during selection drag
        if (term.sel_active) {
            int margin = (int)term.cell_h;  // one cell height from edge triggers scroll
            int scroll_dir = 0;
            if (autoscroll_mouse_y < margin)                scroll_dir =  1; // toward top → more scrollback
            else if (autoscroll_mouse_y > win_h - margin)  scroll_dir = -1; // toward bottom → less scrollback
            if (scroll_dir != 0) {
                float past = (scroll_dir > 0)
                    ? (float)(margin - autoscroll_mouse_y) / margin        // above top edge
                    : (float)(autoscroll_mouse_y - (win_h - margin)) / margin; // below bottom edge
                if (past < 0.f) past = 0.f;
                double rate = 4.0 + past * 12.0;  // rows per second
                autoscroll_accum += dt * rate;
                int steps = (int)autoscroll_accum;
                autoscroll_accum -= steps;
                if (steps > 0) {
                    int new_off = SDL_clamp(term.sb_offset + scroll_dir * steps, 0, term.sb_count);
                    if (new_off != term.sb_offset) {
                        term.sb_offset = new_off;
                        // Update selection end to current mouse position
                        pixel_to_cell(&term, autoscroll_mouse_x, autoscroll_mouse_y, 2, 2,
                                      &term.sel_end_row, &term.sel_end_col);
                        term.sel_exists = true;
                        term_dirty_all(&term);
                        needs_render = true;
                    }
                }
            } else {
                autoscroll_accum = 0.0;
            }
        }

        // SSH connection state machine
#ifdef USESSH
        // Overlay: only redraw when progress actually changed or transfer just finished.
        // When idle (no transfer running), a 30ms sleep makes keyboard nav responsive
        // without spinning the CPU.
        if (g_sftp.visible) {
            static float s_last_progress = -1.f;
            static bool  s_last_transferring = false;
            float  cur_prog   = g_sftp.transferring ? sftp_progress() : g_sftp.progress;
            bool   cur_xfer   = g_sftp.transferring;
            bool   xfer_done  = s_last_transferring && !cur_xfer;  // transition: was running, now done
            if (cur_prog != s_last_progress || xfer_done || cur_xfer) {
                needs_render       = true;
                s_last_progress    = cur_prog;
            }
            s_last_transferring = cur_xfer;
            // Throttle overlay to ~33fps: responsive for keyboard nav, smooth for progress bar
            if (!needs_render)
                SDL_Delay(30);
        }
        if (use_ssh && ssh_phase == SshPhase::SETUP) {
            needs_render = true;
        } else if (use_ssh && ssh_phase == SshPhase::CONNECTING) {
            needs_render = true;

            // Check if background thread posted a password prompt request
            SDL_LockMutex(prompt_req.mtx);
            bool has_prompt = prompt_req.pending && !prompt_req.answered;
            std::string prompt_text = has_prompt ? prompt_req.prompt : "";
            SDL_UnlockMutex(prompt_req.mtx);

            if (has_prompt) {
                SDL_Log("[Main] detected pending prompt, switching to PROMPTING\n");
                term_feed(&term, prompt_text.c_str(), (int)prompt_text.size());
                ssh_field_input.clear();
                ssh_phase = SshPhase::PROMPTING;
            } else if (ssh_thread_done.load()) {
                ssh_thread.join();
                if (ssh_conn_ok) {
                    // sftp_init must run on the main thread — it temporarily
                    // sets the session to blocking mode, which races with
                    // ssh_read/ssh_write if done on the background thread.
                    sftp_init();
                    ssh_phase = SshPhase::ACTIVE;
                    SDL_StartTextInput();
                } else {
                    ssh_phase = SshPhase::FAILED;
                    const char *msg = "\r\nConnection failed. Press any key to close.\r\n";
                    term_feed(&term, msg, (int)strlen(msg));
                    SDL_StopTextInput();
                }
                needs_render = true;
            }
        } else if (use_ssh && ssh_phase == SshPhase::PROMPTING) {
            needs_render = true;
        } else if (use_ssh && ssh_phase == SshPhase::FAILED) {
            needs_render = true;
        }
#endif

        // PTY / SSH read.
        // For local PTY: loop up to 8ms to drain burst output without falling behind.
        // For SSH: ssh_read() already loops internally until EAGAIN, so one call
        // per frame is correct — looping again just hammers the keepalive path.
#ifdef USESSH
        bool ssh_ready = !use_ssh || ssh_phase == SshPhase::ACTIVE;
#else
        constexpr bool ssh_ready = true;
#endif
        {
            bool had_sel = term.sel_exists || term.sel_active;
            int old_sb_count = term.sb_count;
            bool got_data = false;
#ifdef USESSH
            if (ssh_ready) {
                got_data = TERM_READ();
            }
#else
            // Local PTY — drain in a tight loop up to 8ms budget
            uint32_t read_start = SDL_GetTicks();
            while (TERM_READ()) {
                got_data = true;
                if (SDL_GetTicks() - read_start >= 8) break;
            }
            (void)ssh_ready;
#endif
            if (got_data) {
                needs_render = true;
                bool new_lines = (term.sb_count != old_sb_count);
                if (new_lines) {
                    term.sb_offset = 0;
                    if (had_sel) { term.sel_exists = false; term.sel_active = false; }
                }
            }
        }

        // Child / channel exit check
#ifdef USESSH
        if (use_ssh && ssh_phase == SshPhase::FAILED) {
            // Already showing error — running stays true until keypress (handled in events)
        } else if (use_ssh) {
            if (ssh_phase == SshPhase::ACTIVE && ssh_channel_closed()) {
                settings_save();
                running = false;
            }
        } else
#endif
        {
#ifdef _WIN32
            if (term_child_exited()) {
                settings_save();
                running = false;
            }
#else
            int status;
            pid_t wp = (term.child > 0) ? waitpid(term.child, &status, WNOHANG) : 0;
            if (wp > 0) {
                settings_save();
                running = false;
            }
#endif
        }

        // Event loop
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            needs_render = true;
            switch (ev.type) {
            case SDL_QUIT: settings_save();
#ifdef USESSH
                ssh_abort.store(true);
#endif
                running = false; break;

            case SDL_KEYDOWN: {
#ifdef USESSH
                // SETUP: collecting host, user (with visible echo)
                // PROMPTING: collecting password (with * echo)
                if (use_ssh && (ssh_phase == SshPhase::SETUP ||
                                ssh_phase == SshPhase::PROMPTING)) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        term_feed(&term, "\r\n", 2);

                        if (ssh_phase == SshPhase::SETUP) {
                            // Store the typed value into the right field
                            if (ssh_cfg.host.empty())
                                ssh_cfg.host = ssh_field_input;
                            else if (ssh_cfg.user.empty())
                                ssh_cfg.user = ssh_field_input;
                            ssh_field_input.clear();

                            // Advance to next missing field, or start connecting
                            if (ssh_cfg.host.empty()) {
                                ssh_begin_prompt("Host: ", false);
                            } else if (ssh_cfg.user.empty()) {
                                char p[128];
                                snprintf(p, sizeof(p), "User (%s): ", ssh_cfg.host.c_str());
                                ssh_begin_prompt(p, false);
                            } else {
                                // All fields collected — start connection thread
                                ssh_phase = SshPhase::CONNECTING;

                                // Clear any SETUP prompt state so the CONNECTING
                                // handler doesn't mistake it for a password request
                                SDL_LockMutex(prompt_req.mtx);
                                prompt_req.pending  = false;
                                prompt_req.answered = false;
                                prompt_req.response.clear();
                                SDL_UnlockMutex(prompt_req.mtx);

                                char connecting_msg[256];
                                snprintf(connecting_msg, sizeof(connecting_msg),
                                         "Connecting to %s@%s:%d …\r\n",
                                         ssh_cfg.user.c_str(), ssh_cfg.host.c_str(), ssh_cfg.port);
                                term_feed(&term, connecting_msg, (int)strlen(connecting_msg));

                                ssh_cfg.prompt_password = [&](const char *prompt_str) -> std::string {
                                    SDL_LockMutex(prompt_req.mtx);
                                    prompt_req.prompt   = prompt_str;
                                    prompt_req.response.clear();
                                    prompt_req.answered = false;
                                    prompt_req.pending  = true;
                                    prompt_req.secret   = true;
                                    SDL_UnlockMutex(prompt_req.mtx);
                                    while (true) {
                                        if (ssh_abort.load()) return std::string{};
                                        SDL_LockMutex(prompt_req.mtx);
                                        bool done = prompt_req.answered;
                                        std::string resp = prompt_req.response;
                                        SDL_UnlockMutex(prompt_req.mtx);
                                        if (done) return resp;
                                        SDL_Delay(10);
                                    }
                                };
                                ssh_thread = std::thread([&]() {
                                    ssh_conn_ok = ssh_connect(ssh_cfg, &term);
                                    ssh_thread_done.store(true);
                                });
                            }
                        } else {
                            // PROMPTING: post password response to waiting thread
                            SDL_LockMutex(prompt_req.mtx);
                            prompt_req.response = ssh_field_input;
                            prompt_req.pending  = false;
                            prompt_req.answered = true;
                            SDL_UnlockMutex(prompt_req.mtx);
                            ssh_field_input.clear();
                            ssh_phase = SshPhase::CONNECTING;
                        }
                    } else if (sym == SDLK_ESCAPE) {
                        if (ssh_phase == SshPhase::PROMPTING) {
                            term_feed(&term, "\r\n", 2);
                            SDL_LockMutex(prompt_req.mtx);
                            prompt_req.response.clear();
                            prompt_req.pending  = false;
                            prompt_req.answered = true;
                            SDL_UnlockMutex(prompt_req.mtx);
                            ssh_field_input.clear();
                            ssh_phase = SshPhase::CONNECTING;
                        }
                        // In SETUP, ignore Escape (keep prompting)
                    } else if (sym == SDLK_BACKSPACE && !ssh_field_input.empty()) {
                        ssh_field_input.pop_back();
                        if (ssh_phase != SshPhase::PROMPTING)
                            term_feed(&term, "\b \b", 3);
                    }
                    break;
                }
                // Any key on failure screen closes the window
                if (use_ssh && ssh_phase == SshPhase::FAILED) {
                    running = false;
                    break;
                }
                // Still connecting — ignore all normal key input
                if (!ssh_ready) break;
#endif
                // SFTP console (F4) — forward all keys when visible
#ifdef USESSH
                if (g_sftp_console_visible) {
                    sftp_console_keydown(ev.key.keysym, nullptr);
                    needs_render = true;
                    break;
                }
#endif
                // SFTP overlay — forward all keys when visible
#ifdef USESSH
                if (g_sftp.visible) {
                    sftp_overlay_keydown(ev.key.keysym.sym);
                    needs_render = true;
                    break;
                }
                // F2 = upload, F3 = download, F4 = SFTP console
                if (use_ssh && ssh_active()) {
                    if (ev.key.keysym.sym == SDLK_F2) {
                        SDL_Log("[SFTP] F2 upload triggered\n");
                        SDL_GetWindowSize(window, &win_w, &win_h);
                        sftp_overlay_open(SftpOverlayMode::UPLOAD,
                                          guess_remote_cwd().c_str(), win_w, win_h);
                        needs_render = true;
                        break;
                    }
                    if (ev.key.keysym.sym == SDLK_F3) {
                        SDL_Log("[SFTP] F3 download triggered\n");
                        SDL_GetWindowSize(window, &win_w, &win_h);
                        sftp_overlay_open(SftpOverlayMode::DOWNLOAD,
                                          guess_remote_cwd().c_str(), win_w, win_h);
                        needs_render = true;
                        break;
                    }
                    if (ev.key.keysym.sym == SDLK_F4) {
                        SDL_Log("[SFTP] F4 console triggered\n");
                        SDL_GetWindowSize(window, &win_w, &win_h);
                        sftp_console_open(win_w, win_h);
                        needs_render = true;
                        break;
                    }
                }
#endif
                if (ev.key.repeat) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym >= SDLK_SPACE && sym < SDLK_DELETE)
                        break;
                }
                if (ev.key.keysym.sym == SDLK_F11) {
                    bool is_full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(window, is_full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }
                if (g_menu.visible) { g_menu.visible = false; break; }
                SDL_Keymod mod = SDL_GetModState();
                int page = term.rows - 1;
                if (ev.key.keysym.sym == SDLK_PAGEUP && (mod & KMOD_SHIFT)) {
                    term.sb_offset = SDL_min(term.sb_offset + page, term.sb_count);
                    term_dirty_all(&term);
                    break;
                }
                if (ev.key.keysym.sym == SDLK_PAGEDOWN && (mod & KMOD_SHIFT)) {
                    term.sb_offset = SDL_max(term.sb_offset - page, 0);
                    term_dirty_all(&term);
                    break;
                }
                term.sb_offset = 0;
                if (mod & KMOD_CTRL) {
                    if (ev.key.keysym.sym == SDLK_c && term.sel_exists) {
                        term_copy_selection(&term); break;
                    }
                    if (ev.key.keysym.sym == SDLK_v) { term_paste(&term); break; }
                    if (ev.key.keysym.sym == SDLK_LSHIFT ||
                        ev.key.keysym.sym == SDLK_RSHIFT) break;
                }
                handle_key(&term, ev.key.keysym, NULL);
                if (TERM_READ()) needs_render = true;
                break;
            }

            case SDL_TEXTINPUT: {
#ifdef USESSH
                // Forward text to SFTP console when visible
                if (g_sftp_console_visible) {
                    SDL_Keysym ks{}; ks.sym = SDLK_UNKNOWN;
                    sftp_console_keydown(ks, ev.text.text);
                    needs_render = true;
                    break;
                }
                // Drop text input entirely when the SFTP overlay is up
                if (g_sftp.visible) break;
                if (use_ssh && ssh_phase == SshPhase::SETUP) {
                    // Visible echo for host/user fields
                    ssh_field_input += ev.text.text;
                    term_feed(&term, ev.text.text, (int)strlen(ev.text.text));
                    break;
                }
                if (use_ssh && ssh_phase == SshPhase::PROMPTING) {
                    // No echo — don't reveal password length
                    ssh_field_input += ev.text.text;
                    break;
                }
                if (!ssh_ready) break;
#endif
                SDL_Keymod mod = SDL_GetModState();
                if (!(mod & KMOD_CTRL) && !(mod & KMOD_ALT)) {
                    TERM_WRITE(ev.text.text, (int)strlen(ev.text.text));
                    if (TERM_READ()) needs_render = true;
                }
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                int r, c;
                pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2, &r, &c);
                if (term.mouse_report && !g_menu.visible) {
                    int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                              (ev.button.button == SDL_BUTTON_MIDDLE) ? 1 :
                              (ev.button.button == SDL_BUTTON_RIGHT)  ? 2 : -1;
                    if (btn >= 0) {
                        char seq[32]; int slen;
                        if (term.mouse_sgr)
                            slen = snprintf(seq,sizeof(seq),"\x1b[<%d;%d;%dM",btn,c+1,r+1);
                        else
                            slen = snprintf(seq,sizeof(seq),"\x1b[M%c%c%c",
                                            (char)(32+btn),(char)(32+c+1),(char)(32+r+1));
                        TERM_WRITE(seq, slen);
                    }
                    break;
                }
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    menu_open(&g_menu, ev.button.x, ev.button.y, win_w, win_h);
                } else if (ev.button.button == SDL_BUTTON_LEFT) {
                    // Ctrl+click opens URL
                    if (!g_menu.visible) {
                        SDL_Keymod mod = SDL_GetModState();
                        if (mod & KMOD_CTRL) {
                            std::string url = url_at_pixel(&term, ev.button.x, ev.button.y, 2, 2);
                            if (!url.empty()) {
                                open_url(url);
                                break;
                            }
                        }
                    }
                    if (g_menu.visible) {
                        int sub_hit = submenu_hit(&g_menu, ev.button.x, ev.button.y);
                        if (sub_hit >= 0) {
                            if (g_menu.sub_open == MENU_ID_THEMES) {
                                apply_theme(sub_hit);
                                settings_save();
                                term_dirty_all(&term);
                            } else if (g_menu.sub_open == MENU_ID_OPACITY) {
                                g_opacity = ((float[]){1.0f,0.85f,0.7f,0.5f,0.3f,0.1f})[sub_hit];
                                SDL_SetWindowOpacity(window, g_opacity);
                                term_dirty_all(&term);
                            } else if (g_menu.sub_open == MENU_ID_RENDER_MODE) {
                                if (sub_hit == RENDER_MODE_NORMAL) {
                                    g_render_mode = 0;
                                } else {
                                    g_render_mode ^= (1u << sub_hit);
                                }
                                term_dirty_all(&term);
                            } else if (g_menu.sub_open == MENU_ID_NEW_TERMINAL) {
                                if      (sub_hit == NEW_TERM_IDX_LOCAL) action_new_terminal();
                                else if (sub_hit == NEW_TERM_IDX_SSH)   action_new_ssh_session();
                            } else if (g_menu.sub_open == MENU_ID_ENTERTAINMENT) {
                                if      (sub_hit == ENT_IDX_FIGHT)    fight_set_enabled(!fight_get_enabled());
                                else if (sub_hit == ENT_IDX_BOUNCING) bc_set_enabled(!bc_get_enabled());
                                else if (sub_hit == ENT_IDX_SOUND)  { term_audio_set_enabled(!term_audio_get_enabled()); settings_save(); }
                                // keep submenu open so user can toggle multiple items
                                break;
                            } else if (g_menu.sub_open == MENU_ID_FONTS) {
                                SDL_GetWindowSize(window, &win_w, &win_h);
                                font_apply(g_font_list[sub_hit], g_font_list, &term, win_w, win_h);
                                font_save_config(g_font_list[sub_hit].display_name);
                                G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
                                settings_save();
                                term_dirty_all(&term);
                            }
                            g_menu.visible = false;
                        } else {
                            int hit = menu_hit(&g_menu, ev.button.x, ev.button.y);
                            bool is_sub_parent = (hit==MENU_ID_THEMES || hit==MENU_ID_OPACITY ||
                                                  hit==MENU_ID_RENDER_MODE || hit==MENU_ID_ENTERTAINMENT ||
                                                  hit==MENU_ID_NEW_TERMINAL || hit==MENU_ID_FONTS);
                            if (!is_sub_parent) g_menu.visible = false;
                            switch (hit) {
                            case MENU_ID_COPY:      term_copy_selection(&term); break;
                            case MENU_ID_COPY_HTML: term_copy_selection_html(&term); break;
                            case MENU_ID_COPY_ANSI: term_copy_selection_ansi(&term); break;
                            case MENU_ID_PASTE:     term_paste(&term); break;
                            case MENU_ID_RESET:
                                for(int row=0;row<term.rows;row++)
                                    for(int col=0;col<term.cols;col++)
                                        CELL(&term,row,col)={' ',TCOLOR_PALETTE(7),TCOLOR_PALETTE(0),0,{0,0,0}};
                                term.cur_row=term.cur_col=0;
                                term.scroll_top=0; term.scroll_bot=term.rows-1;
                                term.state=PS_NORMAL;
#ifdef WIN32
                                TERM_WRITE("cls\r\n",5);
#else
                                TERM_WRITE("reset\n",6);
#endif
                                break;
                            case MENU_ID_SELECT_ALL: term_select_all(&term); break;
                            case MENU_ID_QUIT: settings_save(); running = false; break;
                            default:
                                if (hit < 0) g_menu.visible = false;
                                break;
                            }
                        }
                    } else {
                        autoscroll_mouse_x = ev.button.x;
                        autoscroll_mouse_y = ev.button.y;
                        term.sel_start_row = term.sel_end_row = r;
                        term.sel_start_col = term.sel_end_col = c;
                        term.sel_active = true; term.sel_exists = false;
                        term_dirty_all(&term);
                    }
                } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    if (g_menu.visible) g_menu.visible = false;
                    else term_paste(&term);
                }
                break;
            }

            case SDL_MOUSEMOTION:
                if (g_menu.visible) {
                    int hit = menu_hit(&g_menu, ev.motion.x, ev.motion.y);
                    if (hit >= 0) g_menu.hovered = hit;
                    if (hit == MENU_ID_THEMES || hit == MENU_ID_OPACITY ||
                        hit == MENU_ID_RENDER_MODE || hit == MENU_ID_ENTERTAINMENT ||
                        hit == MENU_ID_NEW_TERMINAL || hit == MENU_ID_FONTS) {
                        if (g_menu.sub_open != hit) {
                            g_menu.sub_open = hit; g_menu.sub_hovered = -1;
                            g_menu.sub_x = g_menu.x + g_menu.width + 2;
                            int item_y = g_menu.y + 4;
                            for (int i=0;i<hit;i++)
                                item_y += MENU_ITEMS[i].separator ? g_menu.sep_h : g_menu.item_h;
                            g_menu.sub_y = item_y;
                            int count = (hit==MENU_ID_THEMES)        ? THEME_COUNT :
                                        (hit==MENU_ID_RENDER_MODE)    ? RENDER_MODE_COUNT :
                                        (hit==MENU_ID_ENTERTAINMENT)  ? ENT_COUNT :
                                        (hit==MENU_ID_NEW_TERMINAL)   ? NEW_TERM_COUNT :
                                        (hit==MENU_ID_FONTS)          ? (int)g_font_list.size() : 6;
                            int sw = g_menu.width + g_font_size*2;
                            int sh = count * g_menu.item_h + 8;
                            SDL_GetWindowSize(window, &win_w, &win_h);
                            if (g_menu.sub_x + sw > win_w) g_menu.sub_x = g_menu.x - sw - 2;
                            if (g_menu.sub_y + sh > win_h) g_menu.sub_y = win_h - sh - 2;
                        }
                    } else if (hit >= 0) {
                        g_menu.sub_open = -1;
                    }
                    g_menu.sub_hovered = submenu_hit(&g_menu, ev.motion.x, ev.motion.y);
                } else if (term.sel_active && (ev.motion.state & SDL_BUTTON_LMASK)) {
                    autoscroll_mouse_x = ev.motion.x;
                    autoscroll_mouse_y = ev.motion.y;
                    pixel_to_cell(&term, ev.motion.x, ev.motion.y, 2, 2,
                                  &term.sel_end_row, &term.sel_end_col);
                    term.sel_exists = true;
                    term_dirty_all(&term);
                } else {
                    // Update URL hover highlight — only redraw the affected row
                    if (url_update_hover(&term, ev.motion.x, ev.motion.y, 2, 2)) {
                        int hrow = (int)((ev.motion.y - 2) / term.cell_h);
                        term_dirty_row(&term, hrow);
                        needs_render = true;
                    }
                    // Show pointer cursor when over a URL (Ctrl = clickable)
                    SDL_Keymod mod = SDL_GetModState();
                    std::string hurl = url_at_pixel(&term, ev.motion.x, ev.motion.y, 2, 2);
                    if (!hurl.empty() && (mod & KMOD_CTRL))
                        SDL_SetCursor(cursor_hand);
                    else
                        SDL_SetCursor(cursor_ibeam);
                }
                break;

            case SDL_MOUSEBUTTONUP: {
                int r, c;
                pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2, &r, &c);
                if (term.mouse_report && !g_menu.visible) {
                    int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                              (ev.button.button == SDL_BUTTON_MIDDLE) ? 1 :
                              (ev.button.button == SDL_BUTTON_RIGHT)  ? 2 : -1;
                    if (btn >= 0) {
                        char seq[32]; int slen;
                        if (term.mouse_sgr)
                            slen = snprintf(seq,sizeof(seq),"\x1b[<%d;%d;%dm",btn,c+1,r+1);
                        else
                            slen = snprintf(seq,sizeof(seq),"\x1b[M%c%c%c",
                                            (char)(32+3),(char)(32+c+1),(char)(32+r+1));
                        TERM_WRITE(seq, slen);
                    }
                } else if (ev.button.button == SDL_BUTTON_LEFT && term.sel_active) {
                    pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2,
                                  &term.sel_end_row, &term.sel_end_col);
                    term.sel_active = false;
                    bool same = (term.sel_start_row == term.sel_end_row &&
                                 term.sel_start_col == term.sel_end_col);
                    term.sel_exists = !same;
                    if (term.sel_exists) term_copy_selection(&term);
                    term_dirty_all(&term);
                }
                break;
            }

            case SDL_MOUSEWHEEL: {
                SDL_Keymod mod = SDL_GetModState();
                if (mod & KMOD_CTRL) {
                    int delta = (ev.wheel.y > 0) ? 1 : -1;
                    if (mod & KMOD_SHIFT) delta *= 4;
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    term_set_font_size(&term, g_font_size + delta, win_w, win_h);
                    G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
                    settings_save();
                } else if (term.mouse_report) {
                    int r2, c2;
                    pixel_to_cell(&term, ev.motion.x, ev.motion.y, 2, 2, &r2, &c2);
                    int btn = (ev.wheel.y > 0) ? 64 : 65;
                    char seq[32]; int slen;
                    if (term.mouse_sgr)
                        slen = snprintf(seq,sizeof(seq),"\x1b[<%d;%d;%dM",btn,c2+1,r2+1);
                    else
                        slen = snprintf(seq,sizeof(seq),"\x1b[M%c%c%c",
                                        (char)(32+btn),(char)(32+c2+1),(char)(32+r2+1));
                    TERM_WRITE(seq, slen);
                } else {
                    int delta = (ev.wheel.y > 0) ? 3 : -3;
                    int new_off = SDL_clamp(term.sb_offset + delta, 0, term.sb_count);
                    if (new_off != term.sb_offset) {
                        term.sb_offset = new_off;
                        term_dirty_all(&term);
                    }
                }
                break;
            }

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    glViewport(0, 0, win_w, win_h);
                    G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
                    gl_resize_fbo(win_w, win_h);
                    term_resize(&term, win_w, win_h);
#ifdef USESSH
                    if (use_ssh) ssh_pty_resize(term.cols, term.rows);
#endif
                }
                break;
            }
        }

        // Animated render modes (CRT flicker, VHS noise) need continuous redraw
        if (g_render_mode & (RENDER_BIT_CRT | RENDER_BIT_VHS | RENDER_BIT_C64 | RENDER_BIT_COMPOSITE | RENDER_BIT_GHOSTING))
            needs_render = true;

        // Notify audio of mode state
        static uint32_t s_prev_render_mode = ~0u;
        if (g_render_mode != s_prev_render_mode) {
            term_audio_set_mode(g_render_mode);
            s_prev_render_mode = g_render_mode;
        }

        // Fight simulation tick every frame
        fight_tick((float)win_w, (float)win_h);
        if (fight_get_enabled()) needs_render = true;

        // Bouncing circle tick every frame
        static uint32_t s_last_bc_ticks = 0;
        uint32_t cur_ticks = SDL_GetTicks();
        float bc_dt = s_last_bc_ticks ? (float)(cur_ticks - s_last_bc_ticks) / 1000.f : 0.016f;
        s_last_bc_ticks = cur_ticks;
        bc_tick((float)win_w, (float)win_h, bc_dt);
        if (bc_get_enabled()) needs_render = true;

        // Hard cap at ~60 fps for animated/fight modes that don't vsync themselves.
        // Sleep is placed AFTER rendering so input→render has no artificial delay.
        // (vsync via SDL_GL_SetSwapInterval(1) already throttles normal frames.)

        if (needs_render) {
            // Only re-render the terminal FBO if at least one row is dirty.
            // term_render itself skips clean rows, so even a full call with
            // only one dirty row is cheap.  Scrollback, selection, and resize
            // all call term_dirty_all() so they always get a full redraw.
            bool any_dirty = term.all_dirty;
            if (!any_dirty) {
                for (int r = 0; r < term.rows && !any_dirty; r++)
                    any_dirty = term.dirty_rows[r] != 0;
            }

            if (any_dirty) {
                float bg_r = THEMES[g_theme_idx].bg_r;
                float bg_g = THEMES[g_theme_idx].bg_g;
                float bg_b = THEMES[g_theme_idx].bg_b;
                // On a full redraw, clear the FBO first so stale content
                // from a previous layout (resize, theme change) doesn't show.
                if (term.all_dirty)
                    gl_clear_term_frame(win_w, win_h, bg_r, bg_g, bg_b);
                gl_begin_term_frame(win_w, win_h, bg_r, bg_g, bg_b);
                term_render(&term, 2, 2);  // clears dirty flags internally
                gl_end_term_frame();
            }

            gl_update_ghost(win_w, win_h);

            // Composite: blit cached terminal into post-process FBO, overlay fight figures
            gl_begin_frame();
            glViewport(0, 0, win_w, win_h);
            if (fight_get_enabled()) {
                fight_render((float)win_w, (float)win_h);
            }
            if (bc_get_enabled()) {
                bc_render((float)win_w, (float)win_h);
            }
            float t_sec = (float)(SDL_GetTicks()) / 1000.0f;
            gl_end_frame(t_sec, win_w, win_h);

            // Menu renders after post-process so it's never distorted
            glViewport(0, 0, win_w, win_h);
            menu_render(&g_menu);
#ifdef USESSH
            sftp_overlay_render(win_w, win_h);
            sftp_console_render(win_w, win_h);
#endif

            SDL_GL_SwapWindow(window);

            // Cap idle/animated frames at ~60 fps after swap to avoid busy-looping.
            // Input frames are already throttled by vsync above; this only bites
            // when needs_render was forced by fight/CRT/bouncing modes.
            uint32_t elapsed = SDL_GetTicks() - now;
            if (elapsed < 16) SDL_Delay(16 - elapsed);
        } else {
            // Nothing to render this iteration — sleep briefly to yield the CPU
            // rather than spinning at full speed waiting for PTY data.
            SDL_Delay(4);
        }
    }

    SDL_FreeCursor(cursor_hand);
    SDL_FreeCursor(cursor_ibeam);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    crt_audio_shutdown();
    kitty_shutdown();
    menu_font_shutdown();
#ifdef USESSH
    if (use_ssh) {
        ssh_abort.store(true);
        if (ssh_thread.joinable()) ssh_thread.join();
        if (prompt_req.mtx) SDL_DestroyMutex(prompt_req.mtx);
        sftp_console_join();
        sftp_transfer_join();
        sftp_shutdown();
        ssh_disconnect();
    }
#endif
    SDL_Quit();
    ft_shutdown();

    return 0;
}
