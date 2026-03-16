# Felix Terminal

A standalone OpenGL terminal emulator for Linux and MS Windows. Renders text using FreeType triangles via OpenGL 3.3 — no GTK, no Qt, no desktop toolkit dependency.

## Features

- VT100/VT220/xterm-compatible emulation
- Full 256-color and 24-bit RGB color support
- Font variants: Regular, Bold, Italic, Bold Italic (DejaVu Sans Mono)
- Text attributes: bold, italic, underline, strikethrough, overline, dim, blink, reverse
- Emoji rendering via embedded NotoEmoji (fallback for non-ASCII glyphs)
- Scrollback buffer (5000 lines)
- Mouse reporting (X10 and SGR encoding)
- Bracketed paste mode
- Alternate screen buffer (used by vim, less, etc.)
- Configurable themes and window opacity
- Spawn additional terminal windows
- Works with /bin/bash, cmd.exe, powershell, and many more.
- Kitty Graphics Support (static, no animation, Linux only) e.g. timg icon.png

## Dependencies

- SDL2
- OpenGL / GLEW
- FreeType2

On Fedora/RHEL:
```
sudo dnf install SDL2-devel glew-devel freetype-devel
```

On Debian/Ubuntu:
```
sudo apt install libsdl2-dev libglew-dev libfreetype-dev
```

## Building

```
make
```

or for Windows (requires mingw)

```
make windows
```


The fonts are embedded as base64-encoded headers — no external font files required.

## Usage

```
./gl_terminal
```

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+C` (with selection) | Copy selection |
| `Ctrl+SHIFT+C` (with selection) | Copy selection as HTML |
| `Ctrl+V` | Paste |
| `Ctrl+Shift+V` | Paste |
| `Ctrl+Scroll Up/Down` | Increase / decrease font size |
| `Ctrl+Shift+Scroll` | Increase / decrease font size (4× step) |
| `F11` | Toggle Full Screen |


## Mouse

| Action | Behaviour |
|---|---|
| Left click + drag | Select text |
| Release after drag | Auto-copy selection to clipboard |
| Middle click | Paste clipboard |
| Right click | Open context menu |
| Scroll wheel | Scroll scrollback history |
| `Ctrl` + scroll | Resize font |

## Context Menu (Right Click)

- **New Terminal** — spawn a new window
- **Copy** — copy selection as plain text
- **Copy as HTML** — copy with color and style markup
- **Copy as ANSI** — copy with ANSI escape codes
- **Paste**
- **Reset** — clear screen and reset cursor
- **Themes** — submenu to switch color themes
- **Opacity** — submenu to set window transparency
- **Sound** - Enables or Disables Sound
- **Fight Mode** -  Have two guys fight in your console
- **Bouncing Cirle** -  A circle with a little ball that bounces around in your console
- **Quit**

## Themes

| Theme | Description |
|---|---|
| Default | Dark blue-black background |
| Solarized Dark | Ethan Schoonover's Solarized palette |
| Monokai | Classic Monokai |
| Nord | Arctic, north-bluish palette |
| Gruvbox | Retro groove palette |
| Matrix | Black background, green text |
| Ocean | Deep blue background |

## Configuration

Defaults are defined at the top of `gl_terminal.h`:

```c
#define TERM_COLS_DEFAULT  80
#define TERM_ROWS_DEFAULT  24
#define SCROLLBACK_LINES   5000
#define FONT_SIZE_DEFAULT  16
#define FONT_SIZE_MIN      6
#define FONT_SIZE_MAX      72
```

## Embedded Fonts

| File | Font |
|---|---|
| `DejaVuMono.h` | DejaVu Sans Mono Regular |
| `DejaVuMonoBold.h` | DejaVu Sans Mono Bold |
| `DejaVuMonoOblique.h` | DejaVu Sans Mono Oblique |
| `DejaVuMonoBoldOblique.h` | DejaVu Sans Mono Bold Oblique |
| `NotoEmoji.h` | Noto Emoji (grayscale fallback) |

To regenerate a font header, base64-encode the TTF and wrap it with the expected macro name and size constant (see any existing `.h` for the format).
