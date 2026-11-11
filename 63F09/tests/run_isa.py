#!/usr/bin/env python3
"""run_isa.py N... : the ISA oracle of regress (gen_isa.py, asm/isaN.asm) on the RTL emulator and on this simulator.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Each case of an isaN program executes ONE instruction from a known state and writes 96 bytes of result (registers, CC,
two memory windows) at RES = $FFFC0400 + 96 * case.  The program is assembled with A09, converted to ELF (tools/s09_to_elf.py),
run by soc63f09 (the reference: the RTL itself) and by BIN/m63f09 ; the two result areas are compared case by case.
usage: run_isa.py [-v] [N...]      (N = part number 1..13, default all)
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
SIM = os.path.join(ROOT, 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
EMU = os.path.expanduser('~/cvs/gcc/github/63F09-emulator/build/soc63f09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
ASM = os.path.expanduser('~/vhdl/63F09/regress/asm')
RES = 0xFFFFC0400
verbose = '-v' in sys.argv
parts = [int(a) for a in sys.argv[1:] if a.isdigit()] or list(range(1, 14))


def sh(cmd, timeout=120, **kw):
    # a hard limit: a simulation that does not stop (empty ROM, runaway loop) must never stay behind
    try:
        return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=timeout, **kw)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(cmd, 124, (e.stdout or b'').decode() if isinstance(e.stdout, bytes) else (e.stdout or ''), 'TIMEOUT')


tot = bad = 0
for p in parts:
    src = '%s/isa%d.asm' % (ASM, p)
    labels = re.findall(r'^\* case (\d+) : (.*?)  \(cc=', open(src).read(), flags=re.M)
    ncase = len(labels)
    tmp = tempfile.mkdtemp()
    sh([A09, '-oF09', '-S', os.path.abspath(src)], cwd=tmp)
    s09 = os.path.join(tmp, 'isa%d.s09' % p)
    if not os.path.exists(s09):
        sh(['cp', src, tmp]); sh([A09, '-oF09', '-S', 'isa%d.asm' % p], cwd=tmp)
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, s09, elf])
    size = 96 * ncase
    r = sh([EMU, '--ncpus', '1', '--elf', elf, '--ticks', '%d' % (3000000 * ncase + 20000000), '--peek', '0x%X:%d' % (RES, size)])
    ref = bytearray()
    for line in r.stdout.splitlines() + r.stderr.splitlines():
        m = re.match(r'([0-9A-F]{9}): ((?:[0-9A-F-]{2} ?)+)', line)
        if m and int(m.group(1), 16) >= RES:
            ref += bytes(0 if t == '--' else int(t, 16) for t in m.group(2).split())
    ref = bytes(ref[:size])
    ini = os.path.join(tmp, 's.ini')
    out = os.path.join(tmp, 'sim.bin')
    open(ini, 'w').write('load %s\nboot cpu\nset cpu disasm=63f09\ngo\ndump %s %x-%x\n' % (elf, out, RES, RES + size - 1))
    rs = sh([SIM, ini], timeout=60)
    if rs.returncode == 124:
        print('isa%-2d TIMEOUT of the simulator (it did not stop)' % p)
    got = open(out, 'rb').read() if os.path.exists(out) else b''
    stopmsg = [l for l in rs.stdout.splitlines() if 'Not implemented' in l or 'Nonexistent' in l or 'Branch to self' in l]
    if len(ref) != size or len(got) != size or not any(ref) or not any(got):
        # a result area that is missing or all zero would compare equal to another one: it is a failure of the run itself
        print('isa%-2d RUN FAILED: reference %d bytes (%s), simulator %d bytes (%s), %d expected'
              % (p, len(ref), 'non-zero' if any(ref) else 'all zero', len(got), 'non-zero' if any(got) else 'all zero', size))
        bad += ncase
        tot += ncase
        continue
    nbad = 0
    for k in range(ncase):
        a, b = ref[96 * k:96 * k + 96], got[96 * k:96 * k + 96]
        tot += 1
        if a != b:
            nbad += 1
            bad += 1
            if verbose:
                print('  case %d %s: differs at bytes %s' % (k, labels[k][1], [i for i in range(96) if i >= len(b) or a[i] != b[i]][:12]))
    print('isa%-2d %3d cases, %3d differ   %s' % (p, ncase, nbad, stopmsg[0][:90] if stopmsg else ''))
print('%d cases, %d differ' % (tot, bad))
