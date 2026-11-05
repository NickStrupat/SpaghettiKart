#!/usr/bin/env python3
"""Draws the art of Game Select's "Z ONLINE" button (the port's own netplay entry), in the style of the game's
"L OPTION" and "R DATA" buttons, and writes it as PNGs.

    tools/make-online-button.py            Writes online_button.png ("Z ONLINE") and leave_button.png ("Z LEAVE",
                                           shown in a netplay room) to assets/textures/menu/.

Then convert each for the game with tools/png-to-texture.py (<name>.png -> <name>.rgba16).
Each is one 58 x 19 picture, the size of the OPTION and DATA ones, which the game draws the same way (folding,
filtering). The look is matched by eye: a bevelled dark key with a bold white letter, and block letters that fade from
yellow to pink. No pixels come from the game's files. Standard library only.
"""
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "textures", "menu")

LIGHT = (213, 213, 213, 255)
RIM = (164, 164, 164, 255)
EDGE = (82, 82, 82, 255)
DARK = (32, 32, 32, 255)
WHITE = (238, 238, 238, 255)
SHADE = (139, 139, 139, 255)
CLEAR = (0, 0, 0, 0)

# Block letters, 11 rows, strokes 2 pixels wide
GLYPHS = {
    "O": [".####.", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##",
          ".####."],
    "N": ["##...##", "###..##", "###..##", "####.##", "####.##", "##.####", "##.####", "##..###", "##..###",
          "##...##", "##...##"],
    "L": ["##....", "##....", "##....", "##....", "##....", "##....", "##....", "##....", "##....", "######",
          "######"],
    "I": ["##"] * 11,
    "E": ["######", "######", "##....", "##....", "#####.", "#####.", "##....", "##....", "##....", "######",
          "######"],
    "Z": ["######", "######", "....##", "...##.", "..##..", ".##...", "##....", "######", "######"],
    "A": [".####.", "##..##", "##..##", "##..##", "######", "######", "##..##", "##..##", "##..##", "##..##",
          "##..##"],
    "V": ["##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", ".####.", ".####.", "..##..",
          "..##.."],
}


def write_png(path, width, height, pixels):
    raw = b"".join(b"\0" + bytes(c for x in range(width) for c in pixels[y][x]) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def key():
    """16 x 18: light bevel top and left, a grey rim, a dark face with a white Z."""
    width, height = 16, 18
    pixels = [[DARK] * width for _ in range(height)]
    for y in range(height):
        for x in range(width):
            if y == 0 or x == 0:
                pixels[y][x] = LIGHT
            elif y == height - 1 or x == width - 1:
                pixels[y][x] = EDGE
            elif y in (1, height - 2) or x in (1, width - 2):
                pixels[y][x] = RIM
    glyph = GLYPHS["Z"]
    left, top = 5, 4
    for row, line in enumerate(glyph):
        for column, cell in enumerate(line):
            if cell == "#":
                pixels[top + row][left + column] = WHITE
                # A shadow on the dark face below and to the left, as on the L and R keys
                below = pixels[top + row + 1][left + column - 1]
                if below == DARK:
                    pixels[top + row + 1][left + column - 1] = SHADE
    return width, height, pixels


def label_color(row, rows):
    """Yellow in the upper half, then towards pink, like the OPTION and DATA letters."""
    if row < rows // 2:
        return (230, 230, 0, 255)
    t = (row - rows // 2) / (rows - 1 - rows // 2)
    return (int(230 - 50 * t), int(230 - 206 * t), int(148 * t), 255)


def button(text):
    """The key on the left, the word right of it: 58 x 19, like the OPTION and DATA pictures."""
    width, height = 58, 19
    pixels = [[CLEAR] * width for _ in range(height)]
    key_width, key_height, key_pixels = key()
    for y in range(key_height):
        pixels[y][:key_width] = key_pixels[y]
    rows = 11
    x, top = key_width + 2, 4
    for c in text:
        glyph = GLYPHS[c]
        for row, line in enumerate(glyph):
            for column, cell in enumerate(line):
                if cell == "#":
                    pixels[top + row][x + column] = label_color(row, rows)
                    # A dark shadow one pixel down and right
                    if pixels[top + row + 1][x + column + 1] == CLEAR:
                        pixels[top + row + 1][x + column + 1] = (90, 0, 90, 255)
        x += len(glyph[0]) + 1
    assert x <= width, f"{text} is too wide"
    return width, height, pixels


def main():
    os.makedirs(OUT, exist_ok=True)
    write_png(os.path.join(OUT, "online_button.png"), *button("ONLINE"))
    write_png(os.path.join(OUT, "leave_button.png"), *button("LEAVE"))


if __name__ == "__main__":
    main()
