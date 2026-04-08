# Felix Terminal — BASIC Graphics Protocol

Felix Terminal extends the standard terminal with a graphics protocol that BASIC programs can use to draw primitives, manage sprites, change screen modes, and play music. Commands are sent as OSC escape sequences written to stdout — no special libraries required, just `printf`.

## Escape Sequence Format

```
ESC ] 666 ; <command> ; <arg1> ; <arg2> ; ... ESC \
```

In C:
```c
printf("\033]666;circle;320;175;50;14\033\\");
```

In QBasic/GW-BASIC, wrap it in a helper:
```basic
SUB BGCircle(x, y, r, c)
    PRINT CHR$(27) + "]666;circle;" + STR$(x) + ";" + STR$(y) + ";" + STR$(r) + ";" + STR$(c) + CHR$(27) + "\"
END SUB
```

---

## SCREEN — Set Screen Mode

```
screen ; <mode>
```

Sets the coordinate space and optional visual effect. All subsequent drawing commands use the resolution of the selected screen mode.

| Mode | Resolution  | Notes                        | Effect      |
|------|-------------|------------------------------|-------------|
| 0    | —           | Text mode, no change         | —           |
| 1    | 320 × 200   | CGA 4-color                  | Normal      |
| 2    | 640 × 200   | CGA 2-color                  | Normal      |
| 3    | 720 × 348   | Hercules                     | Normal      |
| 4    | 640 × 400   | Olivetti                     | Normal      |
| 5    | 160 × 100   | CGA low                      | Normal      |
| 6    | 160 × 200   | CGA low                      | Normal      |
| 7    | 320 × 200   | EGA 16-color                 | CRT         |
| 8    | 640 × 200   | EGA 16-color                 | CRT         |
| 9    | 640 × 350   | EGA 64-color *(Gorilla mode)*| Normal      |
| 10   | 640 × 350   | EGA monochrome               | Normal      |
| 11   | 640 × 480   | VGA 2-color                  | Normal      |
| 12   | 640 × 480   | VGA 16-color                 | Normal      |
| 13   | 320 × 200   | VGA 256-color                | Normal      |
| 14   | 320 × 200   | PCP 16-color                 | Normal      |
| 15   | 640 × 200   | PCP 4-color                  | Normal      |
| 16   | 640 × 480   | PGC 256-color                | Normal      |
| 17   | 640 × 480   | IBM 8514/A                   | Normal      |
| 18   | 640 × 480   | JEGA                         | Normal      |
| 19   | 640 × 480   | JEGA text                    | Normal      |
| 20   | 512 × 480   | TIGA                         | Normal      |
| 21   | 640 × 400   | SVGA                         | CRT         |
| 22   | 640 × 480   | SVGA                         | VHS         |
| 23   | 800 × 600   | SVGA                         | C64         |
| 24   | 160 × 200   | Tandy/PCjr                   | Composite   |
| 25   | 320 × 200   | Tandy/PCjr                   | Composite   |
| 26   | 640 × 200   | Tandy/PCjr                   | Normal      |
| 27   | 640 × 200   | Tandy ETGA                   | Normal      |
| 28   | 720 × 350   | OGA                          | Normal      |

**Example:**
```basic
PRINT CHR$(27) + "]666;screen;9" + CHR$(27) + "\"   ' EGA 640x350
```

---

## CLS — Clear Screen

```
cls ; <color>
```

Clears the entire screen to the given palette color. Color 0 is black.

**Example:**
```basic
PRINT CHR$(27) + "]666;cls;0" + CHR$(27) + "\"    ' clear to black
PRINT CHR$(27) + "]666;cls;1" + CHR$(27) + "\"    ' clear to blue
```

---

## PALETTE — Set Palette Entry

```
palette ; <index> ; <r> ; <g> ; <b>
```

Sets palette index (0–15) to the given RGB values (0–255 each).

**Example:**
```basic
PRINT CHR$(27) + "]666;palette;2;0;255;0" + CHR$(27) + "\"  ' index 2 = bright green
```

---

## PSET — Plot a Point

```
pset ; <x> ; <y> ; <color>
```

Plots a single pixel at (x, y) in the given palette color.

**Example:**
```basic
PRINT CHR$(27) + "]666;pset;160;100;14" + CHR$(27) + "\"  ' yellow dot at center
```

---

## LINE — Draw a Line or Rectangle

```
line ; <x1> ; <y1> ; <x2> ; <y2> ; <color>
line ; <x1> ; <y1> ; <x2> ; <y2> ; <color> ; B
line ; <x1> ; <y1> ; <x2> ; <y2> ; <color> ; BF
```

- No suffix: draws a line from (x1,y1) to (x2,y2)
- `B`: draws an unfilled rectangle with those corners
- `BF`: draws a filled rectangle with those corners

