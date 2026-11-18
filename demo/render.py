#!/usr/bin/env python3
"""Render an asciicast v2 file from record.py as an animated GIF.

A minimal terminal: printable text, CR/LF, line wrap, scrolling and SGR
colors, which is all record.py produces.  Needs Pillow.

    demo/render.py demo/demo.cast docs/assets/demo.gif
"""

import argparse
import json
import re

from PIL import Image, ImageDraw, ImageFont

# Catppuccin Mocha
BG = (30, 30, 46)
FG = (205, 214, 244)
BAR = (24, 24, 37)
ANSI = {
    30: (69, 71, 90), 31: (243, 139, 168), 32: (166, 227, 161), 33: (249, 226, 175),
    34: (137, 180, 250), 35: (245, 194, 231), 36: (148, 226, 213), 37: (186, 194, 222),
    90: (108, 112, 134),
}
DOTS = [(243, 139, 168), (249, 226, 175), (166, 227, 161)]

SGR = re.compile(r"\x1b\[([0-9;]*)m")


class Screen:
    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.lines = [[] for _ in range(rows)]
        self.x = self.y = 0
        self.style = (FG, False)

    def newline(self):
        self.y += 1
        if self.y == self.rows:
            self.lines = self.lines[1:] + [[]]
            self.y -= 1

    def put(self, ch):
        if self.x == self.cols:
            self.x = 0
            self.newline()
        line = self.lines[self.y]
        line.extend([(" ", self.style)] * (self.x + 1 - len(line)))
        line[self.x] = (ch, self.style)
        self.x += 1

    def sgr(self, params):
        color, bold = self.style
        for p in [int(v) for v in params.split(";") if v] or [0]:
            if p == 0:
                color, bold = FG, False
            elif p == 1:
                bold = True
            elif p in ANSI:
                color = ANSI[p]
        self.style = (color, bold)

    def feed(self, data):
        pos = 0
        while pos < len(data):
            m = SGR.match(data, pos)
            if m:
                self.sgr(m.group(1))
                pos = m.end()
                continue
            ch = data[pos]
            if ch == "\r":
                self.x = 0
            elif ch == "\n":
                self.newline()
            elif ch >= " ":
                self.put(ch)
            pos += 1

    def snapshot(self):
        return tuple(tuple(line) for line in self.lines), (self.x, self.y)


class Painter:
    def __init__(self, cols, rows, font, size, pad):
        self.regular = ImageFont.truetype(font, size)
        self.bold = ImageFont.truetype(font.replace(".ttf", "-Bold.ttf"), size)
        left, top, right, bottom = self.regular.getbbox("M" * 10)
        self.cw = (right - left) / 10
        self.lh = round(size * 1.35)
        self.pad, self.bar = pad, round(size * 2.2)
        self.width = round(pad * 2 + cols * self.cw)
        self.height = self.bar + pad * 2 + rows * self.lh
        self.cache = {}

    def line(self, cells):
        img = self.cache.get(cells)
        if img is None:
            img = Image.new("RGB", (self.width - 2 * self.pad, self.lh), BG)
            d = ImageDraw.Draw(img)
            for i, (ch, (color, bold)) in enumerate(cells):
                if ch != " ":
                    d.text((i * self.cw, self.lh / 2), ch, fill=color,
                           font=self.bold if bold else self.regular, anchor="lm")
            self.cache[cells] = img
        return img

    def frame(self, snap, cursor_on=True):
        lines, (cx, cy) = snap
        img = Image.new("RGB", (self.width, self.height), BG)
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, self.width, self.bar], fill=BAR)
        r = self.bar // 7
        for i, color in enumerate(DOTS):
            x = self.pad + r + i * r * 3.4
            d.ellipse([x - r, self.bar / 2 - r, x + r, self.bar / 2 + r], fill=color)
        top = self.bar + self.pad
        for y, cells in enumerate(lines):
            if cells:
                img.paste(self.line(cells), (self.pad, top + y * self.lh))
        if cursor_on:
            x0 = self.pad + cx * self.cw
            y0 = top + cy * self.lh
            d.rectangle([x0, y0 + 2, x0 + self.cw - 1, y0 + self.lh - 3], fill=ANSI[37])
        return img


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cast")
    ap.add_argument("gif")
    ap.add_argument("--font", default="/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")
    ap.add_argument("--font-size", type=int, default=18)
    ap.add_argument("--hold", type=float, default=5.0, help="seconds on the last frame")
    args = ap.parse_args()

    with open(args.cast) as f:
        header = json.loads(f.readline())
        events = [json.loads(line) for line in f if line.strip()]

    screen = Screen(header["width"], header["height"])
    painter = Painter(header["width"], header["height"], args.font, args.font_size, 24)

    # Group events into frames with GIF-friendly (10 ms) durations.
    frames, times = [], []
    for t, kind, data in events:
        if kind != "o":
            continue
        screen.feed(data)
        snap = screen.snapshot()
        if frames and frames[-1] == snap:
            continue
        if times and t - times[-1] < 0.02:
            frames[-1] = snap
            continue
        frames.append(snap)
        times.append(t)

    durations = [max(20, round((b - a) * 1000 / 10) * 10) for a, b in zip(times, times[1:])]
    durations.append(round(args.hold * 1000))

    images = [painter.frame(s) for s in frames]
    # One palette for all frames, from the busiest frame, so colors do not
    # flicker; a swatch of the theme colors keeps rare ones (the red dot) exact.
    sample = images[-1].copy()
    theme = [BG, FG, BAR] + list(ANSI.values()) + DOTS
    for i, color in enumerate(theme):
        sample.paste(color, (i * 40, 0, i * 40 + 40, 40))
    palette = sample.quantize(colors=96, method=Image.Quantize.MEDIANCUT)
    out = [im.quantize(palette=palette, dither=Image.Dither.NONE) for im in images]
    out[0].save(args.gif, save_all=True, append_images=out[1:], duration=durations,
                loop=0, optimize=False, disposal=1)


if __name__ == "__main__":
    main()
