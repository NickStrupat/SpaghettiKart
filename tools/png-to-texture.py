#!/usr/bin/env python3
"""Converts a PNG into a libultraship texture resource (RGBA16), for the port's own art in assets/.

    tools/png-to-texture.py <in.png> <out.rgba16>

The game draws it from spaghetti.o2r by path, e.g. "__OTR__textures/menu/online_key.rgba16" for
assets/textures/menu/online_key.rgba16. Reads 8-bit palette, RGB and RGBA PNGs; standard library only.
"""
import struct
import sys
import zlib

RGBA16 = 2
HEADER_SIZE = 0x40


def read_png(path):
    """@returns (width, height, [(r, g, b, a)] row by row)."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    offset, chunks = 8, {}
    idat = b""
    while offset < len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IDAT":
            idat += body
        else:
            chunks[kind] = body
    width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    assert depth == 8 and interlace == 0, "only 8-bit, non-interlaced PNGs"
    channels = {3: 1, 2: 3, 6: 4}[color]
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            up_left = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + up) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                line[x] = (line[x] + predictor) & 0xFF
        rows.append(bytes(line))
        previous = line
    palette = chunks.get(b"PLTE", b"")
    alphas = chunks.get(b"tRNS", b"")
    pixels = []
    for line in rows:
        for x in range(width):
            if color == 3:
                index = line[x]
                r, g, b = palette[index * 3:index * 3 + 3]
                a = alphas[index] if index < len(alphas) else 255
            elif color == 2:
                r, g, b = line[x * 3:x * 3 + 3]
                a = 255
            else:
                r, g, b, a = line[x * 4:x * 4 + 4]
            pixels.append((r, g, b, a))
    return width, height, pixels


def main():
    width, height, pixels = read_png(sys.argv[1])
    texels = b"".join(
        struct.pack(">H", ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if a >= 128 else 0))
        for r, g, b, a in pixels)
    # Resource header: endianness, type "OTEX", version 1, id; then the texture: type, size, flags, scales, data.
    header = struct.pack("<I4sIQ", 0, b"XETO", 1, 0xDEADBEEFDEADBEEF).ljust(HEADER_SIZE, b"\0")
    body = struct.pack("<IIIIffI", RGBA16, width, height, 0, 1.0, 1.0, len(texels))
    open(sys.argv[2], "wb").write(header + body + texels)


if __name__ == "__main__":
    main()