**Examples:**
```basic
PRINT CHR$(27) + "]666;line;0;0;640;350;14" + CHR$(27) + "\"       ' diagonal line
PRINT CHR$(27) + "]666;line;10;10;200;100;9;B" + CHR$(27) + "\"    ' unfilled box
PRINT CHR$(27) + "]666;line;10;10;200;100;4;BF" + CHR$(27) + "\"   ' filled box
```

---

## CIRCLE — Draw a Circle

```
circle ; <x> ; <y> ; <radius> ; <color>
```

Draws a circle outline centered at (x, y) with the given radius and palette color.

**Example:**
```basic
PRINT CHR$(27) + "]666;circle;320;175;80;14" + CHR$(27) + "\"
```

---

## PAINT — Flood Fill

```
paint ; <x> ; <y> ; <fill_color>
paint ; <x> ; <y> ; <fill_color> ; <border_color>
```

Flood fills from (x, y) with `fill_color`, stopping at `border_color` boundaries. If `border_color` is omitted, fills until it hits any different color.

**Example:**
```basic
CIRCLE (320, 175), 80, 15
PRINT CHR$(27) + "]666;circle;320;175;80;15" + CHR$(27) + "\"
PRINT CHR$(27) + "]666;paint;320;175;9;15" + CHR$(27) + "\"   ' fill inside with blue
```

---

## GET — Capture a Sprite

```
get ; <sprite_id> ; <x1> ; <y1> ; <x2> ; <y2>
```

Captures the rectangular screen region from (x1,y1) to (x2,y2) and stores it as sprite `sprite_id`. Sprite IDs are integers, reusing an ID overwrites the previous sprite.

**Example:**
```basic
' Draw a gorilla, then capture it
PRINT CHR$(27) + "]666;get;1;100;50;160;120" + CHR$(27) + "\"
```

---

## PUT — Blit a Sprite

```
put ; <sprite_id> ; <x> ; <y> ; pset
put ; <sprite_id> ; <x> ; <y> ; xor
```

Blits sprite `sprite_id` at position (x, y).

- `pset`: normal overwrite blit
- `xor`: XOR blit — blitting twice restores original content (used for animation in QBasic)

**Example:**
```basic
PRINT CHR$(27) + "]666;put;1;200;100;pset" + CHR$(27) + "\"   ' draw sprite
PRINT CHR$(27) + "]666;put;1;200;100;xor" + CHR$(27) + "\"    ' erase sprite
```

---

## PLAY — Play MML Music

```
play ; <mml_string>
```

Plays a Music Macro Language string. Compatible with QB/GW-BASIC MML syntax. Playback is asynchronous — the program continues immediately.

### Supported MML Tokens

| Token         | Description                                              |
|---------------|----------------------------------------------------------|
| `A`–`G`       | Play note (in current octave and length)                 |
| `#` or `+`    | Sharp (follows note letter)                              |
| `-`           | Flat (follows note letter)                               |
| `R` or `P`    | Rest                                                     |
| `O n`         | Set octave (0–7, default 4)                              |
| `<`           | Octave down                                              |
| `>`           | Octave up                                                |
| `L n`         | Set default note length (1=whole, 2=half, 4=quarter…)   |
| `T n`         | Set tempo in BPM (32–255, default 120)                   |
| `V n`         | Set volume (0–15, default 8)                             |
| `N n`         | Play MIDI note number directly (0–84)                    |
| `.`           | Dotted note (×1.5 duration, follows length digit)        |
| `MN`          | Music Normal articulation (default)                      |
| `ML`          | Music Legato                                             |
| `MS`          | Music Staccato                                           |
| `MB` / `MF`   | Music Background / Foreground (accepted, ignored)        |

**Examples:**
```basic
PRINT CHR$(27) + "]666;play;MBT120O2L8CDEFGAB" + CHR$(27) + "\"
PRINT CHR$(27) + "]666;play;MBO0L32EFGEFDC" + CHR$(27) + "\"
```

---

## BATCH — Multiple Commands in One Sequence

```
batch ; <cmd1>
<cmd2>
<cmd3>
```

Sends multiple commands separated by newlines in a single OSC sequence. All commands execute in the same frame, avoiding flicker between draw calls.

In C:
```c
printf("\033]666;batch;cls;0\nline;0;280;640;350;8;BF\ncircle;320;80;35;14\033\\");
```

In shell:
```bash
printf '\033]666;batch;cls;0\nline;0;280;640;350;8;BF\ncircle;320;80;35;14\033\\'
```

---

## Color Reference

Colors 0–15 map to the EGA palette, modified by the current `palette` settings.

