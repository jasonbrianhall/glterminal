// =============================================================================
// wopr_willy.cpp  —  Willy the Worm embedded inside the WOPR overlay
//
// Ported from the GTK/Cairo version in willy.cpp / willy.h.
// Renders entirely through gl_draw_rect() — each pixel of a willy.chr
// 8x8 bitmap sprite becomes one scaled rectangle, matching the look of
// the original game exactly.
//
// Both levels.json and willy.chr are compiled in as zlib-compressed blobs
// via wopr_willy_assets.h.  Disk fallback is used if the header is absent.
//
// To regenerate the header (run from the directory containing the assets):
//   python3 gen_willy_assets.py levels.json willy.chr wopr_willy_assets.h
//
// Build: link with -lminiz  (or add miniz.c to your sources).
// No GTK / Cairo / SDL_mixer dependency.
// =============================================================================

#include "wopr.h"
#include "wopr_render.h"
#include <SDL2/SDL.h>
#include "../miniz.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// Compiled-in compressed assets.
// Falls back gracefully to disk if the header hasn't been generated yet.
#if __has_include("wopr_willy_assets.h")
#  include "wopr_willy_assets.h"
#  define WILLY_ASSETS_EMBEDDED 1
#else
#  define WILLY_ASSETS_EMBEDDED 0
static const unsigned char willy_levels_z[]  = {0};
static const size_t        willy_levels_z_len   = 0;
static const size_t        willy_levels_raw_len  = 0;
static const unsigned char willy_chr_z[]     = {0};
static const size_t        willy_chr_z_len      = 0;
static const size_t        willy_chr_raw_len     = 0;
#endif

// =============================================================================
// CHR SPRITE TABLE
// 128 sprites x 8 bytes each.  Byte 0 = top row, bit 7 = leftmost pixel.
// Index mapping (mirrors spriteloader.cpp):
//   0  = WILLY_RIGHT   1  = WILLY_LEFT    2  = PRESENT
//   3  = LADDER        4  = TACK          5  = UPSPRING
//   6  = SIDESPRING    7  = BALL          8  = BELL
//   51-90 = PIPE1-PIPE40    126 = BALLPIT   127 = EMPTY
// =============================================================================

static uint8_t s_chr[128][8];
static bool    s_chr_loaded = false;

static bool ww_load_chr_from_memory(const void *data, size_t len) {
    if (len < 128 * 8) return false;
    const uint8_t *p = static_cast<const uint8_t*>(data);
    for (int sp = 0; sp < 128; sp++)
        for (int r = 0; r < 8; r++)
            s_chr[sp][r] = p[sp * 8 + r];
    s_chr_loaded = true;
    return true;
}

static bool ww_load_chr_from_disk() {
    const char *paths[] = {
        "willy.chr", "data/willy.chr",
        "/usr/games/willytheworm/data/willy.chr", nullptr
    };
    for (int i = 0; paths[i]; i++) {
        std::ifstream f(paths[i], std::ios::binary);
        if (!f.good()) continue;
        f.seekg(0, std::ios::end);
        size_t sz = (size_t)f.tellg();
        f.seekg(0);
        if (sz < 128 * 8) continue;
        std::vector<uint8_t> buf(sz);
        f.read(reinterpret_cast<char*>(buf.data()), sz);
        return ww_load_chr_from_memory(buf.data(), sz);
    }
    return false;
}

static bool ww_load_chr() {
    // Try compressed header first
    if (WILLY_ASSETS_EMBEDDED && willy_chr_z_len > 1) {
        std::vector<uint8_t> raw(willy_chr_raw_len);
        mz_ulong out_len = (mz_ulong)willy_chr_raw_len;
        if (mz_uncompress(raw.data(), &out_len,
                          willy_chr_z, (mz_ulong)willy_chr_z_len) == MZ_OK)
            if (ww_load_chr_from_memory(raw.data(), out_len))
                return true;
    }
    return ww_load_chr_from_disk();
}

// Sprite index for a tile name.  Returns -1 for EMPTY / unknown.
static int ww_sprite_index(const std::string &tile) {
    if (tile == "WILLY_RIGHT")  return 0;
    if (tile == "WILLY_LEFT")   return 1;
    if (tile == "PRESENT")      return 2;
    if (tile == "LADDER")       return 3;
    if (tile == "TACK")         return 4;
    if (tile == "UPSPRING")     return 5;
    if (tile == "SIDESPRING")   return 6;
    if (tile == "BALL")         return 7;
    if (tile == "BELL")         return 8;
    if (tile == "BALLPIT")      return 126;
    if (tile == "EMPTY")        return -1;
    if (tile.size() > 4 && tile.substr(0, 4) == "PIPE") {
        try {
            int n = std::stoi(tile.substr(4));
            if (n >= 1 && n <= 40) return 50 + n;
        } catch (...) {}
    }
    return -1;
}

