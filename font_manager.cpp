#include "font_manager.h"
#include "ft_font.h"
#include "terminal.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <SDL2/SDL.h>

#include <algorithm>
#include <map>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#  include <windows.h>
#  include <shlobj.h>   // SHGetFolderPathA
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <pwd.h>
#endif

// ============================================================================
// GLOBALS
// ============================================================================

int g_font_index = 0;   // index into the list returned by font_scan()

// ============================================================================
// HELPERS — file system traversal
// ============================================================================

static bool ends_with_ci(const std::string &s, const char *suffix) {
    size_t sl = strlen(suffix);
    if (s.size() < sl) return false;
    for (size_t i = 0; i < sl; i++) {
        if (tolower((unsigned char)s[s.size() - sl + i]) != tolower((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

static bool is_font_file(const std::string &name) {
    return ends_with_ci(name, ".ttf") || ends_with_ci(name, ".otf");
}

#ifndef _WIN32
// Recursively collect font file paths under dir into out.
static void collect_fonts(const std::string &dir, std::vector<std::string> &out) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = dir + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode))  collect_fonts(full, out);
        else if (S_ISREG(st.st_mode) && is_font_file(ent->d_name))
            out.push_back(full);
    }
    closedir(d);
}
#else
static void collect_fonts(const std::string &dir, std::vector<std::string> &out) {
    WIN32_FIND_DATAA fd;
    std::string pattern = dir + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            collect_fonts(full, out);
        else if (is_font_file(fd.cFileName))
            out.push_back(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#endif

// ============================================================================
// font_scan
// ============================================================================

std::vector<FontEntry> font_scan() {
    std::vector<FontEntry> result;

    FontEntry dejavu;
    dejavu.display_name = "DejaVu Sans Mono (embedded)";
    dejavu.path         = "";
    dejavu.is_embedded  = true;
    result.push_back(dejavu);

    std::vector<std::string> search_dirs;

#ifdef _WIN32
    char win_fonts[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_FONTS, nullptr, 0, win_fonts);
    if (win_fonts[0]) search_dirs.push_back(win_fonts);
    char local_fonts[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_fonts) == S_OK) {
        std::string lf = std::string(local_fonts) + "\\Microsoft\\Windows\\Fonts";
        search_dirs.push_back(lf);
    }
#else
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home) {
        search_dirs.push_back(std::string(home) + "/.local/share/fonts");
        search_dirs.push_back(std::string(home) + "/.fonts");
    }
    search_dirs.push_back("/usr/share/fonts");
    search_dirs.push_back("/usr/local/share/fonts");
    search_dirs.push_back("/usr/share/X11/fonts");
#endif

    std::vector<std::string> paths;
    for (const auto &dir : search_dirs)
        collect_fonts(dir, paths);

    SDL_Log("[FontScan] search dirs: %zu, font files found: %zu", search_dirs.size(), paths.size());
    for (const auto &dir : search_dirs)
        SDL_Log("[FontScan]   dir: %s", dir.c_str());

    FT_Library lib = s_ft_lib;
    bool own_lib = false;
    if (!lib) {
        SDL_Log("[FontScan] WARNING: s_ft_lib is null, creating own FT_Library");
        FT_Init_FreeType(&lib);
        own_lib = true;
    } else {
        SDL_Log("[FontScan] Using existing s_ft_lib=%p", (void*)lib);
    }

    std::sort(paths.begin(), paths.end());

    struct FamilyInfo { std::string regular_path, any_path; };
    std::map<std::string, FamilyInfo> families;
    std::vector<std::string> seen = {"DejaVu Sans Mono"};

    int checked = 0, mono_count = 0;
    for (const auto &p : paths) {
        FT_Face face = nullptr;
        FT_Error err = FT_New_Face(lib, p.c_str(), 0, &face);
        if (err != 0) {
            SDL_Log("[FontScan]   SKIP (FT_New_Face err=%d): %s", err, p.c_str());
            continue;
        }
        checked++;
        bool mono = FT_IS_FIXED_WIDTH(face);
        std::string family = face->family_name ? face->family_name : "";
        bool is_regular = !(face->style_flags & (FT_STYLE_FLAG_BOLD | FT_STYLE_FLAG_ITALIC));
        SDL_Log("[FontScan]   [%s] mono=%d regular=%d  %s %s  -> %s",
                mono ? "MONO" : "    ", mono, is_regular,
                face->family_name ? face->family_name : "?",
                face->style_name  ? face->style_name  : "?",
                p.c_str());
        FT_Done_Face(face);
        if (!mono || family.empty()) continue;
        mono_count++;
        auto &fi = families[family];
        if (fi.any_path.empty()) fi.any_path = p;
        if (is_regular && fi.regular_path.empty()) fi.regular_path = p;
    }

    SDL_Log("[FontScan] checked=%d mono=%d families=%zu", checked, mono_count, families.size());

    std::vector<std::string> family_names;
    for (const auto &kv : families) family_names.push_back(kv.first);
    std::sort(family_names.begin(), family_names.end());

    for (const auto &family : family_names) {
        if (std::find(seen.begin(), seen.end(), family) != seen.end()) {
            SDL_Log("[FontScan]   DEDUP skip: %s", family.c_str());
            continue;
        }
        seen.push_back(family);
        const FamilyInfo &fi = families.at(family);
        std::string best_path = fi.regular_path.empty() ? fi.any_path : fi.regular_path;
        SDL_Log("[FontScan]   ADD entry: '%s' -> %s", family.c_str(), best_path.c_str());
        FontEntry fe;
        fe.display_name = family;
        fe.path         = best_path;
        fe.is_embedded  = false;
        result.push_back(fe);
    }

    SDL_Log("[FontScan] total menu entries: %zu", result.size());
    if (own_lib) FT_Done_FreeType(lib);
    return result;
}

// ============================================================================
// ft_font — internal helpers we need access to
// ============================================================================

// Free and reload one FT_Face from a file path (or set to nullptr on failure).
static bool reload_face(FT_Library lib, const std::string &path,
                        unsigned char **buf, FT_Face *face) {
    if (*face) { FT_Done_Face(*face); *face = nullptr; }
    if (*buf)  { free(*buf); *buf = nullptr; }
    if (path.empty()) return false;

    // Read file into a heap buffer (FreeType needs it alive as long as the face is).
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    *buf = (unsigned char*)malloc((size_t)sz);
    if (!*buf) { fclose(f); return false; }
    fread(*buf, 1, (size_t)sz, f);
    fclose(f);

    if (FT_New_Memory_Face(lib, *buf, (FT_Long)sz, 0, face) != 0) {
        free(*buf); *buf = nullptr; *face = nullptr;
        return false;
    }
    return true;
}

// ============================================================================
// font_apply
// ============================================================================

// These are the four font buffers declared in ft_font.cpp.
// We need to be able to free and replace them; expose them via extern.
extern unsigned char *s_font_buf;       // Bold (primary)
extern unsigned char *s_font_buf_reg;   // Regular
extern unsigned char *s_font_buf_obl;   // Oblique
extern unsigned char *s_font_buf_bobl;  // BoldOblique

// Embedded font reload — delegates back to ft_init() helper logic.
// Declared in ft_font.cpp; we call it to restore embedded fonts.
extern void ft_reload_embedded();

void font_apply(const FontEntry &entry,
                const std::vector<FontEntry> &all_entries,
                Terminal *term, int win_w, int win_h) {
    if (entry.is_embedded) {
        // Restore all four embedded faces
        ft_reload_embedded();
        g_font_index = 0;
        if (term) term_resize(term, win_w, win_h);
        return;
    }

    // Find the family name from the chosen entry's file
    FT_Library lib = s_ft_lib;
    FT_Face probe = nullptr;
    if (FT_New_Face(lib, entry.path.c_str(), 0, &probe) != 0) return;
    std::string family = probe->family_name ? probe->family_name : "";
    FT_Done_Face(probe);
    if (family.empty()) return;

    // Collect all entries in the same family from the full list
    struct StyleMatch { std::string path; bool bold, italic; };
    std::vector<StyleMatch> matches;

    for (const auto &fe : all_entries) {
        if (fe.is_embedded || fe.path.empty()) continue;
        FT_Face f = nullptr;
        if (FT_New_Face(lib, fe.path.c_str(), 0, &f) != 0) continue;
        bool same_family = f->family_name && (strcmp(f->family_name, family.c_str()) == 0);
        if (same_family) {
            StyleMatch m;
            m.path   = fe.path;
            m.bold   = (f->style_flags & FT_STYLE_FLAG_BOLD)   != 0;
            m.italic = (f->style_flags & FT_STYLE_FLAG_ITALIC)  != 0;
            matches.push_back(m);
        }
        FT_Done_Face(f);
    }

    // Also probe face_index > 0 in the chosen file (some fonts pack all variants)
    FT_Long num_faces = 0;
    {
        FT_Face tmp = nullptr;
        if (FT_New_Face(lib, entry.path.c_str(), -1, &tmp) == 0) {
            num_faces = tmp->num_faces;
            FT_Done_Face(tmp);
        }
    }
    for (FT_Long fi = 1; fi < num_faces; fi++) {
        FT_Face f = nullptr;
        if (FT_New_Face(lib, entry.path.c_str(), fi, &f) != 0) continue;
        bool same_family = f->family_name && (strcmp(f->family_name, family.c_str()) == 0);
        if (same_family) {
            StyleMatch m;
            m.path   = entry.path;
            m.bold   = (f->style_flags & FT_STYLE_FLAG_BOLD)   != 0;
            m.italic = (f->style_flags & FT_STYLE_FLAG_ITALIC)  != 0;
            matches.push_back(m);
        }
        FT_Done_Face(f);
    }

    // Find best path for each variant (regular, bold, oblique, bold-oblique)
    auto best_path = [&](bool want_bold, bool want_italic) -> std::string {
        // Exact match first
        for (const auto &m : matches)
            if (m.bold == want_bold && m.italic == want_italic) return m.path;
        // Fallback: any match, then chosen entry
        if (!matches.empty()) return matches[0].path;
        return entry.path;
    };

    std::string path_reg  = best_path(false, false);
    std::string path_bold = best_path(true,  false);
    std::string path_obl  = best_path(false, true);
    std::string path_bobl = best_path(true,  true);

    // Reload the four faces
    reload_face(lib, path_bold, &s_font_buf,      &s_ft_face);       // primary = bold
    reload_face(lib, path_reg,  &s_font_buf_reg,  &s_ft_face_reg);
    reload_face(lib, path_obl,  &s_font_buf_obl,  &s_ft_face_obl);
    reload_face(lib, path_bobl, &s_font_buf_bobl, &s_ft_face_bobl);

    // Update active index
    for (int i = 0; i < (int)all_entries.size(); i++) {
        if (all_entries[i].display_name == entry.display_name) {
            g_font_index = i;
            break;
        }
    }

    if (term) term_resize(term, win_w, win_h);
}

// ============================================================================
// CONFIG PERSISTENCE
// ============================================================================

#ifndef _WIN32
static std::string config_path() {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return "";
    return std::string(home) + "/.config/FelixTerminal/font.conf";
}

void font_save_config(const std::string &display_name) {
    std::string path = config_path();
    if (path.empty()) return;
    // Ensure directory exists
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "%s\n", display_name.c_str());
    fclose(f);
}

std::string font_load_config() {
    std::string path = config_path();
    if (path.empty()) return "";
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[256] = {};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    // Strip trailing newline
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return buf;
}

#else   // Windows — registry

static const char *REG_KEY  = "Software\\FelixTerminal";
static const char *REG_VAL  = "FontName";

void font_save_config(const std::string &display_name) {
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExA(hk, REG_VAL, 0, REG_SZ,
                   (const BYTE*)display_name.c_str(),
                   (DWORD)(display_name.size() + 1));
    RegCloseKey(hk);
}

std::string font_load_config() {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return "";
    char buf[256] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    RegQueryValueExA(hk, REG_VAL, nullptr, &type, (BYTE*)buf, &sz);
    RegCloseKey(hk);
    return (type == REG_SZ) ? std::string(buf) : std::string();
}
#endif
