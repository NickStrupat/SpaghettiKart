#!/usr/bin/env python3
"""Names the simulation's data segment in a WebAssembly object file (browser build, docs/WEB.md).

    python3 tools/web/sim-segments.py <object.o> <segment name>
    python3 tools/web/sim-segments.py --compile <compiler command...>

The second form is a compiler launcher (cmake/web/WebTarget.cmake): it runs the command, then names the segment in
its output when the file is a simulation file, sim_data for C and sim_cxx_data for C++.

Simulation files put their mutable globals in named sections with "#pragma clang section" (src/port/netplay/
SimSection.h), so that one copy saves them all. Clang's WebAssembly backend keeps the globals together but writes the
section with an empty name, so the linker neither groups them with other files' nor defines the __start_ and __stop_
symbols that mark where they are. This gives that segment its name. An explicit __attribute__((section)) works, but
would be needed on every global.
"""

import os
import subprocess
import sys

WASM_MAGIC = b"\0asm\x01\0\0\0"
CUSTOM_SECTION = 0
LINKING_SUBSECTION_SEGMENT_INFO = 5


def read_uleb(data, pos):
    result = 0
    shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return result, pos


def write_uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def read_name(data, pos):
    length, pos = read_uleb(data, pos)
    return data[pos:pos + length].decode(), pos + length


def write_name(name):
    encoded = name.encode()
    return write_uleb(len(encoded)) + encoded


def rename_segments(segment_info, new_name):
    """Returns the SEGMENT_INFO payload with every unnamed segment renamed, and how many there were."""
    count, pos = read_uleb(segment_info, 0)
    out = bytearray(write_uleb(count))
    renamed = 0
    for _ in range(count):
        name, pos = read_name(segment_info, pos)
        alignment, pos = read_uleb(segment_info, pos)
        flags, pos = read_uleb(segment_info, pos)
        if name == "":
            name = new_name
            renamed += 1
        out += write_name(name) + write_uleb(alignment) + write_uleb(flags)
    return bytes(out), renamed


def rewrite_linking(payload, new_name):
    """The "linking" custom section's payload after its name, with the segments renamed."""
    version, pos = read_uleb(payload, 0)
    out = bytearray(write_uleb(version))
    renamed = 0
    while pos < len(payload):
        kind = payload[pos]
        size, start = read_uleb(payload, pos + 1)
        body = payload[start:start + size]
        if kind == LINKING_SUBSECTION_SEGMENT_INFO:
            body, renamed = rename_segments(body, new_name)
        out += bytes([kind]) + write_uleb(len(body)) + body
        pos = start + size
    return bytes(out), renamed


def name_segment(path, new_name):
    with open(path, "rb") as file:
        data = file.read()
    if not data.startswith(WASM_MAGIC):
        sys.exit(f"{path}: not a WebAssembly object file")

    out = bytearray(data[:len(WASM_MAGIC)])
    pos = len(WASM_MAGIC)
    renamed = 0
    while pos < len(data):
        kind = data[pos]
        size, start = read_uleb(data, pos + 1)
        section = data[start:start + size]
        if kind == CUSTOM_SECTION:
            name, name_end = read_name(section, 0)
            if name == "linking":
                payload, renamed = rewrite_linking(section[name_end:], new_name)
                section = section[:name_end] + payload
        out += bytes([kind]) + write_uleb(len(section)) + section
        pos = start + size
    if renamed > 1:
        sys.exit(f"{path}: {renamed} unnamed segments, expected at most one")
    if renamed:
        with open(path, "wb") as file:
            file.write(out)


def compile_and_name(command):
    result = subprocess.run(command).returncode
    # CMake force-includes SimSection.h into the simulation files only (CMakeLists.txt).
    if result != 0 or not any(argument.endswith("SimSection.h") for argument in command) or "-o" not in command:
        return result
    output = command[command.index("-o") + 1]
    is_cxx = os.path.basename(command[0]).endswith("++")
    name_segment(output, "sim_cxx_data" if is_cxx else "sim_data")
    return 0


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--compile":
        sys.exit(compile_and_name(sys.argv[2:]))
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    name_segment(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