// Draw one 8x8 sprite scaled to cell_w x cell_h using gl_draw_rect.
// Each set bit → one rectangle; clear bits are transparent.
static void ww_draw_sprite(int sprite_idx, float px, float py,
                           float cell_w, float cell_h,
                           float fr, float fg, float fb)
{
    if (!s_chr_loaded || sprite_idx < 0 || sprite_idx >= 128) return;
    float pw = cell_w / 8.f;
    float ph = cell_h / 8.f;
    for (int row = 0; row < 8; row++) {
        uint8_t byte = s_chr[sprite_idx][row];
        if (!byte) continue;
        for (int col = 0; col < 8; col++) {
            if ((byte >> (7 - col)) & 1)
                gl_draw_rect(px + col * pw, py + row * ph,
                             pw, ph, fr, fg, fb, 1.0f);
        }
    }
}

// Foreground colour per tile type (background is always the game blue).
static void ww_tile_fg(const std::string &tile,
                       float &fr, float &fg, float &fb)
{
    fr = 1.f; fg = 1.f; fb = 1.f;                              // default: white
    if (tile == "PRESENT")    { fr=1.f;  fg=0.f;  fb=1.f;  }  // magenta
    if (tile == "BELL")       { fr=1.f;  fg=1.f;  fb=0.f;  }  // yellow
    if (tile == "UPSPRING")   { fr=0.f;  fg=1.f;  fb=0.f;  }  // green
    if (tile == "SIDESPRING") { fr=0.f;  fg=1.f;  fb=1.f;  }  // cyan
    if (tile == "TACK")       { fr=0.8f; fg=0.8f; fb=0.8f; }  // grey
    if (tile == "BALL")       { fr=1.f;  fg=0.2f; fb=0.2f; }  // red
    if (tile == "BALLPIT")    { fr=0.5f; fg=0.5f; fb=0.5f; }  // dark grey
}

// =============================================================================
// LEVEL LOADER  (mirrors loadlevels.cpp, no GTK)
// =============================================================================

static const int W_COLS    = 40;
static const int W_ROWS    = 25;
static const int W_MAXROWS = 26;
static const int W_NEWLIFE = 2000;

struct WillyLevel {
    std::string grid[W_MAXROWS][W_COLS];
    std::string grid_orig[W_MAXROWS][W_COLS];
    std::pair<int,int> willy_start   = {23, 7};
    std::pair<int,int> ballpit_pos   = {0, 0};
    bool               ballpit_found = false;
};

struct WillyLevels {
    std::map<std::string, WillyLevel> levels;

    // -------------------------------------------------------------------------
    // The actual JSON format (from levels.json) is:
    //
    //   {
    //     "levelN": {
    //       "ROW": { "COL": "TILE", ... },
    //       ...
    //     },
    //     "levelNPIT": { "PRIMARYBALLPIT": [row, col] },
    //     ...
    //   }
    //
    // Row and column keys are plain integer strings ("0", "1", ...).
    // WILLY_LEFT / WILLY_RIGHT tiles mark the start position in the grid.
    // We parse line-by-line tracking indent depth to know which context we're in.
    // -------------------------------------------------------------------------

