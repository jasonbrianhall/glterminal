#pragma once
#include <string>
#include <vector>

// ============================================================================
// FONT MANAGER
// Scans system font directories for monospace TTF/OTF fonts.
// Embedded DejaVu variants are always prepended to the list.
// ============================================================================

struct FontEntry {
    std::string display_name;   // shown in menu, e.g. "JetBrains Mono"
    std::string path;           // absolute path, or "" for embedded
    bool        is_embedded;
};

// Scan system fonts and return list (embedded DejaVu first, then system fonts).
// Only includes fixed-pitch (monospace) TTF/OTF faces via FT_IS_FIXED_WIDTH.
std::vector<FontEntry> font_scan();

// Apply a font by entry. Reloads all four FT_Face variants (regular/bold/
// oblique/bold-oblique) from the same family found in the scan results,
// then resizes the terminal to reflect the new metrics.
// Pass nullptr for term/win_w/win_h to skip terminal resize (e.g. at startup).
struct Terminal;
void font_apply(const FontEntry &entry,
                const std::vector<FontEntry> &all_entries,
                Terminal *term, int win_w, int win_h);

// Persist / restore font choice to ~/.config/FelixTerminal/ (Linux)
// or HKCU\Software\FelixTerminal (Windows).
void font_save_config(const std::string &display_name);
std::string font_load_config();   // returns "" if none saved

// Index of currently active font in the list returned by font_scan().
// Updated by font_apply().
extern int g_font_index;

// The scanned font list — defined in gl_terminal_main.cpp, extern everywhere else.
extern std::vector<FontEntry> g_font_list;
