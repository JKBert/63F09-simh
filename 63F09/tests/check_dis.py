#!/usr/bin/env python3
"""check_dis.py: the simulator's disassembler against objdump on the test programs of binutils-gdb.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

For each test/*.s: assemble (as-new), take the .text bytes (objcopy), load them as a raw binary at the base of the ROM, ask the
simulator for `examine -m` over the range and compare, instruction by instruction, with `objdump -d`: same addresses (so same
lengths) and same mnemonic.  Branch targets and relocated operands are not compared (the .o is not linked).
usage: check_dis.py [binutils-gdb build dir] [file.s ...]
"""
import glob, os, re, subprocess, sys, tempfile

BD = sys.argv[1] if len(sys.argv) > 1 and os.path.isdir(sys.argv[1]) else os.path.expanduser('~/cvs/gcc/binutils-gdb/build')
files = [a for a in sys.argv[1:] if a.endswith('.s')] or sorted(glob.glob(os.path.expanduser('~/cvs/gcc/binutils-gdb/test/*.s')))
SIM = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'BIN', 'm63f09')
AS, OBJDUMP, OBJCOPY = BD + '/gas/as-new', BD + '/binutils/objdump', BD + '/binutils/objcopy'
# (as-new -m option, simulator DISASM) : the file is tried in each state until one assembles it
STATES = [('63f09hf', '63F09HF'), ('63f09', '63F09'), ('6309', '6309'), ('6809', '6809')]
BASE = 0xFFFFF0000


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def check(src):
    for mopt, simarch in STATES:
        tmp = tempfile.mkdtemp()
        obj, binf = tmp + '/x.o', tmp + '/x.bin'
        if run([AS, '-m' + mopt, src, '-o', obj]).returncode:
            continue
        run([OBJCOPY, '-O', 'binary', '-j', '.text', obj, binf])
        size = os.path.getsize(binf) if os.path.exists(binf) else 0
        if size == 0 or size > 0xF000:
            return mopt, 0, 0, ['empty or too large (%d bytes)' % size]
        ref = []
        for line in run([OBJDUMP, '-d', obj]).stdout.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\t(?:[0-9a-f]{2} )+\s*\t(\S+)', line)
            if m:
                ref.append((int(m.group(1), 16), m.group(2).lower()))
        ini = tmp + '/s.ini'
        open(ini, 'w').write('set cpu disasm=%s\nload %s %x\nexamine -m %x-%x\n' % (simarch, binf, BASE, BASE, BASE + size - 1))
        out = run([SIM, ini], stdin=subprocess.DEVNULL).stdout
        # SCP merges consecutive identical lines into "START: thru END: same as above." : expand them with the instruction
        # addresses of the reference (the same text can come from instructions of different lengths)
        rlen = {r[0]: 0 for r in ref}                       # instruction addresses of the reference
        got = []
        for line in out.splitlines():
            m = re.match(r'([0-9A-F]{9}):\t(\S+)', line)
            if m:
                got.append((int(m.group(1), 16) - BASE, m.group(2).lower()))
                continue
            m = re.match(r'([0-9A-F]{9}): thru ([0-9A-F]{9}): same as above', line)
            if m and got:
                a, end, txt = int(m.group(1), 16) - BASE, int(m.group(2), 16) - BASE, got[-1][1]
                got.extend((x, txt) for x in sorted(rlen) if a <= x <= end)
        diffs = []
        for i in range(max(len(ref), len(got))):
            r = ref[i] if i < len(ref) else None
            g = got[i] if i < len(got) else None
            if r is None or g is None or r[0] != g[0] or r[1] != g[1]:
                diffs.append('#%d objdump %s | sim %s' % (i, r, g))
                break
        return mopt, len(ref), len(got), diffs
    return None, 0, 0, ['does not assemble in any state']


total = bad = 0
for f in files:
    mopt, n, ng, diffs = check(f)
    total += 1
    if diffs:
        bad += 1
    print('%-28s -m%-8s %5d insns  %s' % (os.path.basename(f), mopt, n, 'OK' if not diffs else 'DIFF ' + diffs[0]))
print('%d files, %d with a difference' % (total, bad))