    static std::string trim(const std::string &s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Extract the string value from  "key": "VALUE"  or  "key": VALUE
    static std::string extract_string_value(const std::string &t, size_t after_colon) {
        size_t vs = t.find('"', after_colon);
        if (vs == std::string::npos) return "";
        size_t ve = t.find('"', vs + 1);
        if (ve == std::string::npos) return "";
        return t.substr(vs + 1, ve - vs - 1);
    }

    // Count leading spaces to determine indent level
    static int indent(const std::string &line) {
        int n = 0;
        for (char c : line) { if (c == ' ') n++; else break; }
        return n;
    }

    static bool is_integer(const std::string &s) {
        if (s.empty()) return false;
        for (char c : s) if (!isdigit((unsigned char)c)) return false;
        return true;
    }

    bool parse(const std::string &json) {
        std::istringstream stream(json);
        std::string line;

        // Parsing state
        std::string cur_level;      // current "levelN" key
        bool        in_pit  = false; // inside a "levelNPIT" block
        int         cur_row = -1;   // current row index (-1 = not in a row)

        while (std::getline(stream, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] != '"') continue;

            size_t col_pos = t.find("\":");
            if (col_pos == std::string::npos) continue;
            std::string key = t.substr(1, col_pos - 1);
            int ind = indent(line);

            // ── Depth 1: top-level block key ("levelN" or "levelNPIT") ──────
            if (ind == 4) {
                cur_row = -1;
                if (key.find("level") == 0) {
                    size_t pit_pos = key.find("PIT");
                    if (pit_pos != std::string::npos) {
                        // PIT block — extract the level name before "PIT"
                        cur_level = key.substr(0, pit_pos);
                        in_pit    = true;
                    } else {
                        cur_level = key;
                        in_pit    = false;
                        if (!levels.count(cur_level)) {
                            WillyLevel lv;
                            for (int r = 0; r < W_MAXROWS; r++)
                                for (int c = 0; c < W_COLS; c++)
                                    lv.grid[r][c] = "EMPTY";
                            levels[cur_level] = lv;
                        }
                    }
                }
                continue;
            }

            if (cur_level.empty()) continue;

            // ── Depth 2: row index key or PIT key ───────────────────────────
            if (ind == 8) {
                if (in_pit) {
                    // "PRIMARYBALLPIT": [row, col]
                    if (key == "PRIMARYBALLPIT") {
                        size_t lb = t.find('[', col_pos);
                        size_t cm = (lb != std::string::npos) ? t.find(',', lb) : std::string::npos;
                        size_t rb = (cm != std::string::npos) ? t.find(']', cm) : std::string::npos;
                        if (lb != std::string::npos && cm != std::string::npos && rb != std::string::npos) {
                            int r = std::stoi(t.substr(lb + 1, cm - lb - 1));
                            int c = std::stoi(t.substr(cm + 1, rb - cm - 1));
                            auto it = levels.find(cur_level);
                            if (it != levels.end()) {
                                it->second.ballpit_pos   = {r, c};
                                it->second.ballpit_found = true;
                            }
                        }
                    }
                } else {
                    // Row index key — must be a plain integer string
                    if (is_integer(key)) {
                        cur_row = std::stoi(key);
                    } else {
                        cur_row = -1;
                    }
                }
                continue;
            }

            // ── Depth 3: column index key → tile value ───────────────────────
            if (ind == 12 && !in_pit && cur_row >= 0 &&
                cur_row < W_MAXROWS && is_integer(key)) {
                int col = std::stoi(key);
                if (col < 0 || col >= W_COLS) continue;
                std::string tile = extract_string_value(t, col_pos + 1);
                if (tile.empty()) continue;
                auto &lv = levels[cur_level];
                if (tile == "WILLY_RIGHT" || tile == "WILLY_LEFT") {
                    lv.willy_start    = {cur_row, col};
                    lv.grid[cur_row][col] = "EMPTY";
                } else {
                    lv.grid[cur_row][col] = tile;
                }
                continue;
            }
        }

        // Finalise: fill blanks, snapshot orig
        for (auto &[name, lv] : levels)
            for (int r = 0; r < W_MAXROWS; r++)
                for (int c = 0; c < W_COLS; c++) {
                    if (lv.grid[r][c].empty()) lv.grid[r][c] = "EMPTY";
                    lv.grid_orig[r][c] = lv.grid[r][c];
                }

        return !levels.empty();
    }

    bool load_from_string(const std::string &json) { return parse(json); }

    bool load_from_disk(const std::string &filename) {
        for (auto &p : { filename,
                          std::string("data/") + filename,
                          std::string("/usr/games/willytheworm/data/") + filename }) {
            std::ifstream f(p);
            if (!f.good()) continue;
            std::stringstream buf; buf << f.rdbuf();
            return parse(buf.str());
        }
        return false;
    }

    bool level_exists(const std::string &n) const { return levels.count(n) > 0; }

    const std::string &get_tile(const std::string &lname, int r, int c) const {
        static const std::string empty = "EMPTY";
        auto it = levels.find(lname);
        if (it == levels.end()) return empty;
        if (r < 0 || r >= W_MAXROWS || c < 0 || c >= W_COLS) return empty;
        return it->second.grid[r][c];
    }

    void set_tile(const std::string &lname, int r, int c, const std::string &tile) {
        auto it = levels.find(lname);
        if (it == levels.end()) return;
        if (r < 0 || r >= W_MAXROWS || c < 0 || c >= W_COLS) return;
        it->second.grid[r][c] = tile;
    }

