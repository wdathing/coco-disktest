#!/usr/bin/env python3
"""Write disktest's data file: SECTORS sectors of its seed-0 pattern.

usage: mkdata.py OUT [SECTORS]      (default 225: 57,600 bytes, 25 granules)

In file sector s, byte i is a + i*b (mod 256) with a = s*$9D + seed*$35 and
b = 2*(s XOR seed) + 1, all mod 256 (see src/disktest.c).
"""
import sys


def sector(s, seed=0):
    s &= 0xFF
    a = (s * 0x9D + seed * 0x35) & 0xFF
    b = (((s ^ seed) << 1) | 1) & 0xFF
    return bytes((a + i * b) & 0xFF for i in range(256))


def main():
    out = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 225
    with open(out, "wb") as f:
        for s in range(n):
            f.write(sector(s))


if __name__ == "__main__":
    main()
