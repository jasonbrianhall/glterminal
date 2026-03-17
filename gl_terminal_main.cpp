// gl_terminal_main.cpp  — entry point only
// Build (local shell):
//   g++ gl_terminal_main.cpp gl_renderer.cpp ft_font.cpp term_color.cpp
//       terminal.cpp term_pty.cpp term_ui.cpp gl_bouncingcircle.cpp
//       font_manager.cpp
//       -lGL -lGLEW -lSDL2 -lfreetype -o gl_terminal
//
// Build (with SSH support):
//   g++ ... ssh_session.cpp ... -lssh2 -lcrypto -lssl -DUSESSL -o gl_terminal
//
// SSH usage:
//   gl_terminal --ssh user@host[:port] [--ssh-key ~/.ssh/id_ed25519]
//               [--ssh-password secret] [--ssh-known-hosts ~/.ssh/known_hosts]

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
#ifdef USESSL
#  include "ssh_session.h"
#endif

#include <SDL2/SDL.h>
#include "icon.h"
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <iostream>
#include <thread>
#include <chrono>

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

#ifdef USESSL
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

#ifdef USESSL
    SshConfig ssh_cfg;
    bool      use_ssh = false;
#endif

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

#ifdef USESSL
        // --ssh user@host  or  --ssh user@host:port
        if (strcmp(arg, "--ssh") == 0 && i + 1 < argc) {
            const char *target = argv[++i];
            // Parse user@host[:port]
            const char *at = strchr(target, '@');
            if (!at) {
                SDL_Log("[SSH] --ssh requires user@host[:port]\n");
                return 1;
            }
            ssh_cfg.user = std::string(target, at - target);
            const char *host_start = at + 1;
            const char *colon = strrchr(host_start, ':');
            if (colon) {
                ssh_cfg.host = std::string(host_start, colon - host_start);
                ssh_cfg.port = atoi(colon + 1);
                if (ssh_cfg.port <= 0 || ssh_cfg.port > 65535) ssh_cfg.port = 22;
            } else {
                ssh_cfg.host = host_start;
                ssh_cfg.port = 22;
            }
            use_ssh = true;
            continue;
        }
        // --ssh-key /path/to/private_key
        if (strcmp(arg, "--ssh-key") == 0 && i + 1 < argc) {
            ssh_cfg.key_path = argv[++i];
            continue;
        }
        // --ssh-key-pub /path/to/public_key  (optional; defaults to key_path + ".pub")
        if (strcmp(arg, "--ssh-key-pub") == 0 && i + 1 < argc) {
            ssh_cfg.key_path_pub = argv[++i];
            continue;
        }
        // --ssh-password secret
        if (strcmp(arg, "--ssh-password") == 0 && i + 1 < argc) {
            ssh_cfg.password = argv[++i];
            continue;
        }
        // --ssh-known-hosts /path/to/known_hosts  ("" = skip verification)
        if (strcmp(arg, "--ssh-known-hosts") == 0 && i + 1 < argc) {
            ssh_cfg.known_hosts_path = argv[++i];
            continue;
        }
#endif // USESSL

        // Positional: shell command (local mode)
        if (arg[0] != '-') {
            shell = arg;
            continue;
        }

        // --help / -h
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printf("Usage: gl_terminal [shell]\n");
#ifdef USESSL
            printf("       gl_terminal --ssh user@host[:port]\n");
            printf("                   [--ssh-key path_to_private_key]\n");
            printf("                   [--ssh-key-pub path_to_public_key]\n");
            printf("                   [--ssh-password password]\n");
            printf("                   [--ssh-known-hosts path_to_known_hosts]\n");
#endif
            return 0;
        }
    }

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

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

    // Connect to local shell or remote SSH session
#ifdef USESSL
    if (use_ssh) {
        if (!ssh_connect(ssh_cfg, &term)) {
            SDL_Log("[SSH] connection failed\n");
            return 1;
        }
    } else
#endif
    if (!term_spawn(&term, shell)) {
        return 1;
    }

#ifdef _WIN32
    SDL_Delay(300);
    TERM_READ();
#else
    SDL_Delay(200);
    TERM_READ();
    for (int i = 0; i < term.rows * term.cols; i++)
        term.cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    term.cur_row = term.cur_col = 0;
    term.scroll_top = 0; term.scroll_bot = term.rows - 1;
    term.state = PS_NORMAL;
    TERM_WRITE("\n", 1);
    SDL_Delay(100);
    TERM_READ();
