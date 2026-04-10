// wopr_basic.cpp — WOPR sub-game wrapper for BASIC

#include "wopr.h"
#include <SDL2/SDL.h>
#include <string>
#include <cstring>
#include <cctype>
#include <setjmp.h>
#include <signal.h>
#include "sound.h"

// ── BASIC C entry points ────────────────────────────────────────────────
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

// ── Line color encoding ───────────────────────────────────────────────────
static const char COLOR_PREFIX = '\x01';
static std::string make_colored_line(const std::string &text, uint8_t r, uint8_t g, uint8_t b)
{
    std::string s; s += COLOR_PREFIX; s += (char)r; s += (char)g; s += (char)b; s += text;
    return s;
}
static const uint8_t CGA_RGB[16][3] = {
    {  0,   0,   0},{  0,   0, 170},{  0, 170,   0},{  0, 170, 170},
    {170,   0,   0},{170,   0, 170},{170, 170,   0},{170, 170, 170},
    { 85,  85,  85},{ 85,  85, 255},{ 85, 255,  85},{ 85, 255, 255},
    {255,  85,  85},{255,  85, 255},{255, 255,  85},{255, 255, 255},
};

// ── Per-instance state ────────────────────────────────────────────────────
struct basicState {
    SDL_Thread *thread   = nullptr;
    SDL_mutex  *line_mtx = nullptr;
    SDL_sem    *done_sem = nullptr;
    WoprState  *wopr     = nullptr;
    std::string input_buf;
    bool        dead     = false;

    bool        requires_upper = false;
};

static basicState *s_active = nullptr;
static uint8_t     s_fg_r = 0, s_fg_g = 170, s_fg_b = 0;
static std::string s_out_buf;
static std::string s_prompt_buf;
static uint8_t     s_prompt_r = 0, s_prompt_g = 170, s_prompt_b = 0;
static const int   MAX_WOPR_LINES = 500;

// ── INKEY$ ring buffer ────────────────────────────────────────────────────
static char       s_key_buf[16];
static int        s_key_head = 0, s_key_tail = 0;
static SDL_mutex *s_key_mtx  = nullptr;

void wopr_basic_post_key(char c)
{
    if (!s_key_mtx) return;
    SDL_LockMutex(s_key_mtx);

    basicState *zs = s_active;
    if (zs && zs->requires_upper)
        c = (char)std::toupper((unsigned char)c);

    int next = (s_key_tail + 1) % 16;
    if (next != s_key_head) {
        s_key_buf[s_key_tail] = c;
        s_key_tail = next;
    }
    SDL_UnlockMutex(s_key_mtx);
}

int wopr_basic_get_key(void)
{
    if (!s_key_mtx) return -1;
    SDL_LockMutex(s_key_mtx);
    int c = -1;
    if (s_key_head != s_key_tail) { c = (unsigned char)s_key_buf[s_key_head]; s_key_head = (s_key_head+1)%16; }
    SDL_UnlockMutex(s_key_mtx);
    return c;
}

// ── Output helpers ────────────────────────────────────────────────────────
static void commit_line(void)
{
    if (!s_active || !s_active->wopr) { s_out_buf.clear(); return; }
    SDL_LockMutex(s_active->line_mtx);
    auto &lines = s_active->wopr->lines;
    lines.push_back(make_colored_line(s_out_buf, s_fg_r, s_fg_g, s_fg_b));
    if ((int)lines.size() > MAX_WOPR_LINES)
        lines.erase(lines.begin(), lines.begin() + (lines.size() - MAX_WOPR_LINES));
    SDL_UnlockMutex(s_active->line_mtx);
    s_out_buf.clear();
}
void wopr_basic_push_line(const char *text)
{
    if (!s_active || !s_active->wopr || !text) return;
    for (const char *p = text; *p; ++p) { if (*p == '\n') commit_line(); else s_out_buf += *p; }
}
void wopr_basic_flush_partial(void)
{
    if (!s_active || !s_active->wopr || s_out_buf.empty()) return;
    s_prompt_buf = s_out_buf;
    s_prompt_r = s_fg_r; s_prompt_g = s_fg_g; s_prompt_b = s_fg_b;
    s_out_buf.clear();
}
void wopr_basic_cls(void)
{
    if (!s_active || !s_active->wopr) return;
    s_out_buf.clear();
    s_prompt_buf.clear();
    SDL_LockMutex(s_active->line_mtx);
    s_active->wopr->lines.clear();
    SDL_UnlockMutex(s_active->line_mtx);
}
void wopr_basic_color(int fg)
{
    if (fg < 0 || fg > 15) fg = 7;
    s_fg_r = CGA_RGB[fg][0]; s_fg_g = CGA_RGB[fg][1]; s_fg_b = CGA_RGB[fg][2];
}
bool wopr_basic_is_waiting_input(WoprState *w)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return false;
    return g_basic_waiting_input != 0;
}
const char *wopr_basic_get_prompt(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = s_prompt_r; *g = s_prompt_g; *b = s_prompt_b;
    return s_prompt_buf.c_str();
}

