#!/usr/bin/env python3
"""run_wide.py [-v] : MULDU MULQ MULQU MULO MULOU DIVDU DIVQU DIVO DIVOU (63F09 only) on soc63f09 and on this simulator.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Same principle as run_isa.py: every case sets the registers, the CC (ANDCC/ORCC, so that no register is touched) and an operand in memory,
runs ONE instruction in one of four forms (immediate, direct, indexed, extended), then saves CC (PSHS CC first: the stores change the flags),
D W V X Y U and two memory windows (the operand and the 16 bytes below U, where DIVO and DIVOU push their remainder) in a 128-byte slot at
RES = $FFFC0400 + 128 * case.  The two result areas are compared case by case (the first 64 bytes of each slot).
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

# (mnemonic, operand size in bytes, [(register value as (D, W, V) hex words, operand hex)])
M16, M32, M64 = 2, 4, 8
VAL16 = [((0xA55A, 0, 0), 0x0007), ((0x80FF, 0, 0), 0xF00F), ((0xFFFF, 0, 0), 0x8000), ((0x1234, 0, 0), 0x0001)]
VAL32 = [((0xA55A, 0x3C96, 0), 0x00000007), ((0x8001, 0x7FFF, 0), 0xF00F1234), ((0xFFFF, 0xFFFF, 0), 0xFFFFFFFF), ((0x0123, 0x4567, 0), 0x00010001)]
VAL64 = [((0xA55A, 0x3C96, 0x0F1E2D3C), 0x0000000000000007), ((0x8001, 0x7FFF, 0xFFFFFFFF), 0xF00F123456789ABC),
         ((0xFFFF, 0xFFFF, 0xFFFFFFFF), 0xFFFFFFFFFFFFFFFF), ((0x8000, 0x0000, 0x00000000), 0x8000000000000000),
         ((0x0123, 0x4567, 0x89ABCDEF), 0x0000000100000001)]
OPS = [('muldu', M16, VAL16), ('divdu', M16, VAL16), ('mulq', M32, VAL32), ('mulqu', M32, VAL32), ('divqu', M32, VAL32),
       ('mulo', M64, VAL64), ('mulou', M64, VAL64), ('divo', M64, VAL64), ('divou', M64, VAL64)]
CCS = [0x00, 0x0F]
FORMS = ['#', '<$08', '8,x', '>$FFFC0308']

cases = []
for mn, sz, vals in OPS:
    for regs, opd in vals:
        for cc in CCS:
            for f in FORMS:
                cases.append((mn, sz, regs, opd, cc, f))


def build(sub):
    L = ['\t\tORG\t\t$FFFF0010', '\t\tLDMD\t#$31', '\t\tLDS\t\t#$FFFD0000', '\t\tLDD\t\t#$FFFC', '\t\tTFR\t\tD,DS', '\t\tLDA\t\t#$03', '\t\tTFR\t\tA,DP']
    for n, (mn, sz, regs, opd, cc, f) in enumerate(sub):
        D, W, V = regs
        hexs = '%0*X' % (sz * 2, opd)
        L.append('* case %d : %s %s opd=%s D=%04X W=%04X V=%08X cc=%02X' % (n, mn, f, hexs, D, W, V, cc))
        L += ['\t\tLDX\t\t#$FFFC0300', '\t\tLDY\t\t#$FFFC0320', '\t\tLDU\t\t#$FFFC0360']
        # the operand, big-endian, at X+8 (two STQ for 8 bytes), and a known pattern in the window under U
        full = '%016X' % opd if sz == 8 else ('%08X' % opd if sz == 4 else '%04X%04X' % (opd, 0))
        if sz == 8:
            L += ['\t\tLDQ\t\t#$%s' % full[:8], '\t\tSTQ\t\t8,X', '\t\tLDQ\t\t#$%s' % full[8:], '\t\tSTQ\t\t12,X']
        elif sz == 4:
            L += ['\t\tLDQ\t\t#$%s' % full, '\t\tSTQ\t\t8,X']
        else:
            L += ['\t\tLDD\t\t#$%04X' % opd, '\t\tSTD\t\t8,X']
        L += ['\t\tLDQ\t\t#$C3A5961E', '\t\tSTQ\t\t-16,U', '\t\tSTQ\t\t-12,U', '\t\tSTQ\t\t-8,U', '\t\tSTQ\t\t-4,U']
        L += ['\t\tLDD\t\t#$%04X' % D, '\t\tLDW\t\t#$%04X' % W, '\t\tLDV\t\t#$%08X' % V,
              '\t\tANDCC\t#$00', '\t\tORCC\t#$%02X' % cc]
        arg = ('#$' + hexs) if f == '#' else f
        L.append('\t\t%s\t%s' % (mn, arg))
        base = 'RES+%d' % (STRIDE * n)
        L += ['\t\tPSHS\tCC', '\t\tSTD\t\t>%s' % base, '\t\tSTW\t\t>%s+2' % base, '\t\tSTV\t\t>%s+4' % base,
              '\t\tSTX\t\t>%s+8' % base, '\t\tSTY\t\t>%s+12' % base, '\t\tSTU\t\t>%s+16' % base,
              '\t\tLDA\t\t,S+', '\t\tSTA\t\t>%s+20' % base]
        for k in range(4):                              # operand window: FFFC0308..0317
            L += ['\t\tLDQ\t\t>$FFFC%04X' % (0x0308 + 4 * k), '\t\tSTQ\t\t>%s+%d' % (base, 24 + 4 * k)]
        for k in range(4):                              # window under U: FFFC0350..035F
            L += ['\t\tLDQ\t\t>$FFFC%04X' % (0x0350 + 4 * k), '\t\tSTQ\t\t>%s+%d' % (base, 40 + 4 * k)]
    L += ['END:', '\t\tBRA\t\tEND', '', 'RES\t\tEQU\t\t$FFFC0400', '', '\t\tORG\t\t$FFFFFFF0'] + ['\t\tFDB\t\t$0000'] * 7 + ['\t\tFDB\t\t$0010', '', '\t\tEND']
    return L


def sh(cmd, timeout=300, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, '', 'TIMEOUT')


PART = 54
tot = bad = 0
for p in range((len(cases) + PART - 1) // PART):
    sub = cases[p * PART:(p + 1) * PART]
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'wide.asm'), 'w').write('\n'.join(build(sub)) + '\n')
    sh([A09, '-oF09', '-S', 'wide.asm'], cwd=tmp)
    if not os.path.exists(os.path.join(tmp, 'wide.s09')):
        print('part %d: a09 failed' % (p + 1)); bad += len(sub); tot += len(sub); continue
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, os.path.join(tmp, 'wide.s09'), elf])
    size = STRIDE * len(sub)
    r = sh([EMU, '--ncpus', '1', '--elf', elf, '--ticks', str(4000000 * len(sub) + 20000000), '--peek', '0x%X:%d' % (RES, size)], timeout=900)
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
        a, b = ref[STRIDE * k:STRIDE * k + 64], got[STRIDE * k:STRIDE * k + 64]
        tot += 1
        if a != b:
            nb += 1; bad += 1
            if verbose:
                d = [i for i in range(64) if a[i] != b[i]]
                print('  %s %s opd=%X D=%s cc=%02X : bytes %s\n      ref %s\n      sim %s' % (c[0], c[5], c[3], '%04X%04X%08X' % c[2], c[4], d[:10], a[:24].hex(), b[:24].hex()))
    print('part %d: %d cases, %d differ  %s' % (p + 1, len(sub), nb, stop[0][:80] if stop else ''))
print('%d cases, %d differ' % (tot, bad))
