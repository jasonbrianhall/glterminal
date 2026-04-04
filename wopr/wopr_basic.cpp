// wopr_basic.cpp — WOPR sub-game wrapper for Dungeon (basic)

#include "wopr.h"
#include <SDL2/SDL.h>
#include <string>
#include <cstring>
#include <cctype>
#include <setjmp.h>
#include <signal.h>

// ── BASIC C entry points ────────────────────────────────────────────────
extern "C" {
    int  basic_main(void);

    extern char    basic_input_buf[];
    extern int     basic_input_ready;
    extern int     g_basic_game_over;
    extern int     g_basic_waiting_input;
    extern int     g_basic_suppress_newline;
    extern volatile sig_atomic_t g_break;
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

// ── Output callback ───────────────────────────────────────────────────────
static std::string s_out_buf;
static const int   MAX_WOPR_LINES = 500;

extern "C" void wopr_basic_push_line(const char *text)
{
    if (!s_active || !s_active->wopr || !text) return;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') {
            SDL_LockMutex(s_active->line_mtx);
            auto &lines = s_active->wopr->lines;
            lines.push_back(s_out_buf);
            if ((int)lines.size() > MAX_WOPR_LINES)
                lines.erase(lines.begin(), lines.begin() + (lines.size() - MAX_WOPR_LINES));
            SDL_UnlockMutex(s_active->line_mtx);
            s_out_buf.clear();
        } else {
            s_out_buf += *p;
        }
    }
}

bool wopr_basic_is_waiting_input(WoprState *w)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return false;
    return g_basic_waiting_input != 0;
}

// ── Game thread ───────────────────────────────────────────────────────────
static int basic_thread_fn(void *userdata)
{
    basicState *zs = static_cast<basicState *>(userdata);

    SDL_Log("[basic] thread start");

    if (setjmp(basic_exit_jmp) == 0) {
        SDL_Log("[basic] calling basic_main()");
        basic_main();
        SDL_Log("[basic] basic_main() returned normally");
    } else {
        SDL_Log("[basic] longjmp from exit_()");
    }

    g_basic_game_over = 1;
    SDL_Log("[basic] posting done_sem");
    SDL_SemPost(zs->done_sem);
    SDL_Log("[basic] thread end");
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

    g_basic_game_over        = 0;
    basic_input_ready        = 0;
    basic_input_buf[0]       = '\0';
    g_basic_suppress_newline = 0;
    s_out_buf.clear();
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

    // Ctrl+C — interrupt running program
    if (sym == SDLK_c) {
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        if (ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL]) {
            g_break = 1;
            basic_shim_set_input("\n");
            return true;
        }
    }

    if (!g_basic_waiting_input) return true;

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
    if (!zs || zs->dead || !text || !g_basic_waiting_input) return;
    for (const char *p = text; *p; ++p)
        zs->input_buf += (char)((unsigned char)*p);
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
