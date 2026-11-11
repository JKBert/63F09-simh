#!/usr/bin/env python3
"""s09_to_elf.py IN.s09 OUT.elf : S-records (S1/S2/S3, A09 output) to an ELF32 big-endian file with one PT_LOAD per run of
contiguous bytes, so that both the RTL emulator (--elf) and this simulator (LOAD) take the same program.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

The entry point is the first address of the first record.  e_flags = 1 (32-bit, as the programs of regress run LDMD #$31).
"""
import struct
import sys


def main(src, dst, off=0):
    mem = {}
    entry = None
    for line in open(src):
        line = line.strip()
        if len(line) < 10 or line[0] != 'S' or line[1] not in '123':
            continue
        n = int(line[2:4], 16)
        alen = {'1': 2, '2': 3, '3': 4}[line[1]]
        addr = int(line[4:4 + 2 * alen], 16) + off
        data = bytes.fromhex(line[4 + 2 * alen:4 + 2 * (n - 1)])
        if entry is None:
            entry = addr
        for i, b in enumerate(data):
            mem[addr + i] = b
    runs = []
    for a in sorted(mem):
        if runs and runs[-1][0] + len(runs[-1][1]) == a:
            runs[-1][1].append(mem[a])
        else:
            runs.append([a, bytearray([mem[a]])])
    phnum = len(runs)
    off = 52 + 32 * phnum
    ph = b''
    body = b''
    for a, d in runs:
        ph += struct.pack('>IIIIIIII', 1, off + len(body), a, a, len(d), len(d), 7, 1)
        body += bytes(d)
    eh = b'\x7fELF\x01\x02\x01' + b'\x00' * 9
    eh += struct.pack('>HHIIIIIHHHHHH', 2, 0xF509, 1, entry or 0, 52, 0, 1, 52, 32, phnum, 40, 0, 0)
    open(dst, 'wb').write(eh + ph + body)
    print('%d segments, %d bytes, entry 0x%X' % (phnum, sum(len(d) for _, d in runs), entry or 0))


main(sys.argv[1], sys.argv[2], int(sys.argv[3], 16) if len(sys.argv) > 3 else 0)
