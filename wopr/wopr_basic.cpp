// wopr_basic.cpp — WOPR sub-game wrapper for Dungeon (basic)

#include "wopr.h"
#include <SDL2/SDL.h>
#include <string>
#include <cstring>
#include <cctype>
#include <setjmp.h>

// ── BASIC C entry points ────────────────────────────────────────────────
extern "C" {
    int  basic_main(void);

    extern char    basic_input_buf[];
    extern int     basic_input_ready;
    extern int     g_basic_game_over;
    extern jmp_buf basic_exit_jmp;

    void basic_shim_init(void);
    void basic_shim_set_input(const char *line);
}

// ── Per-instance state ────────────────────────────────────────────────────
struct basicState {
    SDL_Thread *thread   = nullptr;
    SDL_mutex  *line_mtx = nullptr;
    SDL_sem    *done_sem = nullptr;
    WoprState  *wopr     = nullptr;
    std::string input_buf;
    bool        dead     = false;
};

static basicState *s_active = nullptr;

// ── Output callback called by supp.c ─────────────────────────────────────
extern "C" void wopr_basic_push_line(const char *line)
{
    if (!s_active || !s_active->wopr) {
        return;
    }
    SDL_LockMutex(s_active->line_mtx);
    s_active->wopr->lines.push_back(std::string(line));
    SDL_UnlockMutex(s_active->line_mtx);
}

// ── Game thread ───────────────────────────────────────────────────────────
static int basic_thread_fn(void *userdata)
{
    basicState *zs = static_cast<basicState *>(userdata);

    if (setjmp(basic_exit_jmp) == 0) {
        // Normal path — run the game
        basic_main();
    } else {
        // exit_() longjmped here
    }

    g_basic_game_over = 1;
    SDL_SemPost(zs->done_sem);
    return 0;
}

// ── Enter ─────────────────────────────────────────────────────────────────
void wopr_basic_enter(WoprState *w)
{
    SDL_Log("[basic] enter");
    basicState *zs  = new basicState();
    zs->line_mtx   = SDL_CreateMutex();
    zs->done_sem   = SDL_CreateSemaphore(0);
    zs->wopr       = w;
    w->sub_state   = zs;
    s_active       = zs;

    g_basic_game_over  = 0;
    basic_input_ready  = 0;
    basic_input_buf[0] = '\0';
    basic_shim_init();

    zs->thread = SDL_CreateThread(basic_thread_fn, "basicThread", zs);
    if (!zs->thread) {
        w->lines.push_back("  [basic] THREAD CREATION FAILED: " + std::string(SDL_GetError()));
        zs->dead = true;
    }
}

// ── Update ────────────────────────────────────────────────────────────────
void wopr_basic_update(WoprState *w, double /*dt*/)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return;

    if (SDL_SemTryWait(zs->done_sem) == 0) {
        zs->dead = true;

        w->lines.push_back("");
        w->lines.push_back("  -- BASIC SESSION ENDED --");
        w->lines.push_back("  TYPE  LIST GAMES  TO PLAY AGAIN.");
        w->lines.push_back("");

        if (zs->thread) {
            SDL_WaitThread(zs->thread, nullptr);
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
    }
}

// ── Render ────────────────────────────────────────────────────────────────
void wopr_basic_render(WoprState *w, int /*x*/, int /*y*/,
                      int /*cw*/, int /*ch*/, int /*cols*/)
{
    (void)w;
}

// ── Keydown ───────────────────────────────────────────────────────────────
bool wopr_basic_keydown(WoprState *w, SDL_Keycode sym)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return false;

    switch (sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
        std::string line = zs->input_buf;
        w->lines.push_back("> " + line);
        line += '\n';
        basic_shim_set_input(line.c_str());
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
void wopr_basic_text(WoprState *w, const char *text)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead || !text) return;
    for (const char *p = text; *p; ++p) {
        char c = (char)((unsigned char)*p);
        zs->input_buf += c;
    }
    w->input_buf = zs->input_buf;
}

// ── Free ─────────────────────────────────────────────────────────────────
void wopr_basic_free(WoprState *w)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs) {
        return;
    }

    if (!zs->dead) {
        g_basic_game_over = 1;
        // Unblock fgets if waiting
        basic_shim_set_input("QUIT\n");
    }

    if (zs->thread) {
        SDL_WaitThread(zs->thread, nullptr);
        zs->thread = nullptr;
    }

    if (s_active == zs)
        s_active = nullptr;

    SDL_DestroyMutex(zs->line_mtx);
    SDL_DestroySemaphore(zs->done_sem);
    delete zs;
    w->sub_state = nullptr;
}
