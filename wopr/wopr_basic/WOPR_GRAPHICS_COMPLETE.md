# WOPR BASIC Graphics — Complete Integration Guide

## Overview

When you compile BASIC with `-DWOPR`, you get a BASIC interpreter running inside the WOPR overlay. Graphics commands (SCREEN, CIRCLE, LINE, etc.) need to route through WOPR's rendering system.

This guide provides **fully working graphics** for all BASIC primitives in WOPR mode.

## Architecture

```
BASIC Code (bubbles.bas)
    ↓
commands.cpp (cmd_circle, cmd_screen, etc.)
    ↓
wopr_basic_graphics.h (macros & wrappers)
    ↓
basic_gfx.h (gfx_circle, gfx_screen, etc.)
    ↓
basic_gfx_sdl.cpp (SDL2 pixel buffer implementation)
    ↓
SDL Texture → SDL Window (visible to user)
```

## Files to Add/Modify

### 1. New Files

**`wopr_basic_graphics.h`** — Header with wrappers
- Provides `wopr_gfx_*()` functions that wrap `gfx_*()` from basic_gfx.h
- Marks graphics dirty after each draw call
- Only compiled when `-DWOPR` is set

**`wopr_basic_graphics.cpp`** — Implementation
- Handles dirty-flag management
- Coordinates with WOPR render loop
- Calls `gfx_sdl_render()` each frame

### 2. Files to Modify

**`commands.cpp`** — The BASIC command interpreter
- Add include: `#include "wopr_basic_graphics.h"`
- Add macro mappings (see below)
- Change `#ifdef USE_SDL_WINDOW` to `#if defined(USE_SDL_WINDOW) || defined(WOPR)`
- Touch ~8 graphics command functions

**`wopr.cpp`** — The WOPR overlay main loop
- Add `wopr_graphics_init()` in `wopr_open()`
- Add `wopr_graphics_shutdown()` in `wopr_close()`
- Add `wopr_graphics_update()` in `wopr_update(dt)`

## Step-by-Step Implementation

### Step 1: Copy New Headers

Copy these files to your BASIC source directory:
- `wopr_basic_graphics.h`
- `wopr_basic_graphics.cpp`

### Step 2: Update commands.cpp

**At the top (after other includes):**
```cpp
#ifdef WOPR
#include "wopr_basic_graphics.h"
/* Map gfx_* functions to WOPR implementations */
#define gfx_screen(m)          wopr_gfx_screen(m)
#define gfx_screen_tc(w,h)     wopr_gfx_screen_tc((w),(h))
#define gfx_screen_ex(m,c,a,v) wopr_gfx_screen((m))
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
#endif
```

**Find and replace all graphics #ifdef blocks:**

**Pattern 1 — cmd_screen (2 locations):**
```cpp
// OLD:
#ifdef USE_SDL_WINDOW
    gfx_screen_tc(tw, th);
#else
    felix_sendf("screen;12");
#endif

// NEW:
#if defined(USE_SDL_WINDOW) || defined(WOPR)
    gfx_screen_tc(tw, th);
#else
    felix_sendf("screen;12");
#endif
```

**Pattern 2 — All other graphics commands (cmd_cls, cmd_pset, cmd_circle, cmd_line_gfx, cmd_box, cmd_paint):**
```cpp
// OLD:
#ifdef USE_SDL_WINDOW
    gfx_function(...);
#else
    felix_drawf(...);
#endif

// NEW:
#if defined(USE_SDL_WINDOW) || defined(WOPR)
    gfx_function(...);
#else
    felix_drawf(...);
#endif
```

**Quick find command:**
```bash
grep -n "#ifdef USE_SDL_WINDOW" commands.cpp
# This will show all ~8 locations that need updating
```

### Step 3: Update wopr.cpp

**In `wopr_open()` function:**
```cpp
void wopr_open() {
    // ... existing initialization ...
    
    #ifdef WOPR
    wopr_graphics_init();
    #endif
}
```