    void reset(const std::string &lname) {
        auto it = levels.find(lname);
        if (it == levels.end()) return;
        for (int r = 0; r < W_MAXROWS; r++)
            for (int c = 0; c < W_COLS; c++)
                it->second.grid[r][c] = it->second.grid_orig[r][c];
    }

    std::pair<int,int> willy_start(const std::string &lname) const {
        auto it = levels.find(lname);
        return (it != levels.end()) ? it->second.willy_start : std::make_pair(23, 7);
    }

    std::pair<int,int> ballpit(const std::string &lname) const {
        auto it = levels.find(lname);
        if (it == levels.end() || !it->second.ballpit_found) return {0, 0};
        return it->second.ballpit_pos;
    }
};

// Load levels — decompresses from header, falls back to disk
static bool ww_load_levels(WillyLevels &lv) {
    if (WILLY_ASSETS_EMBEDDED && willy_levels_z_len > 1) {
        std::vector<uint8_t> raw(willy_levels_raw_len);
        mz_ulong out_len = (mz_ulong)willy_levels_raw_len;
        if (mz_uncompress(raw.data(), &out_len,
                          willy_levels_z, (mz_ulong)willy_levels_z_len) == MZ_OK) {
            std::string json(raw.begin(), raw.begin() + out_len);
            if (lv.load_from_string(json)) return true;
        }
    }
    return lv.load_from_disk("levels.json");
}

// =============================================================================
// BALL
// =============================================================================
struct WBall { int row, col; std::string dir; };

// =============================================================================
// SUB-GAME STATE
// =============================================================================
enum class WillySubState { PLAYING, DEAD_FLASH, GAME_OVER };

struct WillyWoprState {
    WillyLevels  levels;
    std::string  cur_level;

    int  wy = 23, wx = 7;
    int  pwy = 23, pwx = 7;
    std::string dir = "RIGHT";
    int  vy = 0;
    bool jumping = false;

    bool key_left = false, key_right = false;
    bool key_up   = false, key_down  = false;
    bool key_space_new = false;

    bool        moving = false;
    std::string move_dir;

    std::vector<WBall> balls;
    int  num_balls = 6;

    int score = 0, lives = 5, level = 1, bonus = 1000, life_adder = 0;

    double tick_acc       = 0.0;
    double tick_rate      = 0.1;   // 10 fps game logic
    int    frame_count    = 0;

    double ball_spawn_acc   = 0.0;
    double ball_spawn_delay = 1.0;

    WillySubState sub_state   = WillySubState::PLAYING;
    double        flash_timer = 0.0;

    float render_x0 = 0, render_y0 = 0;
    float cell_w = 0,    cell_h = 0;

    std::mt19937 rng{std::random_device{}()};
};

// =============================================================================
// TILE / MOVEMENT HELPERS
// =============================================================================

static const std::string &ww_get(WillyWoprState *s, int r, int c) {
    return s->levels.get_tile(s->cur_level, r, c);
}
static void ww_set(WillyWoprState *s, int r, int c, const std::string &t) {
    s->levels.set_tile(s->cur_level, r, c, t);
}
static bool ww_is_pipe(const std::string &t) {
    return t.size() >= 4 && t.substr(0, 4) == "PIPE";
}
static bool ww_can_move(WillyWoprState *s, int r, int c) {
    if (r < 0 || r >= W_ROWS || c < 0 || c >= W_COLS) return false;
    const std::string &t = ww_get(s, r, c);
    return (t == "EMPTY" || t == "LADDER" || t == "PRESENT" ||
            t == "BELL"  || t == "UPSPRING" || t == "SIDESPRING" ||
            t == "TACK"  || t == "BALLPIT");
}
static bool ww_on_solid(WillyWoprState *s) {
    if (s->wy >= W_MAXROWS - 1) return true;
    return (ww_get(s, s->wy, s->wx) == "LADDER") ||
           ww_is_pipe(ww_get(s, s->wy + 1, s->wx));
}
static bool ww_ball_at(WillyWoprState *s, int r, int c) {
    for (auto &b : s->balls) if (b.row == r && b.col == c) return true;
    return false;
}

// =============================================================================
// LEVEL LOADING
// =============================================================================

static void ww_load_level(WillyWoprState *s, int num) {
    s->level     = num;
    s->cur_level = "level" + std::to_string(num);
    auto start   = s->levels.willy_start(s->cur_level);
    s->wy = start.first;  s->wx = start.second;
    s->pwy = s->wy;       s->pwx = s->wx;
    s->vy = 0;            s->jumping = false;
    s->balls.clear();
    s->ball_spawn_acc   = 0.0;
    s->ball_spawn_delay = 1.0;
    s->bonus            = 1000;
    s->frame_count      = 0;
    s->moving           = false;
    s->move_dir.clear();
}