// ── Game thread ───────────────────────────────────────────────────────────
static int basic_thread_fn(void *userdata)
{
    basicState *zs = static_cast<basicState *>(userdata);
    SDL_Log("[basic] thread start");
    if (setjmp(basic_exit_jmp) == 0) {
        basic_main();
    } else {
        SDL_Log("[basic] longjmp from exit_()");
    }
    g_basic_game_over = 1;
    SDL_SemPost(zs->done_sem);
    return 0;
}

// ── Enter ─────────────────────────────────────────────────────────────────
void wopr_basic_enter(WoprState *w)
{
    SDL_Log("[basic] enter");
    basicState *zs = new basicState();
    zs->line_mtx   = SDL_CreateMutex();
    zs->done_sem   = SDL_CreateSemaphore(0);
    zs->wopr = w; w->sub_state = zs; s_active = zs;
    zs->requires_upper = false;
    if (!s_key_mtx) s_key_mtx = SDL_CreateMutex();
    s_key_head = s_key_tail = 0;
    g_basic_game_over = 0; basic_input_ready = 0; basic_input_buf[0] = '\0';
    g_basic_suppress_newline = 0;
    s_out_buf.clear();
    s_prompt_buf.clear();
    s_fg_r = 0; s_fg_g = 170; s_fg_b = 0;
    basic_shim_init();
    sound_init();   /* audio init on main thread */

    zs->thread = SDL_CreateThread(basic_thread_fn, "basicThread", zs);
    if (!zs->thread) {
        w->lines.push_back("  [basic] THREAD CREATION FAILED: " + std::string(SDL_GetError()));
        zs->dead = true;
    }
}

// ── Update ────────────────────────────────────────────────────────────────
void wopr_basic_update(WoprState *w, double)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return;
    if (SDL_SemTryWait(zs->done_sem) == 0) {
        zs->dead = true;
        w->lines.push_back(""); w->lines.push_back("  -- BASIC SESSION ENDED --");
        w->lines.push_back("  TYPE  LIST GAMES  TO PLAY AGAIN."); w->lines.push_back("");
        if (zs->thread) { SDL_WaitThread(zs->thread, nullptr); zs->thread = nullptr; }
        if (s_active == zs) s_active = nullptr;
        SDL_DestroyMutex(zs->line_mtx); SDL_DestroySemaphore(zs->done_sem);
        delete zs; w->sub_state = nullptr;
        w->phase = WoprPhase::GAME_MENU; w->input_buf.clear();
    }
}

// ── Render ────────────────────────────────────────────────────────────────
void wopr_basic_render(WoprState *w, int, int, int, int, int) { (void)w; }

// ── Keydown ───────────────────────────────────────────────────────────────
bool wopr_basic_keydown(WoprState *w, SDL_Keycode sym)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead) return false;

    if (sym == SDLK_c) {
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        if (ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL]) {
            g_break = 1; basic_shim_set_input("\n"); return true;
        }
    }

    if (g_basic_waiting_input) {
        switch (sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: {
            std::string typed = zs->input_buf;
            std::string full = s_prompt_buf + typed;
            w->lines.push_back(make_colored_line(full, s_prompt_r, s_prompt_g, s_prompt_b));
            s_prompt_buf.clear();
            typed += '\n'; basic_shim_set_input(typed.c_str());
            zs->input_buf.clear(); w->input_buf.clear(); return true;
        }
        case SDLK_BACKSPACE:
            if (!zs->input_buf.empty()) { zs->input_buf.pop_back(); w->input_buf = zs->input_buf; }
            return true;
        case SDLK_ESCAPE: return false;
        default: return true;
        }
    } else {
        if (sym == SDLK_ESCAPE)                          { wopr_basic_post_key(27);   return true; }
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) { wopr_basic_post_key('\r'); return true; }
        return true;
    }
}

