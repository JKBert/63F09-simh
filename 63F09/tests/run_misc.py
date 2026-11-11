#!/usr/bin/env python3
"""run_misc.py [-v] : LDDP LDDS STDP STDS SEXV ENI DII LOCK UNLOCK on soc63f09 and on this simulator.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Each case sets DS = $FFFC and DP = $03, a pattern in the registers, the CC (ANDCC/ORCC), runs ONE instruction and saves, in a 128-byte slot
at $FFFC0400 + 128 * case: CC (PSHS CC first), D W V X Y U, DP and DS, and the memory windows.  The two areas are compared case by case.
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
EMU = os.path.expanduser('~/cvs/gcc/github/63F09-emulator/build/soc63f09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
RES = 0xFFFFC0400
STRIDE = 128
verbose = '-v' in sys.argv

cases = []      # (label, setup lines, instruction line)
CC = (0x00, 0x0F, 0x2A)
for cc in CC:
    for v in (0x00, 0x55, 0x80, 0xFF):
        cases.append(('lddp #$%02X cc=%02X' % (v, cc), [], 'lddp\t#$%02X' % v, cc))
        for f in ('<$08', '8,x', '>$FFFC0308'):
            cases.append(('lddp %s mem=%02X cc=%02X' % (f, v, cc), ['\t\tLDA\t\t#$%02X' % v, '\t\tSTA\t\t8,X'], 'lddp\t%s' % f, cc))
    for v in (0x0000, 0x5AA5, 0x8000, 0xFFFF):
        cases.append(('ldds #$%04X cc=%02X' % (v, cc), [], 'ldds\t#$%04X' % v, cc))
        for f in ('<$08', '8,x', '>$FFFC0308'):
            cases.append(('ldds %s mem=%04X cc=%02X' % (f, v, cc), ['\t\tLDD\t\t#$%04X' % v, '\t\tSTD\t\t8,X'], 'ldds\t%s' % f, cc))
    cases.append(('stdp <$08 cc=%02X' % cc, [], 'stdp\t<$08', cc))
    cases.append(('stds <$08 cc=%02X' % cc, [], 'stds\t<$08', cc))
    for dp in (0x00, 0x80, 0xFF):
        for f in ('8,x', '>$FFFC0308'):
            cases.append(('stdp %s dp=%02X cc=%02X' % (f, dp, cc), ['\t\tLDA\t\t#$%02X' % dp, '\t\tTFR\t\tA,DP'], 'stdp\t%s' % f, cc))
    for ds in (0x0000, 0x8000, 0xFFFC):
        for f in ('8,x', '>$FFFC0308'):
            cases.append(('stds %s ds=%04X cc=%02X' % (f, ds, cc), ['\t\tLDD\t\t#$%04X' % ds, '\t\tTFR\t\tD,DS'], 'stds\t%s' % f, cc))
    for v in (0x00000000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0x0000FFFF):
        cases.append(('sexv V=%08X cc=%02X' % (v, cc), ['\t\tLDV\t\t#$%08X' % v], 'sexv', cc))
    for i in ('eni', 'dii', 'lock', 'unlock'):
        cases.append(('%s cc=%02X' % (i, cc), [], i, cc))
for cc in (0xFF, 0x50, 0x10, 0x40):
    for i in ('eni', 'dii'):
        cases.append(('%s cc=%02X' % (i, cc), [], i, cc))


def build(sub):
    L = ['\t\tORG\t\t$FFFF0010', '\t\tLDMD\t#$31', '\t\tLDS\t\t#$FFFD0000']
    for n, (label, setup, ins, cc) in enumerate(sub):
        L.append('* case %d : %s' % (n, label))
        L += ['\t\tLDD\t\t#$FFFC', '\t\tTFR\t\tD,DS', '\t\tLDA\t\t#$03', '\t\tTFR\t\tA,DP',
              '\t\tLDX\t\t#$FFFC0300', '\t\tLDY\t\t#$FFFC0320', '\t\tLDU\t\t#$FFFC0360', '\t\tLDQ\t\t#$A55A3C96', '\t\tLDV\t\t#$0F1E2D3C']
        L += ['\t\tLDQ\t\t#$C3A5961E', '\t\tSTQ\t\t8,X', '\t\tLDQ\t\t#$A55A3C96']
        L += setup
        L += ['\t\tANDCC\t#$00', '\t\tORCC\t#$%02X' % cc, '\t\t%s' % ins]
        base = 'RES+%d' % (STRIDE * n)
        L += ['\t\tPSHS\tCC', '\t\tSTD\t\t>%s' % base, '\t\tSTW\t\t>%s+2' % base, '\t\tSTV\t\t>%s+4' % base,
              '\t\tSTX\t\t>%s+8' % base, '\t\tSTY\t\t>%s+12' % base, '\t\tSTU\t\t>%s+16' % base,
              '\t\tLDA\t\t,S+', '\t\tSTA\t\t>%s+20' % base,
              '\t\tTFR\t\tDP,A', '\t\tSTA\t\t>%s+21' % base, '\t\tTFR\t\tDS,D', '\t\tSTD\t\t>%s+22' % base]
        for k in range(2):
            L += ['\t\tLDQ\t\t>$FFFC%04X' % (0x0308 + 4 * k), '\t\tSTQ\t\t>%s+%d' % (base, 24 + 4 * k)]
    L += ['END:', '\t\tBRA\t\tEND', '', 'RES\t\tEQU\t\t$FFFC0400', '', '\t\tORG\t\t$FFFFFFF0'] + ['\t\tFDB\t\t$0000'] * 7 + ['\t\tFDB\t\t$0010', '', '\t\tEND']
    return L


def sh(cmd, timeout=300, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, '', 'TIMEOUT')


PART = 60
tot = bad = 0
for p in range((len(cases) + PART - 1) // PART):
    sub = cases[p * PART:(p + 1) * PART]
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'misc.asm'), 'w').write('\n'.join(build(sub)) + '\n')
    sh([A09, '-oF09', '-S', 'misc.asm'], cwd=tmp)
    if not os.path.exists(os.path.join(tmp, 'misc.s09')):
        print('part %d: a09 failed' % (p + 1)); bad += len(sub); tot += len(sub); continue
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, os.path.join(tmp, 'misc.s09'), elf])
    size = STRIDE * len(sub)
    r = sh([EMU, '--ncpus', '1', '--elf', elf, '--ticks', str(3000000 * len(sub) + 20000000), '--peek', '0x%X:%d' % (RES, size)], timeout=900)
    ref = bytearray()
    for line in r.stdout.splitlines() + r.stderr.splitlines():
        m = re.match(r'([0-9A-F]{9}): ((?:[0-9A-F-]{2} ?)+)', line)
        if m and int(m.group(1), 16) >= RES:
            ref += bytes(0 if t == '--' else int(t, 16) for t in m.group(2).split())
    ref = bytes(ref[:size])
    out = os.path.join(tmp, 'sim.bin')
    open(os.path.join(tmp, 's.ini'), 'w').write('load %s\nboot cpu\ngo\ndump %s %x-%x\n' % (elf, out, RES, RES + size - 1))
    rs = sh([SIM, os.path.join(tmp, 's.ini')], timeout=120)
    got = open(out, 'rb').read() if os.path.exists(out) else b''
    stop = [l for l in rs.stdout.splitlines() if 'Not implemented' in l or 'Nonexistent' in l or 'Branch to self' in l]
    if len(ref) != size or len(got) != size or not any(ref) or not any(got):
        print('part %d RUN FAILED: reference %d bytes (%s), simulator %d bytes (%s) %s' % (p + 1, len(ref), 'nz' if any(ref) else 'zero', len(got), 'nz' if any(got) else 'zero', stop[:1]))
        bad += len(sub); tot += len(sub); continue
    nb = 0
    for k, c in enumerate(sub):
        a, b = ref[STRIDE * k:STRIDE * k + 32], got[STRIDE * k:STRIDE * k + 32]
        tot += 1
        if a != b:
            nb += 1; bad += 1
            if verbose:
                d = [i for i in range(32) if a[i] != b[i]]
                print('  %s : bytes %s\n      ref %s\n      sim %s' % (c[0], d[:8], a.hex(), b.hex()))
    print('part %d: %d cases, %d differ  %s' % (p + 1, len(sub), nb, stop[0][:70] if stop else ''))
print('%d cases, %d differ' % (tot, bad))
