// wopr_zork.cpp — WOPR sub-game wrapper for Dungeon (Zork)

#include "wopr.h"
#include <SDL2/SDL.h>
#include <string>
#include <cstring>
#include <cctype>

// ── Dungeon C entry points ────────────────────────────────────────────────
extern "C" {
    int  init_(void);
    void game_(void);

    extern char g_zork_out[];
    extern int  g_zork_out_write;
    extern int  g_zork_out_read;
    extern int  g_zork_game_over;

    void zork_shim_set_input(const char *line);

    static const int ZORK_OUT_SIZE = 65536;
}

// ── Sub-game state ────────────────────────────────────────────────────────
struct ZorkState {
    SDL_Thread *thread      = nullptr;
    SDL_mutex  *out_mtx     = nullptr;
    std::string input_buf;               // line being typed by player
    std::string partial_line;            // incomplete output line accumulator
    bool        dead        = false;     // game thread has exited
};

// ── Ring-buffer drain ─────────────────────────────────────────────────────
static std::string zork_drain_output(ZorkState *zs)
{
    SDL_LockMutex(zs->out_mtx);
    std::string result;
    while (g_zork_out_read != g_zork_out_write) {
        result += g_zork_out[g_zork_out_read];
        g_zork_out_read = (g_zork_out_read + 1) % ZORK_OUT_SIZE;
    }
    SDL_UnlockMutex(zs->out_mtx);
    return result;
}

// ── Game thread ───────────────────────────────────────────────────────────
static int zork_thread_fn(void * /*userdata*/)
{
    g_zork_out_write = 0;
    g_zork_out_read  = 0;
    g_zork_game_over = 0;

    if (init_())
        game_();

    g_zork_game_over = 1;
    return 0;
}

// ── Enter ─────────────────────────────────────────────────────────────────
void wopr_zork_enter(WoprState *w)
{
    ZorkState *zs = new ZorkState();
    zs->out_mtx   = SDL_CreateMutex();
    w->sub_state  = zs;

    zs->thread = SDL_CreateThread(zork_thread_fn, "ZorkThread", nullptr);
    if (!zs->thread) {
        w->lines.push_back("  [ZORK] THREAD CREATION FAILED: " + std::string(SDL_GetError()));
        zs->dead = true;
    }
}

// ── Update (called every frame) ───────────────────────────────────────────
void wopr_zork_update(WoprState *w, double /*dt*/)
{
    ZorkState *zs = static_cast<ZorkState *>(w->sub_state);
    if (!zs) return;

    // Accumulate new output
    zs->partial_line += zork_drain_output(zs);

    // Split on newlines and push complete lines into terminal buffer
    std::string &acc = zs->partial_line;
    size_t pos;
    while ((pos = acc.find('\n')) != std::string::npos) {
        w->lines.push_back(acc.substr(0, pos));
        acc.erase(0, pos + 1);
    }

    // Check for game-over after draining
    if (g_zork_game_over && !zs->dead) {
        zs->dead = true;
        // Flush any remaining partial line
        if (!acc.empty()) {
            w->lines.push_back(acc);
            acc.clear();
        }
        w->lines.push_back("");
        w->lines.push_back("  -- ZORK SESSION ENDED --");
        w->lines.push_back("  TYPE  LIST GAMES  TO PLAY AGAIN.");
        w->lines.push_back("");
        wopr_zork_free(w);
        w->phase = WoprPhase::GAME_MENU;
        w->input_buf.clear();
    }
}

// ── Render ────────────────────────────────────────────────────────────────
// Output is already in w->lines via wopr_zork_update(); the standard WOPR
// terminal renderer handles display. We only need to show the input prompt
// which the WOPR renderer already does for PLAYING_ZORK the same way it
// does for GAME_MENU — using w->input_buf.  Nothing extra needed here.
void wopr_zork_render(WoprState *w, int /*x*/, int /*y*/,
                      int /*cw*/, int /*ch*/, int /*cols*/)
{
    (void)w;
}

// ── Keydown ───────────────────────────────────────────────────────────────
bool wopr_zork_keydown(WoprState *w, SDL_Keycode sym)
{
    ZorkState *zs = static_cast<ZorkState *>(w->sub_state);
    if (!zs || zs->dead) return false;

    switch (sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
        std::string line = zs->input_buf;
        w->lines.push_back("> " + line);
        line += '\n';
        zork_shim_set_input(line.c_str());
        zs->input_buf.clear();
        // Mirror into w->input_buf so renderer clears correctly
        w->input_buf.clear();
        return true;
    }
    case SDLK_BACKSPACE:
        if (!zs->input_buf.empty()) {
            zs->input_buf.pop_back();
            w->input_buf = zs->input_buf;
        }
        return true;
    case SDLK_ESCAPE:
        // Let wopr_keydown's sub_back() handle it
        return false;
    default:
        // Printable input is handled by wopr_zork_text(); nothing extra here.
        return true;
    }
}

// ── Text input (from SDL_TEXTINPUT event) ─────────────────────────────────
void wopr_zork_text(WoprState *w, const char *text)
{
    ZorkState *zs = static_cast<ZorkState *>(w->sub_state);
    if (!zs || zs->dead || !text) return;
    for (const char *p = text; *p; ++p) {
        char c = (char)toupper((unsigned char)*p);
        zs->input_buf += c;
    }
    w->input_buf = zs->input_buf;
}

// ── Free ──────────────────────────────────────────────────────────────────
void wopr_zork_free(WoprState *w)
{
    ZorkState *zs = static_cast<ZorkState *>(w->sub_state);
    if (!zs) return;

    if (!zs->dead) {
        g_zork_game_over = 1;
        zork_shim_set_input("QUIT\n");
    }
    if (zs->thread)
        SDL_WaitThread(zs->thread, nullptr);

    SDL_DestroyMutex(zs->out_mtx);
    delete zs;
    w->sub_state = nullptr;
}