// ── Text input ────────────────────────────────────────────────────────────
void wopr_basic_text(WoprState *w, const char *text)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs || zs->dead || !text) return;

    if (g_basic_waiting_input) {
        for (const char *p = text; *p; ++p) {
            char c = *p;
            if (zs->requires_upper)
                c = (char)std::toupper((unsigned char)c);
            zs->input_buf += c;
        }
        w->input_buf = zs->input_buf;
    } else {
        for (const char *p = text; *p; ++p)
            wopr_basic_post_key(*p);
    }
}

// ── Wizard's Castle entry ─────────────────────────────────────────────────
// The program is embedded as a C array generated by:
//   xxd -n wopr_basic_program -i wizard.bas > wizard_bas.h
// Include it here so it's only compiled into this TU.
#include "wizard_bas.h"

    void load_program(const char *filename);
    // Optional path: if set before basic_main(), the REPL auto-loads and runs it
    extern char g_autoload_path[512];

// Thread function that auto-runs the loaded program instead of showing REPL
static int wizard_thread_fn(void *userdata)
{
    basicState *zs = static_cast<basicState *>(userdata);
    SDL_Log("[wizard] thread start");

    // Write embedded bytes to a platform-appropriate temp file
    char tmp[512];
#ifdef _WIN32
    const char *tmp_dir = getenv("TEMP");
    if (!tmp_dir) tmp_dir = getenv("TMP");
    if (!tmp_dir) tmp_dir = "C:\\Temp";
    snprintf(tmp, sizeof(tmp), "%s\\wopr_wizard.bas", tmp_dir);
#else
    snprintf(tmp, sizeof(tmp), "/tmp/wopr_wizard.bas");
#endif
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fwrite(wopr_basic_program, 1, wopr_basic_program_len, f);
        fclose(f);
    }

    if (setjmp(basic_exit_jmp) == 0) {
        strncpy(g_autoload_path, tmp, 511);
        g_autoload_path[511] = '\0';
        basic_main();
    } else {
        SDL_Log("[wizard] longjmp from exit_()");
    }
    g_basic_game_over = 1;
    SDL_SemPost(zs->done_sem);
    return 0;
}

void wopr_wizard_enter(WoprState *w)
{
    SDL_Log("[wizard] enter");
    basicState *zs = new basicState();
    zs->line_mtx   = SDL_CreateMutex();
    zs->done_sem   = SDL_CreateSemaphore(0);
    zs->wopr = w; w->sub_state = zs; s_active = zs;
    zs->requires_upper = true;
    if (!s_key_mtx) s_key_mtx = SDL_CreateMutex();
    s_key_head = s_key_tail = 0;
    g_basic_game_over = 0; basic_input_ready = 0; basic_input_buf[0] = '\0';
    g_basic_suppress_newline = 0;
    s_out_buf.clear();
    s_prompt_buf.clear();
    s_fg_r = 0; s_fg_g = 170; s_fg_b = 0;
    basic_shim_init();
    sound_init();

    zs->thread = SDL_CreateThread(wizard_thread_fn, "wizardThread", zs);
    if (!zs->thread) {
        w->lines.push_back("  [wizard] THREAD CREATION FAILED: " + std::string(SDL_GetError()));
        zs->dead = true;
    }
}


void wopr_basic_free(WoprState *w)
{
    basicState *zs = static_cast<basicState *>(w->sub_state);
    if (!zs) return;
    if (!zs->dead) { g_basic_game_over = 1; basic_shim_set_input("QUIT\n"); }
    sound_shutdown();
    if (zs->thread) { SDL_WaitThread(zs->thread, nullptr); zs->thread = nullptr; }
    if (s_active == zs) s_active = nullptr;
    SDL_DestroyMutex(zs->line_mtx); SDL_DestroySemaphore(zs->done_sem);
    delete zs; w->sub_state = nullptr;
}
