#!/usr/bin/env python3
"""Draws the arrows that netplay room codes are shown with (a code is 7 stick directions, RelayProtocol.h), and writes
them as PNGs.

    tools/make-code-arrows.py              Writes code_arrow_<up, down, left, right>.png (16 x 16, for the ONLINE
                                           screen) and code_arrow_small_<...>.png (10 x 10, for Game Select, where
                                           little room is left) to assets/textures/menu/.

Then convert each for the game with tools/png-to-texture.py (<name>.png -> <name>.rgba16).
Each is a block arrow with a dark outline, filled yellow fading to pink from top to bottom, like the letters of Game
Select's OPTION, DATA and ONLINE buttons (tools/make-online-button.py). Standard library only.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
make_online_button = __import__("make-online-button")

OUT = make_online_button.OUT
OUTLINE = (32, 32, 32, 255)
CLEAR = make_online_button.CLEAR


def up_arrow(size):
    """@returns the set of (x, y) the up arrow fills, inside a 1-pixel margin: a head as wide as that leaves, over a
    shaft a little under half as wide."""
    inner = size - 2
    half_width = inner // 2
    shaft = inner // 2 - inner // 8
    filled = set()
    for row in range(half_width):
        for x in range(half_width - row - 1, half_width + row + 1):
            filled.add((1 + x, 1 + row))
    for row in range(half_width, inner):
        for x in range(half_width - shaft // 2 - shaft % 2, half_width + shaft // 2 + shaft % 2):
            filled.add((1 + x, 1 + row))
    return filled


def rotate(filled, size, turns):
    """Turns the shape a quarter clockwise, this many times."""
    for _ in range(turns):
        filled = {(size - 1 - y, x) for x, y in filled}
    return filled


def draw(filled, size):
    pixels = [[CLEAR] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            if (x, y) in filled:
                pixels[y][x] = make_online_button.label_color(y - 1, size - 2)
            elif any((x + dx, y + dy) in filled for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                pixels[y][x] = OUTLINE
    return pixels


def main():
    os.makedirs(OUT, exist_ok=True)
    for prefix, size in (("code_arrow_", 16), ("code_arrow_small_", 10)):
        for name, turns in (("up", 0), ("right", 1), ("down", 2), ("left", 3)):
            path = os.path.join(OUT, f"{prefix}{name}.png")
            make_online_button.write_png(path, size, size, draw(rotate(up_arrow(size), size, turns), size))
            print(path)


if __name__ == "__main__":
    main()
