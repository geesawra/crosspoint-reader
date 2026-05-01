#!/usr/bin/env python3
"""Decode ESP32 (RISC-V) crash log via addr2line."""

import sys
import re
import subprocess


def run_addr2line(addr2line_bin, elf, addr):
    try:
        result = subprocess.run(
            [addr2line_bin, "-e", elf, "-f", "-C", addr],
            capture_output=True, text=True, timeout=10
        )
        lines = result.stdout.strip().splitlines()
        func = lines[0] if lines else "??"
        loc = lines[1] if len(lines) > 1 else "??"
        return func, loc
    except Exception as e:
        return f"error: {e}", "??"


def is_likely_code(addr):
    if addr == 0:
        return False
    # ESP32-C3/C6 code regions: ROM / SRAM instructions / flash cache
    if 0x40000000 <= addr < 0x44000000:
        return True
    return False


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <elf-file> <addr2line-binary>", file=sys.stderr)
        sys.exit(1)

    elf = sys.argv[1]
    addr2line = sys.argv[2]
    log = sys.stdin.read()

    items = []  # (label, addr_str)

    # Extract assert info if present
    assert_match = re.search(r'assert failed:\s+(\S+)\s+(.+?)\s*\(', log)
    if assert_match:
        func, path = assert_match.groups()
        print(f"Assert failed in: {func}")
        print(f"Location: {path}")
        print()

    # 1. Parse registers: "  MEPC    : 0x403871d6"
    reg_re = re.compile(r'^\s*(\w+)\s+:\s+(0x[0-9a-fA-F]+)')
    for line in log.splitlines():
        m = reg_re.match(line)
        if m:
            label, addr_str = m.groups()
            if is_likely_code(int(addr_str, 16)):
                items.append((label, addr_str))

    # 2. Parse stack memory: "3fcb2000: 0x3fcb200c 0x00000000 ..."
    stack_line_re = re.compile(r'^\s*([0-9a-fA-F]{8}):\s+(.*)')
    word_re = re.compile(r'0x[0-9a-fA-F]{8}')
    for line in log.splitlines():
        m = stack_line_re.match(line)
        if not m:
            continue
        base_str, rest = m.groups()
        base = int(base_str, 16)
        words = word_re.findall(rest)
        for i, w in enumerate(words):
            addr = int(w, 16)
            if is_likely_code(addr):
                label = f"mem[0x{base + i*4:08x}]"
                items.append((label, w))

    # Deduplicate while preserving order
    seen = set()
    unique = [(l, a) for l, a in items if not (a in seen or seen.add(a))]

    if not unique:
        print("No code addresses found to decode.")
        return

    print(f"{'Address':>12} {'Function':<50} {'Location':<30} {'Source'}")
    print("-" * 110)
    for label, addr in unique:
        func, loc = run_addr2line(addr2line, elf, addr)
        print(f"{addr} {func:<50} {loc:<30} ({label})")


if __name__ == "__main__":
    main()
