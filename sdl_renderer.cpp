// ============================================================================
// sdl_renderer.cpp
//
// SDL2 fallback renderer.  Always compiled into the binary alongside
// gl_renderer.cpp.  The public API functions (gl_init_renderer, draw_verts,
// etc.) are defined ONLY in gl_renderer.cpp; this file implements the SDL
// equivalents under sdl_* names and is called by the dispatch layer that is
// added to gl_renderer.cpp (see gl_renderer.cpp.diff).
//
// When g_use_sdl_renderer is false every dispatch stub in gl_renderer.cpp
// falls straight through to the original GL code with zero overhead.
// ============================================================================

#include "gl_renderer.h"   // Vertex, Mat4, GLState, MAX_VERTS, mat4_ortho
#include "sdl_renderer.h"
#include "gl_terminal.h"   // MAX_VERTS cross-check
#include <SDL2/SDL.h>
#include <string.h>

// ============================================================================
// GLOBALS
// ============================================================================

bool          g_use_sdl_renderer = false;
SDL_Renderer *g_sdl_renderer     = nullptr;

static SDL_Texture *s_term_tex      = nullptr;
static SDL_Texture *s_composite_tex = nullptr;
static int          s_tex_w = 0, s_tex_h = 0;

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static SDL_Texture *sdl_make_target(int w, int h) {
    SDL_Texture *t = SDL_CreateTexture(
        g_sdl_renderer, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET, w, h);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static inline SDL_Vertex to_sv(const Vertex &v) {
    SDL_Vertex sv;
    sv.position.x  = v.x;
    sv.position.y  = v.y;
    sv.color.r     = (Uint8)(v.r * 255.f);
    sv.color.g     = (Uint8)(v.g * 255.f);
    sv.color.b     = (Uint8)(v.b * 255.f);
    sv.color.a     = (Uint8)(v.a * 255.f);
    sv.tex_coord.x = sv.tex_coord.y = 0.f;
    return sv;
}

static SDL_Vertex s_accum[MAX_VERTS];
static int        s_accum_n = 0;

// ============================================================================
// PUBLIC SDL-PATH ENTRY POINTS  (called by dispatch stubs in gl_renderer.cpp)
// ============================================================================

void sdl_init_renderer(int w, int h) {
    // Renderer created by gl_terminal_main before this call; just build textures.
    s_tex_w = w; s_tex_h = h;
    if (s_term_tex)      SDL_DestroyTexture(s_term_tex);
    if (s_composite_tex) SDL_DestroyTexture(s_composite_tex);
    s_term_tex      = sdl_make_target(w, h);
    s_composite_tex = sdl_make_target(w, h);
    SDL_SetRenderTarget(g_sdl_renderer, s_term_tex);
    SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdl_renderer);
    SDL_SetRenderTarget(g_sdl_renderer, nullptr);
    // Populate stub GLState so mat4/proj references in shared code don't crash
    G.cr = G.cg = G.cb = G.ca = 1.f;
    G.proj = mat4_ortho(0, (float)w, (float)h, 0, -1, 1);
}

void sdl_resize_fbo(int w, int h) {
    sdl_init_renderer(w, h);  // recreate textures at new size
}

void sdl_flush_verts(void) {
    if (s_accum_n == 0) return;
    SDL_RenderGeometry(g_sdl_renderer, nullptr, s_accum, s_accum_n, nullptr, 0);
    s_accum_n = 0;
}

void sdl_draw_verts(Vertex *v, int n) {
    if (n <= 0) return;
    if (s_accum_n + n > MAX_VERTS) sdl_flush_verts();
    if (n > MAX_VERTS) {
        int done = 0;
        while (done < n) {
            int chunk = (n - done < MAX_VERTS) ? (n - done) : MAX_VERTS;
            for (int i = 0; i < chunk; i++) s_accum[i] = to_sv(v[done + i]);
            SDL_RenderGeometry(g_sdl_renderer, nullptr, s_accum, chunk, nullptr, 0);
            done += chunk;
        }
        return;
    }
    for (int i = 0; i < n; i++) s_accum[s_accum_n + i] = to_sv(v[i]);
    s_accum_n += n;
}

void sdl_begin_term_frame(int, int, float, float, float) {
    s_accum_n = 0;
    SDL_SetRenderTarget(g_sdl_renderer, s_term_tex);
}

void sdl_clear_term_frame(int, int, float bg_r, float bg_g, float bg_b) {
    SDL_SetRenderTarget(g_sdl_renderer, s_term_tex);
    SDL_SetRenderDrawColor(g_sdl_renderer,
        (Uint8)(bg_r*255), (Uint8)(bg_g*255), (Uint8)(bg_b*255), 255);
    SDL_RenderClear(g_sdl_renderer);
}

void sdl_end_term_frame(void) {
    sdl_flush_verts();
    SDL_SetRenderTarget(g_sdl_renderer, nullptr);
}

void sdl_begin_frame(void) {
    s_accum_n = 0;
    SDL_SetRenderTarget(g_sdl_renderer, s_composite_tex);
    SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdl_renderer);
    SDL_SetTextureBlendMode(s_term_tex, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(g_sdl_renderer, s_term_tex, nullptr, nullptr);
    SDL_SetTextureBlendMode(s_term_tex, SDL_BLENDMODE_BLEND);
}

void sdl_end_frame(float /*time*/, int win_w, int win_h) {
    sdl_flush_verts();
    // Post-process modes not implemented — straight blit to screen
    SDL_SetRenderTarget(g_sdl_renderer, nullptr);
    SDL_Rect dst = { 0, 0, win_w, win_h };
    SDL_SetTextureBlendMode(s_composite_tex, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(g_sdl_renderer, s_composite_tex, nullptr, &dst);
    SDL_SetTextureBlendMode(s_composite_tex, SDL_BLENDMODE_BLEND);
}

// sdl_update_ghost: no-op — no feedback FBOs in SDL path
void sdl_update_ghost(int, int) {}
