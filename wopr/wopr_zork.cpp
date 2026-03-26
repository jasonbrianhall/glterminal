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

    // Shared I/O surfaces declared in supp.c
    extern char zork_input_buf[];
    extern int  zork_input_ready;
    extern int  g_zork_game_over;
    void zork_shim_set_input(const char *line);
}

// ── Per-instance state ────────────────────────────────────────────────────
struct ZorkState {
    SDL_Thread *thread   = nullptr;
    SDL_mutex  *line_mtx = nullptr;   // guards access to the WoprState lines
    WoprState  *wopr     = nullptr;   // back-pointer so the callback can push
    std::string input_buf;            // line being typed by the player
    bool        dead     = false;
};

// ── Active instance pointer (one game at a time) ──────────────────────────
static ZorkState *s_active = nullptr;

// ── Output callback: called by supp.c's more_output() per complete line ───
// This is a plain C function so supp.c can call it without C++ linkage issues.
extern "C" void wopr_zork_push_line(const char *line)
{
    if (!s_active || !s_active->wopr) return;
    SDL_LockMutex(s_active->line_mtx);
    s_active->wopr->lines.push_back(std::string(line));
    SDL_UnlockMutex(s_active->line_mtx);
}

// ── Game thread ───────────────────────────────────────────────────────────
static int zork_thread_fn(void * /*userdata*/)
{
    g_zork_game_over = 0;
    zork_input_ready = 0;

    if (init_())
        game_();

    g_zork_game_over = 1;
    return 0;
}

// ── Enter ─────────────────────────────────────────────────────────────────
void wopr_zork_enter(WoprState *w)
{
    ZorkState *zs  = new ZorkState();
    zs->line_mtx   = SDL_CreateMutex();
    zs->wopr       = w;
    w->sub_state   = zs;
    s_active       = zs;

    zs->thread = SDL_CreateThread(zork_thread_fn, "ZorkThread", nullptr);
    if (!zs->thread) {
        w->lines.push_back("  [ZORK] THREAD CREATION FAILED: " + std::string(SDL_GetError()));
        zs->dead = true;
    }
}

// ── Update ────────────────────────────────────────────────────────────────
void wopr_zork_update(WoprState *w, double /*dt*/)
{
    ZorkState *zs = static_cast<ZorkState *>(w->sub_state);
    if (!zs || zs->dead) return;

    // Lines are pushed directly into w->lines by wopr_zork_push_line().
    // We only need to check for game-over here.
    if (g_zork_game_over) {
        zs->dead = true;
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
// Output is already in w->lines; the standard WOPR terminal renderer handles
// everything. The "> " prompt is drawn by wopr_render() when in_zork is true.
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
        return false;  // let sub_back() in wopr.cpp handle it
    default:
        return true;   // printable chars handled by wopr_zork_text()
    }
}

// ── Text input (SDL_TEXTINPUT event) ─────────────────────────────────────
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

    if (s_active == zs)
        s_active = nullptr;

    SDL_DestroyMutex(zs->line_mtx);
    delete zs;
    w->sub_state = nullptr;
}
