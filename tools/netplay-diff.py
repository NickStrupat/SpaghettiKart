#!/usr/bin/env python3
"""
Names the variables behind a netplay rollback mismatch or desync.

The determinism harness (src/port/netplay/DeterminismHarness.h) writes a trace with SK_DETERMINISM_TRACE.
On a rollback mismatch it lists every byte range where the re-simulated state differs from the original,
as link-time addresses. This script maps those addresses to symbols using nm on the same executable.

Netplay peers run with SK_NETPLAY_DUMP=1 write the state each segment starts from; given two of those files,
this script lists where the two peers' states differ.

Usage: tools/netplay-diff.py <executable> <trace file>
       tools/netplay-diff.py <executable> <snapshot file> <snapshot file>
"""
import bisect
import platform
import subprocess
import sys


def load_symbols(executable):
    """Returns sorted (address, name) pairs for data symbols."""
    if platform.system() == "Darwin":
        output = subprocess.run(["xcrun", "nm", "-n", "-m", executable], capture_output=True, text=True).stdout
        lines = [line for line in output.splitlines() if "(__DATA," in line]
    else:
        output = subprocess.run(["nm", "-n", executable], capture_output=True, text=True).stdout
        lines = [line for line in output.splitlines() if len(line.split()) == 3 and line.split()[1] in "bBdD"]

    symbols = []
    for line in lines:
        parts = line.split()
        try:
            symbols.append((int(parts[0], 16), parts[-1]))
        except ValueError:
            continue
    symbols.sort()
    return symbols


def demangle(names):
    output = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout
    return output.splitlines()


def read_snapshot(path):
    """Returns the header fields and the data, bss, arena and world bytes of a snapshot file."""
    with open(path, "rb") as file:
        header = file.readline().split()
        data_address, data_size = int(header[1], 16), int(header[2])
        bss_address, bss_size = int(header[3], 16), int(header[4])
        arena_size, world_size = int(header[5]), int(header[6])
        data = file.read(data_size)
        bss = file.read(bss_size)
        arena = file.read(arena_size)
        world = file.read(world_size)
    return (data_address, data, bss_address, bss, arena, world)


def is_pointer(value):
    """Heap and image addresses differ between processes (ASLR) without the game behaving differently."""
    return 0x100000000 <= value < 0x800000000000


def differing_runs(a, b, base):
    """
    Yields (address, length) for each run of differing bytes; runs fewer than 8 bytes apart are merged.
    Aligned 8-byte words holding a pointer on both sides are skipped.
    """
    size = min(len(a), len(b))
    i = 0
    while i < size:
        if a[i] == b[i]:
            i += 1
            continue
        word = (base + i) & ~7
        start = word - base
        if start >= 0 and start + 8 <= size:
            value_a = int.from_bytes(a[start:start + 8], "little")
            value_b = int.from_bytes(b[start:start + 8], "little")
            if is_pointer(value_a) and is_pointer(value_b):
                i = start + 8
                continue
        end = i
        while end < size and (a[end] != b[end] or a[end:end + 8] != b[end:end + 8]):
            end += 1
        yield (base + i, end - i)
        i = end


def snapshot_lines(path_a, path_b):
    """Compares two snapshot files and returns the differences in the trace file's line format."""
    a = read_snapshot(path_a)
    b = read_snapshot(path_b)
    lines = ["rollback differences between " + path_a + " and " + path_b]
    for address, length in differing_runs(a[1], b[1], a[0]):
        lines.append(f"sim_data {address:x} {length}")
    for address, length in differing_runs(a[3], b[3], a[2]):
        lines.append(f"sim_bss {address:x} {length}")
    if len(a[4]) != len(b[4]):
        lines.append(f"arena size {len(a[4])} vs {len(b[4])}")
    for address, length in differing_runs(a[4], b[4], 0):
        lines.append(f"arena {address:x} {length}")
    for address, length in differing_runs(a[5], b[5], 0):
        lines.append(f"world {address:x} {length}")
    return lines


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(1)

    symbols = load_symbols(sys.argv[1])
    addresses = [address for address, _ in symbols]
    lines = snapshot_lines(sys.argv[2], sys.argv[3]) if len(sys.argv) == 4 else open(sys.argv[2])

    differences = {}
    order = []
    inside = False
    for line in lines:
        if line.startswith("rollback differences"):
            inside = True
            print(line.strip())
            continue
        if line.startswith("rollback original"):
            inside = False
        parts = line.split()
        if not inside or len(parts) != 3:
            continue
        if parts[0] not in ("sim_data", "sim_bss"):
            print(f"  {line.strip()}")
            continue
        address, length = int(parts[1], 16), int(parts[2])
        index = bisect.bisect_right(addresses, address) - 1
        if index < 0:
            continue
        base, name = symbols[index]
        if name not in differences:
            differences[name] = []
            order.append(name)
        differences[name].append((address - base, length))

    # Mach-O prefixes C symbols with "_"; c++filt handles both.
    readable = demangle([name[1:] if name.startswith("__Z") else name for name in order])
    for name, pretty in zip(order, readable):
        runs = differences[name]
        shown = ", ".join(f"+0x{offset:x} ({length} bytes)" for offset, length in runs[:6])
        more = f", ... {len(runs)} runs" if len(runs) > 6 else ""
        print(f"  {pretty}: {shown}{more}")


if __name__ == "__main__":
    main()