#endif

    SDL_Cursor *cursor_ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    SDL_Cursor *cursor_hand  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    SDL_SetCursor(cursor_ibeam);

    uint32_t last_ticks = SDL_GetTicks();
    bool running = true;

    // Auto-scroll state for selection drag
    int  autoscroll_mouse_x = 0, autoscroll_mouse_y = 0;
    double autoscroll_accum = 0.0;

    while (running) {
        uint32_t now = SDL_GetTicks();
        double dt = (now - last_ticks) / 1000.0;
        last_ticks = now;
        bool needs_render = false;

        // Text blink (500ms)
        term.blink += dt;
        if (term.blink >= 0.5) {
            term.blink = 0;
            g_blink_text_on = !g_blink_text_on;
            needs_render = true;
        }

        // Cursor blink (600ms)
        if (term.cursor_blink_enabled) {
            term.cursor_blink += dt;
            if (term.cursor_blink >= 0.6) {
                term.cursor_blink = 0;
                term.cursor_on = !term.cursor_on;
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
                        needs_render = true;
                    }
                }
            } else {
                autoscroll_accum = 0.0;
            }
        }

        // PTY / SSH read
        {
            bool had_sel = term.sel_exists || term.sel_active;
            int old_sb_count = term.sb_count;
            bool got_data = TERM_READ();
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
#ifdef USESSL
        if (use_ssh) {
            if (ssh_channel_closed()) {
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
            if (waitpid(term.child, &status, WNOHANG) == term.child) {
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
            case SDL_QUIT: settings_save(); running = false; break;

            case SDL_KEYDOWN: {
                if (ev.key.repeat) {
                    // Block repeat for printable keys — SDL_TEXTINPUT handles those.
                    // Arrow keys, backspace, delete, etc. still need repeat.
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
                    break;
                }
                if (ev.key.keysym.sym == SDLK_PAGEDOWN && (mod & KMOD_SHIFT)) {
                    term.sb_offset = SDL_max(term.sb_offset - page, 0);
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
                SDL_Delay(1);  // give pty/ssh a tick to echo
                if (TERM_READ()) needs_render = true;
                break;
            }

            case SDL_TEXTINPUT: {
                SDL_Keymod mod = SDL_GetModState();
                if (!(mod & KMOD_CTRL) && !(mod & KMOD_ALT)) {
                    TERM_WRITE(ev.text.text, (int)strlen(ev.text.text));
                    SDL_Delay(1);  // give pty/ssh a tick to echo
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
                            } else if (g_menu.sub_open == MENU_ID_OPACITY) {
                                g_opacity = ((float[]){1.0f,0.85f,0.7f,0.5f,0.3f,0.1f})[sub_hit];
                                SDL_SetWindowOpacity(window, g_opacity);
                            } else if (g_menu.sub_open == MENU_ID_RENDER_MODE) {
                                if (sub_hit == RENDER_MODE_NORMAL) {
                                    g_render_mode = 0;  // "Normal" clears all modes
                                } else {
                                    g_render_mode ^= (1u << sub_hit);  // toggle the bit
                                }
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
                            }
                            g_menu.visible = false;
                        } else {
                            int hit = menu_hit(&g_menu, ev.button.x, ev.button.y);
                            bool is_sub_parent = (hit==MENU_ID_THEMES || hit==MENU_ID_OPACITY ||
                                                  hit==MENU_ID_RENDER_MODE || hit==MENU_ID_ENTERTAINMENT ||
                                                  hit==MENU_ID_FONTS);
                            if (!is_sub_parent) g_menu.visible = false;
                            switch (hit) {
                            case MENU_ID_NEW_TERMINAL: action_new_terminal(); break;
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
                        hit == MENU_ID_FONTS) {
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
                } else {
                    // Update URL hover highlight — only redraw if hover state changed
                    needs_render = url_update_hover(&term, ev.motion.x, ev.motion.y, 2, 2);
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
                    term.sb_offset = new_off;
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
#ifdef USESSL
                    if (use_ssh) ssh_pty_resize(term.cols, term.rows);
#endif
                }
                break;
            }
        }

        // Snapshot terminal dirtiness BEFORE fight mode and animated render
        // modes force needs_render — those don't require re-walking cell data.
        bool term_needs_render = needs_render;

        // Animated render modes (CRT flicker, VHS noise) need continuous redraw
        if (g_render_mode & (RENDER_BIT_CRT | RENDER_BIT_VHS | RENDER_BIT_C64 | RENDER_BIT_COMPOSITE))
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
            // s_term_dirty tracks whether terminal cell content has changed.
            // Animation-only frames (fight mode, CRT/VHS flicker) reuse the
            // cached terminal FBO and skip the expensive term_render call.
            static bool s_term_dirty = true;
            if (term_needs_render) s_term_dirty = true;

            if (s_term_dirty) {
                s_term_dirty = false;
                gl_begin_term_frame(win_w, win_h, THEMES[g_theme_idx].bg_r, THEMES[g_theme_idx].bg_g, THEMES[g_theme_idx].bg_b);
                term_render(&term, 2, 2);
                gl_end_term_frame();
            }

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
#ifdef USESSL
    if (use_ssh) ssh_disconnect();
#endif
    SDL_Quit();
    ft_shutdown();

    return 0;
}