**In `wopr_close()` function:**
```cpp
void wopr_close() {
    #ifdef WOPR
    wopr_graphics_shutdown();
    #endif
    
    // ... existing cleanup ...
}
```

**In `wopr_update(double dt)` function:**
```cpp
void wopr_update(double dt) {
    // ... existing update code ...
    
    #ifdef WOPR
    wopr_graphics_update();
    #endif
}
```

### Step 4: Compile

```bash
gcc -DWOPR -DUSE_SDL_WINDOW \
    commands.cpp wopr_basic_graphics.cpp \
    wopr_basic.cpp wopr.cpp \
    basic_gfx_sdl.cpp \
    ... other source files ... \
    $(pkg-config --cflags --libs sdl2) \
    -o wopr_basic
```

## Testing

### Test 1: Simple Graphics

Run this in WOPR BASIC (F7):
```basic
SCREEN _NEWIMAGE(800, 600, 32)
CLS
CIRCLE (400, 300), 100, _RGB(255, 0, 0), , , , 1
SLEEP 3
```

Expected: Red filled circle appears in center of SDL window

### Test 2: Animation

```basic
SCREEN _NEWIMAGE(640, 480, 32)
FOR i = 1 TO 50
    CLS
    CIRCLE (320, 240), i * 3, _RGB(100 + i*2, i*2, 200), , , , 1
    SLEEP 0.05
NEXT
```

Expected: Animated circles growing from center

### Test 3: Full Program

```basic
LOAD "bubbles.bas"
RUN
```

Expected: 200 gradient circles animate continuously

## Troubleshooting

### "undefined reference to `gfx_screen`"
**Cause:** Not linking `basic_gfx_sdl.cpp`
**Fix:** Add to compile command:
```bash
basic_gfx_sdl.cpp
```

### "undefined reference to `wopr_gfx_*`"
**Cause:** Not linking `wopr_basic_graphics.cpp`
**Fix:** Add to compile command:
```bash
wopr_basic_graphics.cpp
```

### Graphics commands compile but don't draw anything
**Cause:** `wopr_graphics_update()` not called in render loop
**Fix:** Verify `wopr_update(dt)` contains:
```cpp
#ifdef WOPR
wopr_graphics_update();
#endif
```

### SDL window doesn't appear
**Cause:** `gfx_sdl_init()` not called before BASIC runs
**Fix:** In your main loop, before calling `wopr_open()`:
```cpp
#ifdef USE_SDL_WINDOW
if (!gfx_sdl_init("WOPR BASIC", 1024, 768)) {
    SDL_Log("Graphics init failed");
    return 1;
}
#endif
wopr_open();
```

### Graphics appear but text overlay doesn't render
**Cause:** WOPR text and graphics competing for rendering
**Fix:** Ensure `wopr_render()` is called after graphics update:
```cpp
wopr_update(dt);
wopr_render(win_w, win_h);  // Renders text + UI over graphics
```

## What Works

With this integration, all BASIC graphics commands work in WOPR:

- ✓ SCREEN (all modes)
- ✓ _NEWIMAGE (truecolor)
- ✓ CLS
- ✓ PSET
- ✓ CIRCLE (filled, arcs)
- ✓ LINE / LINEEX
- ✓ BOX (outline & filled)
- ✓ PAINT (flood fill)
- ✓ PALETTE (color override)
- ✓ _RGB (color packing)
- ✓ GET / PUT (sprite capture & blit)

## Performance Notes

- Graphics are SDL2 pixel operations (fast)
- Dirty-flag system prevents redundant renders
- ~60 FPS achievable with simple graphics
- Complex fills or large PAINT operations may slow down

## Files Summary

| File | Purpose | Size |
|------|---------|------|
| `wopr_basic_graphics.h` | Headers & inline wrappers | ~150 lines |
| `wopr_basic_graphics.cpp` | Init/shutdown & dirty flags | ~70 lines |
| `commands.cpp` | Modified (8 locations) | Minimal changes |
| `wopr.cpp` | Modified (3 locations) | 3 function calls |

Total new code: ~220 lines
