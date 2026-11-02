#!/usr/bin/env python3
"""
Names the variables behind a netplay rollback mismatch.

The determinism harness (src/port/netplay/DeterminismHarness.h) writes a trace with SK_DETERMINISM_TRACE.
On a rollback mismatch it lists every byte range where the re-simulated state differs from the original,
as link-time addresses. This script maps those addresses to symbols using nm on the same executable.

Usage: tools/netplay-diff.py <executable> <trace file>
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


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    symbols = load_symbols(sys.argv[1])
    addresses = [address for address, _ in symbols]

    differences = {}
    order = []
    inside = False
    for line in open(sys.argv[2]):
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
