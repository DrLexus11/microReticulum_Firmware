#!/usr/bin/env python3
"""Turn a panel framebuffer dump back into a picture.

Iterating on a 128x64 layout otherwise means asking someone to lean over the
bench and describe what changed, which is slow and lossy. Build with
-DNODE_PANEL_DUMP, capture the serial output, and this renders exactly what the
panel is showing.

    python tools/panel_render.py --port /dev/ttyUSB0 --out panel.png

or from a capture already on disk:

    python tools/panel_render.py --file capture.txt --out panel.png

Without Pillow it still prints the frame as text, which is usually enough to
see that a row is one pixel off.

The SSD1306 buffer is column-major in 8-pixel pages: byte n holds x = n % width,
and the eight vertical pixels starting at y = (n // width) * 8, least
significant bit topmost.
"""

import argparse
import re
import sys
import time

FRAME_BEGIN = re.compile(r"\[panel\] (\d+)x(\d+) begin")
FRAME_END = "[panel] end"


def parse_frames(text):
    """Yield (width, height, bytes) for each complete frame in text."""
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        match = FRAME_BEGIN.search(lines[index])
        if match is None:
            index += 1
            continue
        width, height = int(match.group(1)), int(match.group(2))
        payload = []
        index += 1
        while index < len(lines) and FRAME_END not in lines[index]:
            payload.append(lines[index].strip())
            index += 1
        raw = "".join(payload)
        expected = width * height // 8
        if len(raw) >= expected * 2:
            try:
                yield width, height, bytes.fromhex(raw[:expected * 2])
            except ValueError:
                pass
        index += 1


def pixels(width, height, buffer):
    grid = [[0] * width for _ in range(height)]
    for n, byte in enumerate(buffer):
        x = n % width
        page = n // width
        for bit in range(8):
            y = page * 8 + bit
            if y < height and (byte >> bit) & 1:
                grid[y][x] = 1
    return grid


def as_text(grid):
    # Two rows per line with half-blocks: a 128x64 frame then fits a terminal.
    out = []
    for y in range(0, len(grid), 2):
        top = grid[y]
        bottom = grid[y + 1] if y + 1 < len(grid) else [0] * len(top)
        line = []
        for x in range(len(top)):
            t, b = top[x], bottom[x]
            line.append("█" if t and b else "▀" if t else "▄" if b else " ")
        out.append("".join(line))
    return "\n".join(out)


def save_png(grid, path, scale):
    try:
        from PIL import Image
    except ImportError:
        print("Pillow is not installed; skipping %s" % path, file=sys.stderr)
        return False
    height, width = len(grid), len(grid[0])
    image = Image.new("1", (width, height))
    image.putdata([grid[y][x] for y in range(height) for x in range(width)])
    if scale > 1:
        image = image.resize((width * scale, height * scale), Image.NEAREST)
    image.save(path)
    return True


def capture(port, seconds):
    import serial
    with serial.Serial(port, 115200, timeout=0.2) as handle:
        chunks = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            data = handle.read(4096)
            if data:
                chunks.append(data)
    return b"".join(chunks).decode("utf-8", "replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="serial port to capture from")
    source.add_argument("--file", help="a capture already on disk")
    parser.add_argument("--seconds", type=float, default=25.0,
                        help="how long to capture for (default 25)")
    parser.add_argument("--out", help="write a PNG here")
    parser.add_argument("--scale", type=int, default=4,
                        help="PNG pixel scale (default 4)")
    parser.add_argument("--quiet", action="store_true",
                        help="do not print the frame as text")
    args = parser.parse_args()

    if args.port:
        text = capture(args.port, args.seconds)
    else:
        # A with-block, like capture() uses for the serial port: the descriptor
        # is released even if parsing below raises.
        with open(args.file, encoding="utf-8", errors="replace") as handle:
            text = handle.read()

    frames = list(parse_frames(text))
    if not frames:
        raise SystemExit("no complete panel frame found -- is the image built "
                         "with -DNODE_PANEL_DUMP?")

    width, height, buffer = frames[-1]
    grid = pixels(width, height, buffer)
    print("%d frame(s), showing the last: %dx%d" % (len(frames), width, height),
          file=sys.stderr)
    if not args.quiet:
        print(as_text(grid))
    if args.out:
        if save_png(grid, args.out, args.scale):
            print("wrote %s" % args.out, file=sys.stderr)


if __name__ == "__main__":
    main()
