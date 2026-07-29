# WOPR BASIC Graphics — Quick Checklist

## Pre-Integration Verification

- [ ] Have `basic_gfx.h` and `basic_gfx_sdl.h` in your source
- [ ] Have `basic_gfx_sdl.cpp` compiled into your build
- [ ] Can compile with `-DUSE_SDL_WINDOW` flag
- [ ] SDL2 development libraries installed

## File Additions

- [ ] Copy `wopr_basic_graphics.h` to source directory
- [ ] Copy `wopr_basic_graphics.cpp` to source directory
- [ ] Add both to your Makefile/CMake

## commands.cpp Changes

### Add Include (top of file, after other includes)
```cpp
#ifdef WOPR
#include "wopr_basic_graphics.h"
#define gfx_screen(m)          wopr_gfx_screen(m)
#define gfx_screen_tc(w,h)     wopr_gfx_screen_tc((w),(h))
// ... (copy all 15 #defines from wopr_basic_graphics.h header comment)
#endif
```
- [ ] Added include block

### Replace Graphics Conditionals (8 locations)
Use search-and-replace:
- [ ] Search: `#ifdef USE_SDL_WINDOW`
- [ ] Count: Should find ~8 occurrences

For each occurrence in a graphics command, change to:
```cpp
#if defined(USE_SDL_WINDOW) || defined(WOPR)
```

Affected functions:
- [ ] cmd_screen (2 locations)
- [ ] cmd_cls (1 location)
- [ ] cmd_pset (1 location)  
- [ ] cmd_box (1 location)
- [ ] cmd_circle (1 location)
- [ ] cmd_line_gfx (1 location)
- [ ] cmd_paint (1 location)

## wopr.cpp Changes

### wopr_open() — Add graphics init
```cpp
void wopr_open() {
    // ... existing code ...
    #ifdef WOPR
    wopr_graphics_init();
    #endif
}
```
- [ ] Added init call

### wopr_close() — Add graphics shutdown
```cpp
void wopr_close() {
    #ifdef WOPR
    wopr_graphics_shutdown();
    #endif
    // ... existing cleanup ...
}
```
- [ ] Added shutdown call

### wopr_update(double dt) — Add graphics render
```cpp
void wopr_update(double dt) {
    // ... existing update code ...
    #ifdef WOPR
    wopr_graphics_update();
    #endif
}
```
- [ ] Added update call

## Main Loop Setup

Ensure these calls exist in your main game loop:
```cpp
int main() {
    // Initialize graphics backend BEFORE WOPR
    #ifdef USE_SDL_WINDOW
    if (!gfx_sdl_init("WOPR BASIC", 1024, 768)) {
        return 1;
    }
    #endif
    
    // Initialize WOPR (calls wopr_graphics_init internally)
    wopr_open();
    
    // Main loop
    while (running) {
        wopr_update(dt);      // includes wopr_graphics_update()
        wopr_render(w, h);    // render text & UI over graphics
        #ifdef USE_SDL_WINDOW
        gfx_sdl_pump();       // handle SDL events
        #endif
    }
    
    wopr_close();            // calls wopr_graphics_shutdown()
    
    #ifdef USE_SDL_WINDOW
    gfx_sdl_shutdown();
    #endif
    
    return 0;
}
```
- [ ] gfx_sdl_init() called before wopr_open()
- [ ] wopr_update() called each frame
- [ ] gfx_sdl_render() (implicitly via wopr_graphics_update())
- [ ] wopr_graphics_shutdown() called on exit

## Compilation

```bash
gcc -DWOPR -DUSE_SDL_WINDOW \
    commands.cpp wopr_basic_graphics.cpp wopr_basic.cpp wopr.cpp \
    basic_gfx_sdl.cpp basic_gfx.cpp \
    ... other files ... \
    $(pkg-config --cflags --libs sdl2) \
    -o wopr_basic
```
- [ ] Includes `-DWOPR` flag
- [ ] Includes `-DUSE_SDL_WINDOW` flag  
- [ ] Links `wopr_basic_graphics.cpp`
- [ ] Links `basic_gfx_sdl.cpp`
- [ ] Links SDL2 libraries

## Quick Test

```basic
SCREEN _NEWIMAGE(640, 480, 32)
CLS
CIRCLE (320, 240), 50, _RGB(255, 0, 0), , , , 1
SLEEP 3
```

- [ ] Red filled circle appears
- [ ] Circle centered on screen
- [ ] Program completes without errors

## Debugging

If graphics don't appear, check in order:

1. **Compilation:**
   - [ ] Verify both `-DWOPR` and `-DUSE_SDL_WINDOW` flags passed
   - [ ] Check for linker errors (missing wopr_basic_graphics.cpp)

2. **SDL Window:**
   - [ ] Verify SDL window opens before BASIC runs
   - [ ] Check console for SDL initialization errors
   - [ ] Add logging: `SDL_Log("SDL active");` in main loop

3. **Graphics Functions:**
   - [ ] Check if `gfx_active()` returns true after SCREEN
   - [ ] Add logging to `wopr_graphics_update()`:
     ```cpp
     SDL_Log("[GFX] update, dirty=%d", s_graphics_dirty);
     ```
   - [ ] Verify `wopr_gfx_circle()` is being called

4. **Render Loop:**
   - [ ] Check if `wopr_graphics_update()` called each frame
   - [ ] Add frame counter: `static int frames=0; if(++frames%60==0) SDL_Log("frame %d", frames);`
   - [ ] Verify SDL window stays open during execution

## Common Errors

| Error | Cause | Fix |
|-------|-------|-----|
| `undefined reference to gfx_*` | Missing `basic_gfx_sdl.cpp` | Link the file |
| `undefined reference to wopr_gfx_*` | Missing `wopr_basic_graphics.cpp` | Link the file |
| Graphics don't draw | `#ifdef USE_SDL_WINDOW` still blocking WOPR | Change to `#if defined() \|\| defined(WOPR)` |
| Nothing renders | `gfx_sdl_init()` not called | Add to main before `wopr_open()` |
| Window freezes | `gfx_sdl_pump()` not called | Add to main loop |
| Text invisible | Graphics mode not cleared | Call `wopr_graphics_shutdown()` on exit |

## Next Steps

After graphics work:
- [ ] Test with bubbles.bas
- [ ] Test line drawing
- [ ] Test flood fill (PAINT)
- [ ] Test sprite GET/PUT operations
- [ ] Test palette changes
