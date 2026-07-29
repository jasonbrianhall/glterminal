# WOPR BASIC Print Issue - Root Cause & Fix

## The Problem

When you run:
```
10 PRINT "Hello World"
20 GOTO 10
```

The text **is being generated and stored correctly** (as proven by debug output), but **the display is blank**.

## Root Cause

The issue is in **viewport management**:

1. **`s_screen_top = 0`** — The display viewport is anchored at line 0
2. **`s_cur_row`** climbs to 9410+ as PRINT keeps running
3. **`target = s_screen_top + s_cur_row - 1 = 0 + 9410 - 1 = 9409`**
4. Lines are written to indices 9410+, but the renderer is trying to display lines 0-24 (assuming a 25-row display)
5. **Result: blank screen** (rendering empty space before the actual output)

## The Fix

In `commit_line()`, after appending a new line, auto-adjust `s_screen_top` to keep output visible:

```cpp
// Keep s_screen_top adjusted so new output stays visible
if ((int)lines.size() > 100) {
    if (target >= 100) {
        s_screen_top = (int)lines.size() - 100;
        SDL_Log("[BASIC_DEBUG]   auto-adjusted s_screen_top to %d", s_screen_top);
    }
}
```

This ensures:
- The display viewport "follows" the output as new text is generated
- Oldest lines are pruned (there's already `MAX_WOPR_LINES = 500` limit)
- New text always appears on screen instead of scrolling off into the void

## Why This Happened

`s_screen_top` is designed for programs that use **absolute positioning** (like `LOCATE 5,1`). The fix assumes programs that just use `PRINT` want continuous scrolling like old CRT terminals.

If a program mixes PRINT with explicit `LOCATE` calls, the auto-scroll might conflict. A more robust fix would:
- Set `s_screen_top` properly on `CLS` (which already does `s_screen_top = s_cur_row`)
- Only auto-scroll if the user hasn't explicitly positioned the cursor
- Respect `LOCATE` commands

## Files Modified

- `/home/claude/wopr_basic_debug.cpp` — Contains the fix + enhanced debugging
- Two key changes:
  1. Auto-scroll logic in `commit_line()`
  2. Render viewport debug logging in `wopr_basic_render()`