// =============================================================================
// DIE / COMPLETE
// =============================================================================

static void ww_die(WillyWoprState *s) {
    s->lives--;
    s->sub_state  = WillySubState::DEAD_FLASH;
    s->flash_timer = 0.4;
}

static void ww_complete_level(WillyWoprState *s) {
    s->score += s->bonus;
    s->levels.reset(s->cur_level);
    int next = s->level + 1;
    if (s->levels.level_exists("level" + std::to_string(next)))
        ww_load_level(s, next);
    else { s->levels.reset("level1"); ww_load_level(s, 1); }
    s->sub_state = WillySubState::PLAYING;
}

// =============================================================================
// GAME TICK  (mirrors willy.cpp exactly)
// =============================================================================

static void ww_tick(WillyWoprState *s) {
    if (s->sub_state != WillySubState::PLAYING) return;

    s->pwy = s->wy; s->pwx = s->wx;

    const std::string &cur_tile = ww_get(s, s->wy, s->wx);
    bool on_ladder       = (cur_tile == "LADDER");
    bool moved_on_ladder = false;

    // Up (ladder)
    if (s->key_up) {
        int tr = s->wy - 1;
        if (tr >= 0) {
            const std::string &at = ww_get(s, tr, s->wx);
            if ((on_ladder || at == "LADDER") && at == "LADDER" && ww_can_move(s, tr, s->wx)) {
                if (!ww_ball_at(s, tr, s->wx)) {
                    s->wy--; s->vy = 0; moved_on_ladder = true;
                    s->moving = false; s->move_dir.clear();
                } else { ww_die(s); return; }
            }
        }
    }

    // Down (ladder)
    if (s->key_down && !moved_on_ladder) {
        int tr = s->wy + 1;
        if (tr < W_ROWS) {
            const std::string &at = ww_get(s, tr, s->wx);
            if ((on_ladder || at == "LADDER") && ww_can_move(s, tr, s->wx)) {
                if (!ww_ball_at(s, tr, s->wx)) {
                    s->wy++; s->vy = 0; moved_on_ladder = true;
                    s->moving = false; s->move_dir.clear();
                } else { ww_die(s); return; }
            }
        }
    }

    // Horizontal
    if (!moved_on_ladder) {
        std::string hdir;
        if      (s->moving && !s->move_dir.empty()) hdir = s->move_dir;
        else if (s->key_left)  { hdir = "LEFT";  s->dir = "LEFT";  }
        else if (s->key_right) { hdir = "RIGHT"; s->dir = "RIGHT"; }

        if (!hdir.empty()) {
            int tc = (hdir == "LEFT") ? s->wx - 1 : s->wx + 1;
            if (ww_can_move(s, s->wy, tc)) {
                if (!ww_ball_at(s, s->wy, tc)) {
                    s->wx = tc; s->dir = hdir;
                } else { ww_die(s); return; }
            } else if (s->moving) {
                s->moving = false; s->move_dir.clear();
            }
        }
    }

    // Gravity / jump
    const std::string &ct2 = ww_get(s, s->wy, s->wx);
    bool on_lad2 = (ct2 == "LADDER");

    if (s->key_space_new && !s->jumping) {
        const std::string &below = ww_get(s, s->wy + 1, s->wx);
        if (ct2 == "UPSPRING" || ww_is_pipe(below) || s->wy == W_MAXROWS - 1) {
            s->jumping = true;
            s->vy = (ct2 == "UPSPRING") ? -6 : -5;
        }
    }
    s->key_space_new = false;

    if (!on_lad2) {
        if (!ww_on_solid(s)) s->vy += 1;
        else if (s->vy > 0)  { s->vy = 0; s->jumping = false; }
        if (s->vy != 0) {
            int ny = s->wy + (s->vy > 0 ? 1 : -1);
            if (ww_can_move(s, ny, s->wx)) {
                if (!ww_ball_at(s, ny, s->wx)) s->wy = ny;
                else { ww_die(s); return; }
            }
            if (s->vy < 0) s->vy++;
        }
    } else {
        s->vy = 0; s->jumping = false;
    }

    // Ball movement (mirrors update_balls)
    auto pit = s->levels.ballpit(s->cur_level);
    for (auto &b : s->balls) {
        if (b.row < W_MAXROWS - 1 && !ww_is_pipe(ww_get(s, b.row + 1, b.col))) {
            b.row++; b.dir.clear(); continue;
        }
        if (b.dir.empty()) {
            std::uniform_real_distribution<> dis(0.0, 1.0);
            b.dir = (dis(s->rng) > 0.5) ? "RIGHT" : "LEFT";
        }
        if (b.dir == "RIGHT") {
            if (b.col + 1 < W_COLS && !ww_is_pipe(ww_get(s, b.row, b.col + 1))) b.col++;
            else b.dir = "LEFT";
        } else {
            if (b.col - 1 >= 0 && !ww_is_pipe(ww_get(s, b.row, b.col - 1))) b.col--;
            else b.dir = "RIGHT";
        }
        // Return stray balls to ballpit
        if (ww_get(s, b.row, b.col) == "BALLPIT" &&
            (b.row != pit.first || b.col != pit.second)) {
            b.row = pit.first; b.col = pit.second; b.dir.clear();
        }
    }

    // Collisions (mirrors check_collisions)
    const std::string &tile = ww_get(s, s->wy, s->wx);

    // PIPE18 (destroyable) left behind
    if ((s->pwy != s->wy || s->pwx != s->wx) && s->pwy + 1 < W_ROWS) {
        if (ww_get(s, s->pwy + 1, s->pwx) == "PIPE18") {
            ww_set(s, s->pwy + 1, s->pwx, "EMPTY");
            s->score += 50;
        }
    }

    // Ball collision
    for (auto &b : s->balls) {
        if (b.row == s->wy && b.col == s->wx && tile != "BALLPIT") {
            ww_die(s); return;
        }
    }

    // Tile interaction
    if      (tile == "TACK") { ww_die(s); return; }
    else if (tile == "BELL") { ww_complete_level(s); return; }
    else if (tile == "PRESENT") {
        s->score += 100;
        ww_set(s, s->wy, s->wx, "EMPTY");
    } else if (tile == "SIDESPRING") {
        if (s->moving) {
            if      (s->move_dir == "RIGHT") { s->move_dir = "LEFT";  s->dir = "LEFT";  }
            else if (s->move_dir == "LEFT")  { s->move_dir = "RIGHT"; s->dir = "RIGHT"; }
        } else {
            s->dir = (s->dir == "RIGHT") ? "LEFT" : "RIGHT";
        }
    }

    // Bonus countdown / extra life (~1 second at 10 fps)
    s->frame_count++;
    if (s->frame_count >= 10) {
        s->frame_count = 0;
        s->bonus = std::max(0, s->bonus - 10);
        if ((s->score / W_NEWLIFE) > s->life_adder) { s->lives++; s->life_adder++; }
        if (s->bonus <= 0) { ww_die(s); return; }
    }
}

