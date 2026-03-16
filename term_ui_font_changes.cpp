// ============================================================================
// CHANGES TO term_ui.h
// ============================================================================

// Add after the existing MENU_ID_* defines:
#define MENU_ID_FONTS        17   // pick a value after your current last item

// Add to the end of the includes / externs section:
#include "font_manager.h"
extern std::vector<FontEntry> g_font_list;


// ============================================================================
// CHANGES TO term_ui.cpp
// ============================================================================

// --- 1. Add to includes at the top ---
#include "font_manager.h"

// --- 2. Add global font list (near the other statics) ---
std::vector<FontEntry> g_font_list;   // populated once at startup


// --- 3. Add "Font  >" to MENU_ITEMS[] ---
// Insert before the final separator + Quit:
    { "Font  >",         false },   // MENU_ID_FONTS
    { nullptr,           true  },
    { "Quit",            false },


// --- 4. In submenu_hit(), extend the count calculation ---
// BEFORE:
//     int count = (m->sub_open == MENU_ID_THEMES)       ? THEME_COUNT :
//                 (m->sub_open == MENU_ID_RENDER_MODE)   ? RENDER_MODE_COUNT :
//                 (m->sub_open == MENU_ID_ENTERTAINMENT) ? ENT_COUNT :
//                                                          OPACITY_COUNT;
// AFTER:
    int count = (m->sub_open == MENU_ID_THEMES)       ? THEME_COUNT :
                (m->sub_open == MENU_ID_RENDER_MODE)   ? RENDER_MODE_COUNT :
                (m->sub_open == MENU_ID_ENTERTAINMENT) ? ENT_COUNT :
                (m->sub_open == MENU_ID_FONTS)         ? (int)g_font_list.size() :
                                                         OPACITY_COUNT;


// --- 5. In menu_render(), extend the sub_open condition ---
// BEFORE:
//     if (m->sub_open == MENU_ID_THEMES || m->sub_open == MENU_ID_OPACITY ||
//         m->sub_open == MENU_ID_RENDER_MODE || m->sub_open == MENU_ID_ENTERTAINMENT) {
// AFTER:
    if (m->sub_open == MENU_ID_THEMES    || m->sub_open == MENU_ID_OPACITY     ||
        m->sub_open == MENU_ID_RENDER_MODE || m->sub_open == MENU_ID_ENTERTAINMENT ||
        m->sub_open == MENU_ID_FONTS) {

// And extend the count / label selection inside that block:
// BEFORE:
//         int count = (m->sub_open == MENU_ID_THEMES)       ? THEME_COUNT :
//                     (m->sub_open == MENU_ID_RENDER_MODE)   ? RENDER_MODE_COUNT :
//                     (m->sub_open == MENU_ID_ENTERTAINMENT) ? ENT_COUNT :
//                                                              OPACITY_COUNT;
// AFTER:
        int count = (m->sub_open == MENU_ID_THEMES)       ? THEME_COUNT :
                    (m->sub_open == MENU_ID_RENDER_MODE)   ? RENDER_MODE_COUNT :
                    (m->sub_open == MENU_ID_ENTERTAINMENT) ? ENT_COUNT :
                    (m->sub_open == MENU_ID_FONTS)         ? (int)g_font_list.size() :
                                                             OPACITY_COUNT;

// And extend the label selection:
// BEFORE:
//             const char *lbl = (m->sub_open == MENU_ID_THEMES)       ? THEMES[j].name :
//                               (m->sub_open == MENU_ID_RENDER_MODE)   ? RENDER_MODE_NAMES[j] :
//                               (m->sub_open == MENU_ID_ENTERTAINMENT) ? ENT_NAMES[j] :
//                                                                        OPACITY_NAMES[j];
// AFTER:
            const char *lbl = (m->sub_open == MENU_ID_THEMES)       ? THEMES[j].name :
                              (m->sub_open == MENU_ID_RENDER_MODE)   ? RENDER_MODE_NAMES[j] :
                              (m->sub_open == MENU_ID_ENTERTAINMENT) ? ENT_NAMES[j] :
                              (m->sub_open == MENU_ID_FONTS)         ? g_font_list[j].display_name.c_str() :
                                                                       OPACITY_NAMES[j];

// And extend the active check:
// BEFORE:
//             if (m->sub_open == MENU_ID_THEMES)
//                 active = (j == g_theme_idx);
//             else if ...
// AFTER:
            if (m->sub_open == MENU_ID_THEMES)
                active = (j == g_theme_idx);
            else if (m->sub_open == MENU_ID_FONTS)
                active = (j == g_font_index);
            else if (m->sub_open == MENU_ID_RENDER_MODE)
                ...


// ============================================================================
// CHANGES TO gl_terminal_main.cpp
// ============================================================================

// --- 1. Add include ---
#include "font_manager.h"

// --- 2. After settings_load(), scan fonts and restore saved font ---
    g_font_list = font_scan();
    {
        std::string saved = font_load_config();
        if (!saved.empty()) {
            for (const auto &fe : g_font_list) {
                if (fe.display_name == saved) {
                    font_apply(fe, g_font_list, &term, win_w, win_h);
                    break;
                }
            }
        }
    }

// --- 3. In the submenu hit handler, add the MENU_ID_FONTS case ---
// Find the block that handles sub_hit for themes/opacity/render/entertainment
// and add:
                            } else if (g_menu.sub_open == MENU_ID_FONTS) {
                                font_apply(g_font_list[sub_hit], g_font_list, &term, win_w, win_h);
                                font_save_config(g_font_list[sub_hit].display_name);
                                settings_save();
                                needs_render = true;
                            }

// --- 4. Add font_manager.cpp to the build line at the top of gl_terminal_main.cpp ---
// Build: g++ gl_terminal_main.cpp gl_renderer.cpp ft_font.cpp term_color.cpp
//            terminal.cpp term_pty.cpp term_ui.cpp gl_bouncingcircle.cpp
//            font_manager.cpp                          <-- add this
//            -lGL -lGLEW -lSDL2 -lfreetype -o gl_terminal
