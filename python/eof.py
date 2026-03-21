#!/usr/bin/env python3
import sys, io, base64, os, termios, select, signal, time, zipfile
from pathlib import Path

SUPPORTED_EXTS = {'.jpg', '.jpeg', '.png', '.gif', '.bmp', '.webp', '.tiff', '.tif'}

# ── Kitty ─────────────────────────────────────────────────────────────────────

def kitty_send(png_data: bytes, row: int = 1, col: int = 1):
    b64 = base64.standard_b64encode(png_data)
    chunks = [b64[i:i+4096] for i in range(0, len(b64), 4096)]
    out = sys.stdout.buffer
    out.write(f"\x1b[{row};{col}H".encode())
    for i, chunk in enumerate(chunks):
        last = (i == len(chunks) - 1)
        m = 0 if last else 1
        hdr = (f"a=T,f=100,m={m}" if i == 0 else f"m={m}").encode()
        out.write(b"\x1b_G" + hdr + b";" + chunk + b"\x1b\\")
    out.flush()

def kitty_clear():
    sys.stdout.buffer.write(b"\x1b_Ga=d,d=A\x1b\\")
    sys.stdout.buffer.flush()

# ── Image → PNG bytes ─────────────────────────────────────────────────────────

def cell_size():
    import fcntl, struct
    try:
        r,c,xp,yp = struct.unpack('HHHH', fcntl.ioctl(1, termios.TIOCGWINSZ, b'\0'*8))
        if r and c and xp and yp:
            return xp, yp, c, r, xp//c, yp//r
    except Exception:
        pass
    c,r = term_size()
    return c*10, r*20, c, r, 10, 20