// =============================================================================
// RENDER
// =============================================================================

void wopr_willy_render(WoprState *w, int px, int py, int cw, int ch, int /*cols*/) {
    if (!w->sub_state) return;
    WillyWoprState *s = static_cast<WillyWoprState*>(w->sub_state);

    // Cell size: use font cell height, keep square, clamp to grid width
    float cell = (float)ch;
    if (cell * W_COLS > 1280.f) cell = 1280.f / W_COLS;
    if (cell < 4.f) cell = 4.f;

    float cw_f = cell, ch_f = cell;
    s->render_x0 = (float)px;
    s->render_y0 = (float)py;
    s->cell_w    = cw_f;
    s->cell_h    = ch_f;

    float grid_w = cw_f * W_COLS;
    float grid_h = ch_f * W_ROWS;

    // Blue background (original game colour)
    gl_draw_rect((float)px, (float)py, grid_w, grid_h + ch_f * 2,
                 0.0f, 0.0f, 0.6f, 1.0f);

    // Dead flash: alternate white / blue
    if (s->sub_state == WillySubState::DEAD_FLASH &&
        ((int)(s->flash_timer * 12.0) & 1))
        gl_draw_rect((float)px, (float)py, grid_w, grid_h,
                     1.0f, 1.0f, 1.0f, 1.0f);

    // Level grid
    for (int r = 0; r < W_ROWS; r++) {
        for (int c = 0; c < W_COLS; c++) {
            const std::string &tile = ww_get(s, r, c);
            if (tile == "EMPTY") continue;
            float fx = (float)px + c * cw_f;
            float fy = (float)py + r * ch_f;
            float fr, fg, fb;
            ww_tile_fg(tile, fr, fg, fb);
            ww_draw_sprite(ww_sprite_index(tile), fx, fy, cw_f, ch_f, fr, fg, fb);
        }
    }

    // Balls
    for (auto &b : s->balls) {
        if (b.row < 0 || b.row >= W_ROWS || b.col < 0 || b.col >= W_COLS) continue;
        if (ww_get(s, b.row, b.col) == "BALLPIT") continue;
        ww_draw_sprite(7 /*BALL*/,
                       (float)px + b.col * cw_f, (float)py + b.row * ch_f,
                       cw_f, ch_f, 1.f, 0.2f, 0.2f);
    }

    // Willy (blink during flash)
    bool draw_willy = !(s->sub_state == WillySubState::DEAD_FLASH &&
                        ((int)(s->flash_timer * 12.0) & 1));
    if (draw_willy && s->wy >= 0 && s->wy < W_ROWS && s->wx >= 0 && s->wx < W_COLS)
        ww_draw_sprite((s->dir == "LEFT") ? 1 : 0,
                       (float)px + s->wx * cw_f, (float)py + s->wy * ch_f,
                       cw_f, ch_f, 1.f, 1.f, 1.f);

    // Status bar
    float sy = (float)py + grid_h + 4.f;
    if (s->sub_state == WillySubState::GAME_OVER) {
        gl_draw_text("GAME OVER  -  PRESS ENTER TO PLAY AGAIN",
                     (float)px, sy, 1.f, 0.2f, 0.2f, 1.f, 1.f);
        char sc[64];
        snprintf(sc, sizeof(sc), "FINAL SCORE: %d", s->score);
        gl_draw_text(sc, (float)px, sy + (float)ch, 1.f, 1.f, 0.f, 1.f, 1.f);
    } else {
        char status[128];
        snprintf(status, sizeof(status),
                 "SCORE:%5d  BONUS:%4d  LVL:%2d  LIVES:%d  ESC=EXIT",
                 s->score, s->bonus, s->level, s->lives);
        gl_draw_text(status, (float)px, sy, 0.f, 1.f, 0.4f, 1.f, 1.f);
    }

    gl_flush_verts();
}

