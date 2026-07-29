// wopr_basic_graphics.cpp
// ============================================================================
// WOPR Graphics Backend for BASIC Interpreter — Implementation
//
// Provides init/shutdown and dirty-flag management for graphics rendering
// in the WOPR overlay when -DWOPR is defined.
// ============================================================================

#ifdef WOPR

#include "wopr.h"
#include "wopr_basic_graphics.h"
#include "basic_gfx.h"
#include "basic_gfx_sdl.h"
#include <SDL2/SDL.h>

// ============================================================================
// Graphics State
// ============================================================================

/* Track whether graphics need to be rendered this frame */
static bool s_graphics_dirty = false;

/* Called from wopr_update to check if we need to render graphics */
bool wopr_graphics_is_dirty(void) {
    return s_graphics_dirty;
}

/* Reset the dirty flag after rendering */
void wopr_graphics_clear_dirty(void) {
    s_graphics_dirty = false;
}

/* Mark graphics as needing render (called by all drawing functions) */
void wopr_graphics_mark_dirty(void) {
    s_graphics_dirty = true;
    #ifdef USE_SDL_WINDOW
    gfx_sdl_mark_dirty();
    #endif
}

// ============================================================================
// Lifecycle
// ============================================================================

void wopr_graphics_init(void) {
    SDL_Log("[WOPR_GRAPHICS] Init: checking gfx_active=%d", gfx_active());
    /* Graphics backend is already initialized by gfx_sdl_init() 
       called from main. This is just a hook for future setup. */
    s_graphics_dirty = false;
}

void wopr_graphics_shutdown(void) {
    SDL_Log("[WOPR_GRAPHICS] Shutdown");
    /* Graphics cleanup is handled by gfx_sdl_shutdown() in main */
}

// ============================================================================
// Integration with WOPR render loop
// ============================================================================

/* Call this from wopr_update(dt) to handle graphics rendering */
void wopr_graphics_update(void) {
    if (s_graphics_dirty) {
        #ifdef USE_SDL_WINDOW
        gfx_sdl_render();
        #endif
        s_graphics_dirty = false;
    }
}

#endif /* WOPR */
