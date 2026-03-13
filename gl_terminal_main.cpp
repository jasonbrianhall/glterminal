// gl_terminal_main.cpp  — entry point only
// Build: g++ gl_terminal_main.cpp gl_renderer.cpp ft_font.cpp term_color.cpp
//            terminal.cpp term_pty.cpp term_ui.cpp
//            -lGL -lGLEW -lSDL2 -lfreetype -o gl_terminal

#include "gl_terminal.h"
#include "gl_renderer.h"
#include "ft_font.h"
#include "term_color.h"
#include "terminal.h"
#include "term_pty.h"
#include "term_ui.h"

#include <SDL2/SDL.h>
#include <sys/wait.h>

// ============================================================================
// GLOBALS referenced across modules
// ============================================================================

int         g_font_size   = FONT_SIZE_DEFAULT;
float       g_opacity     = 1.0f;
bool        g_blink_text_on = true;
SDL_Window *g_sdl_window  = nullptr;

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    const char *shell = (argc > 1) ? argv[1] : "/bin/bash";

    SDL_Init(SDL_INIT_VIDEO);

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
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, ctx);
    SDL_GL_SetSwapInterval(1);

    apply_theme(0);
    ft_init();

    Terminal term;
    term_init(&term);

    win_w = (int)(term.cell_w * term.cols) + 4;
    win_h = (int)(term.cell_h * term.rows) + 4;
    SDL_SetWindowSize(window, win_w, win_h);
    SDL_GetWindowSize(window, &win_w, &win_h);

    gl_init_renderer(win_w, win_h);
    glViewport(0, 0, win_w, win_h);

    term_resize(&term, win_w, win_h);

    if (!term_spawn(&term, shell)) {
        return 1;
    }

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

    uint32_t last_ticks = SDL_GetTicks();
    bool running = true;

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

        // PTY read
        {
            bool had_sel = term.sel_exists || term.sel_active;
            int old_sb_count = term.sb_count;
            bool got_data = term_read(&term);
            if (got_data) {
                needs_render = true;
                bool new_lines = (term.sb_count != old_sb_count);
                if (new_lines) {
                    term.sb_offset = 0;
                    if (had_sel) { term.sel_exists = false; term.sel_active = false; }
                }
            }
        }

        // Child exit check
        int status;
        if (waitpid(term.child, &status, WNOHANG) == term.child) {
            running = false;
        }

        // Event loop
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            needs_render = true;
            switch (ev.type) {
            case SDL_QUIT: running = false; break;

            case SDL_KEYDOWN: {
                if (ev.key.repeat) {
                    // Block repeat for printable keys — SDL_TEXTINPUT handles those.
                    // Arrow keys, backspace, delete, etc. still need repeat.
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym >= SDLK_SPACE && sym < SDLK_DELETE)
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
                term_read(&term);  // drain echo immediately after writing
                break;
            }

            case SDL_TEXTINPUT: {
                SDL_Keymod mod = SDL_GetModState();
                if (!(mod & KMOD_CTRL) && !(mod & KMOD_ALT)) {
                    term_write(&term, ev.text.text, (int)strlen(ev.text.text));
                    term_read(&term);  // drain echo immediately after writing
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
                        term_write(&term, seq, slen);
                    }
                    break;
                }
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    menu_open(&g_menu, ev.button.x, ev.button.y, win_w, win_h);
                } else if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (g_menu.visible) {
                        int sub_hit = submenu_hit(&g_menu, ev.button.x, ev.button.y);
                        if (sub_hit >= 0) {
                            if (g_menu.sub_open == MENU_ID_THEMES)
                                apply_theme(sub_hit);
                            else if (g_menu.sub_open == MENU_ID_OPACITY) {
                                g_opacity = ((float[]){1.0f,0.85f,0.7f,0.5f,0.3f,0.1f})[sub_hit];
                                SDL_SetWindowOpacity(window, g_opacity);
                            }
                            g_menu.visible = false;
                        } else {
                            int hit = menu_hit(&g_menu, ev.button.x, ev.button.y);
                            bool is_sub_parent = (hit==MENU_ID_THEMES || hit==MENU_ID_OPACITY);
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
                                term_write(&term,"reset\n",6);
                                break;
                            case MENU_ID_QUIT: running = false; break;
                            default:
                                if (hit < 0) g_menu.visible = false;
                                break;
                            }
                        }
                    } else {
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
                    if (hit == MENU_ID_THEMES || hit == MENU_ID_OPACITY) {
                        if (g_menu.sub_open != hit) {
                            g_menu.sub_open = hit; g_menu.sub_hovered = -1;
                            g_menu.sub_x = g_menu.x + g_menu.width + 2;
                            int item_y = g_menu.y + 4;
                            for (int i=0;i<hit;i++)
                                item_y += MENU_ITEMS[i].separator ? g_menu.sep_h : g_menu.item_h;
                            g_menu.sub_y = item_y;
                            int count = (hit==MENU_ID_THEMES)?THEME_COUNT:6;
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
                    pixel_to_cell(&term, ev.motion.x, ev.motion.y, 2, 2,
                                  &term.sel_end_row, &term.sel_end_col);
                    term.sel_exists = true;
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
                        term_write(&term, seq, slen);
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
                    term_write(&term, seq, slen);
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
                    term_resize(&term, win_w, win_h);
                }
                break;
            }
        }

        if (needs_render) {
            glClearColor(
                THEMES[g_theme_idx].bg_r,
                THEMES[g_theme_idx].bg_g,
                THEMES[g_theme_idx].bg_b,
                g_opacity);
            glClear(GL_COLOR_BUFFER_BIT);
            glViewport(0, 0, win_w, win_h);
            term_render(&term, 2, 2);
            menu_render(&g_menu);
            SDL_GL_SwapWindow(window);
        }

        uint32_t elapsed = SDL_GetTicks() - now;
        if (elapsed < 16) SDL_Delay(16 - elapsed);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    ft_shutdown();

    return 0;
}