def to_png(data: bytes):
    from PIL import Image
    img = Image.open(io.BytesIO(data))
    px_w, px_h, tc, tr, cw, ch = cell_size()
    max_h = px_h - ch * 2   # leave 2 rows for status bar
    scale = min(px_w / img.width, max_h / img.height)
    img = img.resize((max(1, int(img.width*scale)), max(1, int(img.height*scale))), Image.LANCZOS)
    if img.mode not in ('RGB', 'RGBA'):
        img = img.convert('RGB')
    buf = io.BytesIO()
    img.save(buf, 'PNG')
    png = buf.getvalue()
    # Center: offset in cells
    col = max(1, (tc - img.width  // cw) // 2 + 1)
    row = max(1, (tr - 2 - img.height // ch) // 2 + 1)
    return png, row, col

# ── Image source ──────────────────────────────────────────────────────────────

class ImageSource:
    def __init__(self, paths):
        self.entries = []  # (label, loader_fn)
        for p in map(Path, paths):
            if not p.exists():
                print(f"Warning: {p} not found", file=sys.stderr); continue
            if p.suffix.lower() == '.zip':
                with zipfile.ZipFile(p) as zf:
                    for n in sorted(zf.namelist()):
                        if Path(n).suffix.lower() in SUPPORTED_EXTS and not n.startswith('__MACOSX'):
                            self.entries.append((f"{p.name}::{n}", (p, n)))
            else:
                self.entries.append((str(p), p))
        if not self.entries:
            raise ValueError("No images found.")

    def load_png(self, i):
        _, src = self.entries[i]
        if isinstance(src, Path):
            return to_png(src.read_bytes())
        zp, name = src
        with zipfile.ZipFile(zp) as zf:
            return to_png(zf.read(name))

    def label(self, i): return self.entries[i][0]
    def __len__(self): return len(self.entries)

# ── Terminal ──────────────────────────────────────────────────────────────────

def term_size():
    import shutil; s = shutil.get_terminal_size((80,24)); return s.columns, s.lines

def raw_mode(fd):
    old = termios.tcgetattr(fd)
    new = termios.tcgetattr(fd)
    new[3] &= ~(termios.ICANON | termios.ECHO | termios.ISIG)
    new[0] &= ~(termios.IXON | termios.ICRNL)
    new[6][termios.VMIN]  = 0   # non-blocking like the C++ code
    new[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSAFLUSH, new)
    return old

def read_key(fd) -> str:
    """Block until a keypress using select, then read the full sequence."""
    select.select([fd], [], [])  # block until data available
    buf = b''
    # Read available bytes non-blocking
    while True:
        r, _, _ = select.select([fd], [], [], 0.02)
        if not r:
            break
        buf += os.read(fd, 32)
        # If we got an APC response, keep reading until ESC\ terminator
        if b'\x1b_' in buf and not buf.endswith(b'\x1b\\'):
            continue
        break

    if not buf:
        return 'NONE'

    # Swallow Kitty APC responses entirely
    if b'\x1b_' in buf:
        # Strip out all APC sequences, check what's left
        import re
        cleaned = re.sub(rb'\x1b_[^\x1b]*\x1b\\', b'', buf)
        if not cleaned:
            return 'NONE'
        buf = cleaned

    if buf == b'\x1b[D': return 'LEFT'
    if buf == b'\x1b[C': return 'RIGHT'
    if buf == b'\x1b[A': return 'UP'
    if buf == b'\x1b[B': return 'DOWN'
    if buf in (b'\x1b[1~', b'\x1b[H'): return 'HOME'
    if buf in (b'\x1b[4~', b'\x1b[F'): return 'END'
    if buf == b'\x1b': return 'ESC'
    if buf[0:1] == b'\x1b': return 'IGNORE'
    return buf.decode('latin-1', errors='replace')[0]

# ── Display ───────────────────────────────────────────────────────────────────

def show(source, idx):
    cols, rows = term_size()
    kitty_clear()
    # Fill terminal with black background
    sys.stdout.write("\x1b[2J\x1b[H")
    sys.stdout.write("\x1b[40m")  # black background
    blank_row = " " * cols
    for r in range(rows - 1):
        sys.stdout.write(blank_row + ("\n" if r < rows - 2 else ""))
    sys.stdout.write("\x1b[0m")
    sys.stdout.flush()
    png, row, col = source.load_png(idx)
    kitty_send(png, row, col)
    # status bar
    label = os.path.basename(source.label(idx))
    counter = f" [{idx+1}/{len(source)}] "
    hint = "← → navigate  space=next  q=quit  g/G=first/last"
    maxl = max(0, cols - len(counter) - len(hint) - 2)
    if len(label) > maxl: label = "…" + label[-(maxl-1):]
    left = counter + label
    bar = left + " " * max(0, cols - len(left) - len(hint)) + hint
    sys.stdout.write(f"\x1b[{rows};1H\x1b[2K\x1b[7m{bar[:cols]}\x1b[0m")
    sys.stdout.flush()

# ── Main ──────────────────────────────────────────────────────────────────────

def run(paths):
    source = ImageSource(paths)
    fd = sys.stdin.fileno()
    old = raw_mode(fd)
    sys.stdout.write("\x1b[?25l"); sys.stdout.flush()

    def cleanup():
        termios.tcsetattr(fd, termios.TCSAFLUSH, old)
        kitty_clear()
        sys.stdout.write("\x1b[2J\x1b[H\x1b[?25h"); sys.stdout.flush()

    signal.signal(signal.SIGINT, lambda *_: (cleanup(), sys.exit(0)))

    try:
        state = [0]  # mutable so SIGWINCH lambda sees current value
        show(source, state[0])
        signal.signal(signal.SIGWINCH, lambda *_: show(source, state[0]))
        while True:
            key = read_key(fd)
            if key in ('q', 'Q', '\x03'): break
            if key in ('IGNORE', 'NONE'): continue
            if key == 'ESC': break
            prev = state[0]
            if key in ('RIGHT', 'n', ' '): state[0] = (state[0]+1) % len(source)
            elif key in ('LEFT', 'p'):     state[0] = (state[0]-1) % len(source)
            elif key == 'g':               state[0] = 0
            elif key in ('G', 'END'):      state[0] = len(source)-1
            elif key == 'HOME':            state[0] = 0
            if state[0] != prev:
                show(source, state[0])
    finally:
        cleanup()

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(f"Usage: {sys.argv[0]} <image|zip> [...]")
        sys.exit(1)
    run(args)

if __name__ == '__main__':
    main()
