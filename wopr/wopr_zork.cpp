// wopr_zork.cpp — WOPR sub-game wrapper for Dungeon (Zork)

#include "wopr.h"
#include <SDL2/SDL.h>
#include <string>
#include <cstring>
#include <cctype>
#include <setjmp.h>

// ── Dungeon C entry points ────────────────────────────────────────────────
extern "C" {
    int  zork_main(void);

    extern char    zork_input_buf[];
    extern int     zork_input_ready;
    extern int     g_zork_game_over;
    extern jmp_buf zork_exit_jmp;

    void zork_shim_init(void);
    void zork_shim_set_input(const char *line);
}

// ── Per-instance state ────────────────────────────────────────────────────
struct ZorkState {
    SDL_Thread *thread   = nullptr;
    SDL_mutex  *line_mtx = nullptr;
    SDL_sem    *done_sem = nullptr;
    WoprState  *wopr     = nullptr;
    std::string input_buf;
    bool        dead     = false;
};

static ZorkState *s_active = nullptr;

// ── Output callback called by supp.c ─────────────────────────────────────
extern "C" void wopr_zork_push_line(const char *line)
{
    if (!s_active || !s_active->wopr) {
        SDL_Log("[ZORK] push_line: dropping (no active state): %s", line);
        return;
    }
    SDL_LockMutex(s_active->line_mtx);
    s_active->wopr->lines.push_back(std::string(line));
    SDL_UnlockMutex(s_active->line_mtx);
}

// ── Game thread ───────────────────────────────────────────────────────────
static int zork_thread_fn(void *userdata)
{
    ZorkState *zs = static_cast<ZorkState *>(userdata);
    SDL_Log("[ZORK] thread started");

    if (setjmp(zork_exit_jmp) == 0) {
        // Normal path — run the game
        zork_main();
        SDL_Log("[ZORK] zork_main returned normally");
    } else {
        // exit_() longjmped here
        SDL_Log("[ZORK] longjmp caught — dungeon exited cleanly");
    }

    g_zork_game_over = 1;
    SDL_SemPost(zs->done_sem);
    SDL_Log("[ZORK] thread posted done_sem, exiting");
    return 0;
}

// ── Enter ─────────────────────────────────────────────────────────────────
void wopr_zork_enter(WoprState *w)
{
    SDL_Log("[ZORK] enter");
    ZorkState *zs  = new ZorkState();
    zs->line_mtx   = SDL_CreateMutex();
    zs->done_sem   = SDL_CreateSemaphore(0);
    zs->wopr       = w;
    w->sub_state   = zs;
    s_active       = zs;

    g_zork_game_over  = 0;
    zork_input_ready  = 0;
    zork_input_buf[0] = '\0';
    zork_shim_init();

    SDL_Log("[ZORK] creating thread");
    zs->thread = SDL_CreateThread(zork_thread_fn, "ZorkThread", zs);
    if (!zs->thread) {
        SDL_Log("[ZORK] thread creation FAILED: %s", SDL_GetError());
        w->lines.push_back("  [ZORK] THREAD CREATION FAILED: " + std::string(SDL_GetError()));
        zs->dead = true;
    }
}

// ── Update ────────────────────────────────────────────────────────────────
void wopr_zork_update(WoprState *w, double /*dt*/)
{
    ZorkState *zs = static_cast<ZorkState *>(w->sub_state);
    if (!zs || zs->dead) return;

    if (SDL_SemTryWait(zs->done_sem) == 0) {
        SDL_Log("[ZORK] update: done, cleaning up");
        zs->dead = true;

        w->lines.push_back("");
        w->lines.push_back("  -- ZORK SESSION ENDED --");
        w->lines.push_back("  TYPE  LIST GAMES  TO PLAY AGAIN.");
        w->lines.push_back("");

        if (zs->thread) {
            SDL_Log("[ZORK] update: joining thread");
            SDL_WaitThread(zs->thread, nullptr);
            SDL_Log("[ZORK] update: thread joined");
            zs->thread = nullptr;
        }

        if (s_active == zs)
            s_active = nullptr;

        SDL_DestroyMutex(zs->line_mtx);
        SDL_DestroySemaphore(zs->done_sem);
        delete zs;
        w->sub_state = nullptr;
        w->phase = WoprPhase::GAME_MENU;
        w->input_buf.clear();
        SDL_Log("[ZORK] update: returned to GAME_MENU");
    }
}

// ── Render ────────────────────────────────────────────────────────────────
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
        SDL_Log("[ZORK] input: '%s'", line.c_str());
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
        return false;
    default:
        return true;
    }
}

// ── Text input ────────────────────────────────────────────────────────────
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

// ── Free ─────────────────────────────────────────────────────────────────
void wopr_zork_free(WoprState *w)
{
    SDL_Log("[ZORK] free called");
    ZorkState *zs = static_cast<ZorkState *>(w->sub_state);
    if (!zs) {
        SDL_Log("[ZORK] free: already null");
        return;
    }

    if (!zs->dead) {
        SDL_Log("[ZORK] free: force-stopping thread");
        g_zork_game_over = 1;
        // Unblock fgets if waiting
        zork_shim_set_input("QUIT\n");
    }

    if (zs->thread) {
        SDL_Log("[ZORK] free: joining thread");
        SDL_WaitThread(zs->thread, nullptr);
        SDL_Log("[ZORK] free: thread joined");
        zs->thread = nullptr;
    }

    if (s_active == zs)
        s_active = nullptr;

    SDL_DestroyMutex(zs->line_mtx);
    SDL_DestroySemaphore(zs->done_sem);
    delete zs;
    w->sub_state = nullptr;
    SDL_Log("[ZORK] free done");
}
