#!/usr/bin/env python3
"""run_shn.py [-v] : shifts and rotates by n bits (LSRN ASRN ASLN RORN ROLN, on registers and on a byte in memory) on soc63f09 and on this simulator.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Registers A B D E F W O X Y U V Q (S carries the stack and is not tested), counts 0, 1, 2, max/2 and max of each width, two values,
CC 00 for all and CC 0F for a selection ; a byte in memory in the three forms (direct, indexed, extended) with counts 0 1 3 7.  Same
slots as run_wide.py (CC saved first by PSHS CC, then D W V X Y U and the memory windows, 128 bytes per case at $FFFC0400).
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
only = [a for a in sys.argv[1:] if not a.startswith('-')]

# register: (bytes, max count, load instruction)
REGS = {'a': (1, 7, 'lda'), 'b': (1, 7, 'ldb'), 'e': (1, 7, 'lde'), 'f': (1, 7, 'ldf'), 'd': (2, 15, 'ldd'), 'w': (2, 15, 'ldw'),
        'x': (4, 31, 'ldx'), 'y': (4, 31, 'ldy'), 'u': (4, 31, 'ldu'), 'v': (4, 31, 'ldv'), 'q': (4, 31, 'ldq'), 'o': (8, 63, 'ldo')}
OPS = ['lsrn', 'asrn', 'asln', 'rorn', 'roln']
VALS = [0xA55A3C96F00F1234, 0x80017FFFFFFFC3E1, 0x0000000000000001, 0xFFFFFFFFFFFFFFFF]
cases = []     # (kind, mnemonic, reg-or-form, count, value, cc)
for op in OPS:
    for reg, (sz, mx, ld) in REGS.items():
        for cnt in sorted(set([0, 1, 2, mx // 2, mx])):
            for vi, val in enumerate(VALS):
                for cc in ((0x00, 0x0F) if (vi in (0, 3) and cnt in (1, mx)) else (0x00,)):
                    cases.append(('reg', op, reg, cnt, val, cc))
for op in OPS:
    for form in ('<$08', '8,x', '>$FFFC0308'):
        for cnt in (0, 1, 3, 7):
            for val in (0xA5, 0x81, 0x7F, 0xFF):
                for cc in (0x00, 0x0F):
                    cases.append(('mem', op, form, cnt, val, cc))


def build(sub):
    L = ['\t\tORG\t\t$FFFF0010', '\t\tLDMD\t#$31', '\t\tLDS\t\t#$FFFD0000', '\t\tLDD\t\t#$FFFC', '\t\tTFR\t\tD,DS', '\t\tLDA\t\t#$03', '\t\tTFR\t\tA,DP']
    for n, (kind, op, what, cnt, val, cc) in enumerate(sub):
        L.append('* case %d : %s %s cnt=%d val=%X cc=%02X' % (n, op, what, cnt, val, cc))
        L += ['\t\tLDX\t\t#$FFFC0300', '\t\tLDY\t\t#$FFFC0320', '\t\tLDU\t\t#$FFFC0360', '\t\tLDQ\t\t#$A55A3C96', '\t\tLDV\t\t#$0F1E2D3C']
        if kind == 'reg':
            sz, mx, ld = REGS[what]
            v = val & ((1 << (sz * 8)) - 1)
            if sz == 8:
                L += ['\t\tLDQ\t\t#$%08X' % (v >> 32), '\t\tLDV\t\t#$%08X' % (v & 0xFFFFFFFF)]
            else:
                L.append('\t\t%s\t\t#$%0*X' % (ld, sz * 2, v))
            L += ['\t\tANDCC\t#$00', '\t\tORCC\t#$%02X' % cc, '\t\t%s%s\t#%d' % (op, what, cnt)]
        else:
            L += ['\t\tLDA\t\t#$%02X' % val, '\t\tSTA\t\t8,X', '\t\tLDD\t\t#$A55A', '\t\tANDCC\t#$00', '\t\tORCC\t#$%02X' % cc,
                  '\t\t%s\t#%d,%s' % (op, cnt, what)]
        base = 'RES+%d' % (STRIDE * n)
        L += ['\t\tPSHS\tCC', '\t\tSTD\t\t>%s' % base, '\t\tSTW\t\t>%s+2' % base, '\t\tSTV\t\t>%s+4' % base,
              '\t\tSTX\t\t>%s+8' % base, '\t\tSTY\t\t>%s+12' % base, '\t\tSTU\t\t>%s+16' % base, '\t\tLDA\t\t,S+', '\t\tSTA\t\t>%s+20' % base]
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
parts = (len(cases) + PART - 1) // PART
sel = [int(a) for a in only] or list(range(1, parts + 1))
tot = bad = 0
for p in sel:
    sub = cases[(p - 1) * PART:p * PART]
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'shn.asm'), 'w').write('\n'.join(build(sub)) + '\n')
    sh([A09, '-oF09', '-S', 'shn.asm'], cwd=tmp)
    if not os.path.exists(os.path.join(tmp, 'shn.s09')):
        print('part %d: a09 failed' % p); bad += len(sub); tot += len(sub); continue
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, os.path.join(tmp, 'shn.s09'), elf])
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
        print('part %d RUN FAILED: reference %d bytes (%s), simulator %d bytes (%s) %s' % (p, len(ref), 'nz' if any(ref) else 'zero', len(got), 'nz' if any(got) else 'zero', stop[:1]))
        bad += len(sub); tot += len(sub); continue
    nb = 0
    for k, c in enumerate(sub):
        a, b = ref[STRIDE * k:STRIDE * k + 32], got[STRIDE * k:STRIDE * k + 32]
        tot += 1
        if a != b:
            nb += 1; bad += 1
            if verbose and nb <= 6:
                d = [i for i in range(32) if a[i] != b[i]]
                print('  %s %s cnt=%d val=%X cc=%02X : bytes %s\n      ref %s\n      sim %s' % (c[1], c[2], c[3], c[4], c[5], d[:8], a.hex(), b.hex()))
    print('part %d/%d: %d cases, %d differ  %s' % (p, parts, len(sub), nb, stop[0][:70] if stop else ''))
print('%d cases, %d differ' % (tot, bad))