// =============================================================================
// UPDATE
// =============================================================================

void wopr_willy_update(WoprState *w, double dt) {
    if (!w->sub_state) return;
    WillyWoprState *s = static_cast<WillyWoprState*>(w->sub_state);

    if (s->sub_state == WillySubState::DEAD_FLASH) {
        s->flash_timer -= dt;
        if (s->flash_timer <= 0.0) {
            if (s->lives <= 0) {
                s->sub_state = WillySubState::GAME_OVER;
            } else {
                s->levels.reset(s->cur_level);
                auto start = s->levels.willy_start(s->cur_level);
                s->wy = start.first; s->wx = start.second;
                s->pwy = s->wy;     s->pwx = s->wx;
                s->vy = 0; s->jumping = false;
                s->balls.clear();
                s->ball_spawn_acc = 0.0; s->ball_spawn_delay = 1.0;
                s->bonus = 1000; s->frame_count = 0;
                s->moving = false; s->move_dir.clear();
                s->key_left = s->key_right = s->key_up = s->key_down = false;
                s->sub_state = WillySubState::PLAYING;
            }
        }
        return;
    }

    if (s->sub_state != WillySubState::PLAYING) return;

    // Ball spawning
    s->ball_spawn_acc += dt;
    if ((int)s->balls.size() < s->num_balls && s->ball_spawn_acc >= s->ball_spawn_delay) {
        s->ball_spawn_acc = 0.0;
        std::uniform_real_distribution<> dd(0.5, 2.0);
        s->ball_spawn_delay = dd(s->rng);
        auto pit = s->levels.ballpit(s->cur_level);
        if (pit.first != 0 || pit.second != 0)
            s->balls.push_back({pit.first, pit.second, ""});
    }

    // Fixed-rate game logic
    s->tick_acc += dt;
    while (s->tick_acc >= s->tick_rate) {
        s->tick_acc -= s->tick_rate;
        ww_tick(s);
        if (s->sub_state != WillySubState::PLAYING) break;
    }
}

// =============================================================================
// KEYBOARD
// =============================================================================

