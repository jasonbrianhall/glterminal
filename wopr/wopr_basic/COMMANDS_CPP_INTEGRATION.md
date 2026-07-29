// ============================================================================
// HOW TO UPDATE commands.cpp FOR WOPR GRAPHICS
// ============================================================================

// Step 1: Add this at the top of commands.cpp (after other includes):

#ifdef WOPR
    #include "wopr_basic_graphics.h"
    /* Map graphics commands to WOPR implementations */
    #define gfx_screen(m)          wopr_gfx_screen(m)
    #define gfx_screen_tc(w,h)     wopr_gfx_screen_tc((w),(h))
    #define gfx_screen_ex(m,c,a,v) wopr_gfx_screen((m))  // simplified for WOPR
    #define gfx_cls(c)             wopr_gfx_cls(c)
    #define gfx_pset(x,y,c)        wopr_gfx_pset((x),(y),(c))
    #define gfx_point(x,y)         wopr_gfx_point((x),(y))
    #define gfx_line(x1,y1,x2,y2,c) wopr_gfx_line((x1),(y1),(x2),(y2),(c))
    #define gfx_box(x1,y1,x2,y2,c) wopr_gfx_box((x1),(y1),(x2),(y2),(c))
    #define gfx_boxfill(x1,y1,x2,y2,c) wopr_gfx_boxfill((x1),(y1),(x2),(y2),(c))
    #define gfx_circle(cx,cy,r,c)  wopr_gfx_circle((cx),(cy),(r),(c))
    #define gfx_arc(cx,cy,r,sa,ea,c) wopr_gfx_arc((cx),(cy),(r),(sa),(ea),(c))
    #define gfx_paint(x,y,fc,bc)   wopr_gfx_paint((x),(y),(fc),(bc))
    #define gfx_palette(i,r,g,b)   wopr_gfx_palette((i),(r),(g),(b))
    #define gfx_active()           wopr_gfx_active()
    #define gfx_width()            wopr_gfx_width()
    #define gfx_height()           wopr_gfx_height()
    #define gfx_get(id,x1,y1,x2,y2) wopr_gfx_get((id),(x1),(y1),(x2),(y2))
    #define gfx_put(id,x,y,xor)    wopr_gfx_put((id),(x),(y),(xor))
#else
    /* For non-WOPR builds: map to OSC escape codes or stub implementations */
    /* (keep original behavior) */
#endif

// Step 2: Now REPLACE all the #ifdef USE_SDL_WINDOW blocks in commands.cpp:

// BEFORE:
// -------
// #ifdef USE_SDL_WINDOW
//     gfx_circle((int)x, (int)y, (int)(r + 0.5), color);
//     gfx_paint((int)x, (int)y, color, color);
// #else
//     felix_drawf("circle;%d;%d;%d;%d", ...);
// #endif

// AFTER:
// ------
// #ifdef WOPR
//     gfx_circle((int)x, (int)y, (int)(r + 0.5), color);
//     gfx_paint((int)x, (int)y, color, color);
// #elif defined(USE_SDL_WINDOW)
//     gfx_circle((int)x, (int)y, (int)(r + 0.5), color);
//     gfx_paint((int)x, (int)y, color, color);
// #else
//     felix_drawf("circle;%d;%d;%d;%d", ...);
// #endif

// Or simpler, just change:
//     #ifdef USE_SDL_WINDOW
// To:
//     #if defined(USE_SDL_WINDOW) || defined(WOPR)

// Step 3: Update wopr.cpp to call graphics render:

// In wopr_update(double dt), add after other updates:
#ifdef WOPR
    wopr_graphics_update();
#endif

// In wopr_open(), add after initialization:
#ifdef WOPR
    wopr_graphics_init();
#endif

// In wopr_close(), add before cleanup:
#ifdef WOPR
    wopr_graphics_shutdown();
#endif

// ============================================================================
// COMPLETE LIST OF CHANGES TO commands.cpp
// ============================================================================

/*
1. After #include directives, add:
   #ifdef WOPR
   #include "wopr_basic_graphics.h"
   // ... (add all the #define mappings above) ...
   #endif

2. In cmd_screen (around line 351):
   CHANGE: #ifdef USE_SDL_WINDOW
   TO:     #if defined(USE_SDL_WINDOW) || defined(WOPR)

3. In cmd_screen (around line 363):
   CHANGE: #ifdef USE_SDL_WINDOW
   TO:     #if defined(USE_SDL_WINDOW) || defined(WOPR)

4. In cmd_cls (around line 653):
   CHANGE: #ifdef USE_SDL_WINDOW
   TO:     #if defined(USE_SDL_WINDOW) || defined(WOPR)

5. In cmd_pset (around line 2094):
   CHANGE: #ifdef USE_SDL_WINDOW
   TO:     #if defined(USE_SDL_WINDOW) || defined(WOPR)

6. In cmd_box (around line 2126):
   CHANGE: #ifdef USE_SDL_WINDOW
   TO:     #if defined(USE_SDL_WINDOW) || defined(WOPR)

7. In cmd_circle (around line 2200):
   CHANGE: #ifdef USE_SDL_WINDOW
   TO:     #if defined(USE_SDL_WINDOW) || defined(WOPR)

8. In cmd_line_gfx (around line 2230):
   CHANGE: #ifdef USE_SDL_WINDOW
   TO:     #if defined(USE_SDL_WINDOW) || defined(WOPR)

9. In cmd_paint (around line ~2300):
   CHANGE: #ifdef USE_SDL_WINDOW
   TO:     #if defined(USE_SDL_WINDOW) || defined(WOPR)
*/

// ============================================================================
// COMPILE COMMAND
// ============================================================================

// gcc -DWOPR -DUSE_SDL_WINDOW commands.cpp wopr_basic_graphics.cpp \
//     basic_gfx_sdl.cpp wopr_basic.cpp wopr.cpp \
//     $(pkg-config --cflags --libs sdl2) \
//     -o wopr_basic
