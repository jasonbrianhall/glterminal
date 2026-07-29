# WOPR BASIC Debug Output Additions

## Problem
When running:
```
10 PRINT "hello World"
20 GOTO 10
```

The output shows blank lines instead of the text.

## Debug Changes Made

### 1. Enhanced `wopr_basic_push_line()`
Added logging to trace when text is received and accumulated:

```cpp
SDL_Log("[BASIC_DEBUG] wopr_basic_push_line called with: %s", text);
SDL_Log("[BASIC_DEBUG]   s_out_buf before: '%s' (len=%zu)", s_out_buf.c_str(), s_out_buf.length());
SDL_Log("[BASIC_DEBUG]   s_cur_row=%d, s_cur_col=%d", s_cur_row, s_cur_col);
// ... processing ...
SDL_Log("[BASIC_DEBUG] wopr_basic_push_line done");
SDL_Log("[BASIC_DEBUG]   s_out_buf after: '%s' (len=%zu)", s_out_buf.c_str(), s_out_buf.length());
```

This shows:
- What text the BASIC engine is trying to output
- Whether it's being accumulated in `s_out_buf`
- Current cursor position

### 2. Enhanced `commit_line()`
Added detailed logging when lines are flushed to the terminal buffer:

```cpp
SDL_Log("[BASIC_DEBUG] commit_line: buf='%s' (len=%zu), row=%d, col=%d", 
        s_out_buf.c_str(), s_out_buf.length(), s_cur_row, s_cur_col);
// ...
SDL_Log("[BASIC_DEBUG]   target line index: %d (screen_top=%d)", target, s_screen_top);
SDL_Log("[BASIC_DEBUG]   appended as line %d (total lines now: %zu)", target, lines.size());
SDL_Log("[BASIC_DEBUG]   commit_line done, moving to row %d", s_cur_row);
```

This shows:
- Which lines are being written
- The actual line buffer contents
- How many total lines have been accumulated

## What to Look For

Run your test program and check the console output for patterns like:

**Expected (working):**
```
[BASIC_DEBUG] wopr_basic_push_line called with: hello World
[BASIC_DEBUG]   s_out_buf before: '' (len=0)
[BASIC_DEBUG]   s_cur_row=1, s_cur_col=1
[BASIC_DEBUG] wopr_basic_push_line done
[BASIC_DEBUG]   s_out_buf after: 'hello World' (len=11)
[BASIC_DEBUG] commit_line: buf='hello World' (len=11), row=1, col=12
[BASIC_DEBUG]   target line index: 0 (screen_top=0)
[BASIC_DEBUG]   appended as line 0 (total lines now: 1)
```

**Potential Issues:**

1. **`wopr_basic_push_line` not called at all** → Check if BASIC is even calling print output
2. **Text received but `s_out_buf` empty after** → Characters being dropped during ANSI parsing
3. **`commit_line` never called** → Newlines not being sent (check PRINT implementation)
4. **`commit_line` early return** → `s_active` or `wopr` is null (initialization issue)

## File Location
Modified version: `/home/claude/wopr_basic_debug.cpp`

Replace your `wopr_basic.cpp` with this debug version to enable logging.