bool wopr_willy_keydown(WoprState *w, SDL_Keycode sym) {
    if (!w->sub_state) return false;
    WillyWoprState *s = static_cast<WillyWoprState*>(w->sub_state);

    if (s->sub_state == WillySubState::GAME_OVER) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            ww_load_level(s, 1);
            s->score = 0; s->lives = 5; s->bonus = 1000; s->life_adder = 0;
            s->sub_state = WillySubState::PLAYING;
        }
        return true;
    }

    switch (sym) {
        case SDLK_LEFT:  case SDLK_a:
            s->key_left = true; s->moving = true;
            s->move_dir = "LEFT";  s->dir = "LEFT";  break;
        case SDLK_RIGHT: case SDLK_d:
            s->key_right = true; s->moving = true;
            s->move_dir = "RIGHT"; s->dir = "RIGHT"; break;
        case SDLK_UP:   case SDLK_w: s->key_up    = true; break;
        case SDLK_DOWN: case SDLK_s: s->key_down  = true; break;
        case SDLK_SPACE:             s->key_space_new = true; break;
        default: s->moving = false; s->move_dir.clear(); break;
    }
    return true;
}

void wopr_willy_keyup(WoprState *w, SDL_Keycode sym) {
    if (!w->sub_state) return;
    WillyWoprState *s = static_cast<WillyWoprState*>(w->sub_state);
    switch (sym) {
        case SDLK_LEFT:  case SDLK_a:
            s->key_left = false;
            if (s->move_dir == "LEFT")  { s->moving = false; s->move_dir.clear(); } break;
        case SDLK_RIGHT: case SDLK_d:
            s->key_right = false;
            if (s->move_dir == "RIGHT") { s->moving = false; s->move_dir.clear(); } break;
        case SDLK_UP:   case SDLK_w: s->key_up   = false; break;
        case SDLK_DOWN: case SDLK_s: s->key_down = false; break;
        default: break;
    }
}

// =============================================================================
// MOUSE
// =============================================================================

void wopr_willy_mousedown(WoprState *w, int mx, int my, int button) {
    if (!w->sub_state) return;
    WillyWoprState *s = static_cast<WillyWoprState*>(w->sub_state);
    if (s->sub_state != WillySubState::PLAYING || s->cell_w == 0.f) return;

    int dc = (int)((mx - s->render_x0) / s->cell_w) - s->wx;
    int dr = (int)((my - s->render_y0) / s->cell_h) - s->wy;

    if (button == 1) {
        if (std::abs(dc) >= std::abs(dr)) {
            if      (dc > 0) { s->moving = true; s->move_dir = "RIGHT"; s->dir = "RIGHT"; }
            else if (dc < 0) { s->moving = true; s->move_dir = "LEFT";  s->dir = "LEFT";  }
        } else {
            if      (dr < 0) s->key_up   = true;
            else if (dr > 0) s->key_down = true;
        }
    } else if (button == 2) {
        s->moving = false; s->move_dir.clear(); s->key_up = s->key_down = false;
    } else if (button == 3) {
        s->key_space_new = true;
    }
}

void wopr_willy_mouseup(WoprState *w, int /*mx*/, int /*my*/, int button) {
    if (!w->sub_state) return;
    WillyWoprState *s = static_cast<WillyWoprState*>(w->sub_state);
    if (button == 1) {
        s->moving = false; s->move_dir.clear();
        s->key_up = s->key_down = false;
    }
}

// =============================================================================
// ENTER / FREE
// =============================================================================

void wopr_willy_enter(WoprState *w) {
    if (w->sub_state) {
        delete static_cast<WillyWoprState*>(w->sub_state);
        w->sub_state = nullptr;
    }

    if (!s_chr_loaded && !ww_load_chr()) {
        w->lines.push_back("  ERROR: willy.chr NOT FOUND.");
        w->lines.push_back("  COPY willy.chr NEXT TO THE BINARY OR RUN gen_willy_assets.py.");
        w->lines.push_back("");
        return;
    }

    auto *s = new WillyWoprState();

    if (!ww_load_levels(s->levels)) {
        w->lines.push_back("  ERROR: levels.json NOT FOUND.");
        w->lines.push_back("  COPY levels.json NEXT TO THE BINARY OR RUN gen_willy_assets.py.");
        w->lines.push_back("");
        delete s; return;
    }
    if (!s->levels.level_exists("level1")) {
        w->lines.push_back("  ERROR: levels.json HAS NO 'level1'.");
        w->lines.push_back("");
        delete s; return;
    }

    s->num_balls  = 6;
    s->lives      = 5;
    s->score      = 0;
    s->life_adder = 0;
    ww_load_level(s, 1);
    s->sub_state = WillySubState::PLAYING;
    w->sub_state = s;
}

void wopr_willy_free(WoprState *w) {
    if (w->sub_state) {
        delete static_cast<WillyWoprState*>(w->sub_state);
        w->sub_state = nullptr;
    }
}

void wopr_willy_textinput(WoprState *w, const char *text) {
    (void)w; (void)text;
}