| Index | Default Color   |
|-------|-----------------|
| 0     | Black           |
| 1     | Blue            |
| 2     | Green           |
| 3     | Cyan            |
| 4     | Red             |
| 5     | Magenta         |
| 6     | Brown           |
| 7     | Light Gray      |
| 8     | Dark Gray       |
| 9     | Light Blue      |
| 10    | Light Green     |
| 11    | Light Cyan      |
| 12    | Light Red       |
| 13    | Light Magenta   |
| 14    | Yellow          |
| 15    | White           |

Colors above 15 are interpreted as 24-bit RGB: `0xRRGGBB`.

---

## Complete QBasic Helper Subroutines

Drop these into any QBasic program to use the protocol:

```basic
' Send an OSC 666 command
SUB BGSend(cmd$)
    PRINT CHR$(27) + "]666;" + cmd$ + CHR$(27) + "\";
END SUB

SUB BGScreen(mode)
    BGSend "screen;" + LTRIM$(STR$(mode))
END SUB

SUB BGCLS(c)
    BGSend "cls;" + LTRIM$(STR$(c))
END SUB

SUB BGPalette(idx, r, g, b)
    BGSend "palette;" + LTRIM$(STR$(idx)) + ";" + LTRIM$(STR$(r)) + ";" + LTRIM$(STR$(g)) + ";" + LTRIM$(STR$(b))
END SUB

SUB BGPset(x, y, c)
    BGSend "pset;" + LTRIM$(STR$(x)) + ";" + LTRIM$(STR$(y)) + ";" + LTRIM$(STR$(c))
END SUB

SUB BGLine(x1, y1, x2, y2, c)
    BGSend "line;" + LTRIM$(STR$(x1)) + ";" + LTRIM$(STR$(y1)) + ";" + LTRIM$(STR$(x2)) + ";" + LTRIM$(STR$(y2)) + ";" + LTRIM$(STR$(c))
END SUB

SUB BGBox(x1, y1, x2, y2, c)
    BGSend "line;" + LTRIM$(STR$(x1)) + ";" + LTRIM$(STR$(y1)) + ";" + LTRIM$(STR$(x2)) + ";" + LTRIM$(STR$(y2)) + ";" + LTRIM$(STR$(c)) + ";B"
END SUB

SUB BGBoxFill(x1, y1, x2, y2, c)
    BGSend "line;" + LTRIM$(STR$(x1)) + ";" + LTRIM$(STR$(y1)) + ";" + LTRIM$(STR$(x2)) + ";" + LTRIM$(STR$(y2)) + ";" + LTRIM$(STR$(c)) + ";BF"
END SUB

SUB BGCircle(x, y, r, c)
    BGSend "circle;" + LTRIM$(STR$(x)) + ";" + LTRIM$(STR$(y)) + ";" + LTRIM$(STR$(r)) + ";" + LTRIM$(STR$(c))
END SUB

SUB BGPaint(x, y, c, bc)
    BGSend "paint;" + LTRIM$(STR$(x)) + ";" + LTRIM$(STR$(y)) + ";" + LTRIM$(STR$(c)) + ";" + LTRIM$(STR$(bc))
END SUB

SUB BGGet(id, x1, y1, x2, y2)
    BGSend "get;" + LTRIM$(STR$(id)) + ";" + LTRIM$(STR$(x1)) + ";" + LTRIM$(STR$(y1)) + ";" + LTRIM$(STR$(x2)) + ";" + LTRIM$(STR$(y2))
END SUB

SUB BGPut(id, x, y, xorMode)
    IF xorMode THEN
        BGSend "put;" + LTRIM$(STR$(id)) + ";" + LTRIM$(STR$(x)) + ";" + LTRIM$(STR$(y)) + ";xor"
    ELSE
        BGSend "put;" + LTRIM$(STR$(id)) + ";" + LTRIM$(STR$(x)) + ";" + LTRIM$(STR$(y)) + ";pset"
    END IF
END SUB

SUB BGPlay(mml$)
    BGSend "play;" + mml$
END SUB
```

---

## Notes

- Coordinates are always in the BASIC screen pixel space of the current screen mode. The terminal scales them to the window automatically.
- Commands that are not GL draw calls (`screen`, `palette`, `play`) take effect immediately when received. All draw commands (`line`, `circle`, `pset`, `paint`, `cls`, `get`, `put`) are queued and executed together on the next rendered frame to avoid partial draws.
- The OSC buffer in the terminal is 512 bytes. Individual commands are well within this limit. Use `batch` for sending many commands at once to reduce round-trips, but keep the total batch payload under 512 bytes or split across multiple sequences.
- `GET`/`PUT` sprite IDs are global integers. There is no limit on the number of sprites other than available memory.
- `PLAY` pre-renders the full MML sequence before handing it to the audio system, so very long strings (over ~30 seconds) are rejected.
