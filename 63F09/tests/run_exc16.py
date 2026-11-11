#!/usr/bin/env python3
"""run_exc16.py [-v] : the 16-bit states (6809, 6309) take SWI, SWI2, SWI3 and a FIRQ and come back by RTI.  A 16-bit core only sees
the ROM (FFFF:xxxx), so the RTL bench cannot give a reference : the frames are checked against the layout of 63F09.vhd
(INTERRUPT_PUSH_*, RTI_*).  The simulator runs with SET CPU ROMWR (the stack and the results are in the ROM).

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
verbose = '-v' in sys.argv


def program(native):
    L = ['\t\tORG\t\t$0010\n']
    if native:
        L.append('\t\tLDMD\t#$01\n')
    L.append('\t\tLDS\t\t#$EF00\n\t\tLDB\t\t#$22\n')
    if native:
        L.append('\t\tLDE\t\t#$33\n\t\tLDF\t\t#$44\n')
    L.append('\t\tLDX\t\t#$5678\n\t\tLDY\t\t#$9ABC\n\t\tLDU\t\t#$DEF0\n\t\tLDA\t\t#$11\n\t\tANDCC\t#$40\n\t\tORCC\t#$0A\n')
    for n, ins in enumerate(('SWI', 'SWI2', 'SWI3')):
        o = 16 * n
        L.append('\t\t%s\n\t\tPSHS\tCC\n\t\tSTA\t\t>$F100+%d\n\t\tSTB\t\t>$F101+%d\n' % (ins, o, o))
        if native:
            L.append('\t\tSTE\t\t>$F102+%d\n\t\tSTF\t\t>$F103+%d\n' % (o, o))
        L.append('\t\tSTX\t\t>$F104+%d\n\t\tSTY\t\t>$F106+%d\n\t\tSTU\t\t>$F108+%d\n\t\tSTS\t\t>$F10A+%d\n\t\tPULS\tA\n\t\tSTA\t\t>$F10C+%d\n\t\tLDA\t\t#$11\n'
                 % (o, o, o, o, o))
    L.append('\t\tANDCC\t#$BF\n\t\tNOP\n\t\tNOP\n\t\tLDA\t\t#$77\n\t\tSTA\t\t>$F1F0\nEND\t\tBRA\t\tEND\n')
    flen = 14 if native else 12
    for name, k in (('HS', 0), ('HS2', 1), ('HS3', 2)):
        L.append('%s\t\tTFR\t\tS,X\n\t\tLDY\t\t#$F200+%d\n\t\tLDB\t\t#%d\n%sC\tLDA\t\t,X+\n\t\tSTA\t\t,Y+\n\t\tDECB\n\t\tBNE\t\t%sC\n'
                 % (name, 32 * k, flen, name, name))
        L.append('\t\tLDA\t\t#$EE\n\t\tLDB\t\t#$DD\n')
        if native:
            L.append('\t\tLDE\t\t#$CC\n\t\tLDF\t\t#$BB\n')
        L.append('\t\tLDX\t\t#$AAAA\n\t\tLDY\t\t#$9999\n\t\tLDU\t\t#$8888\n\t\tRTI\n')
    L.append('HF\t\tTFR\t\tS,X\n\t\tLDY\t\t#$F280\n\t\tLDB\t\t#3\nHFC\tLDA\t\t,X+\n\t\tSTA\t\t,Y+\n\t\tDECB\n\t\tBNE\t\tHFC\n\t\tLDA\t\t#$99\n\t\tJMP\t\tHFW\n')
    L.append('\t\tORG\t\t$0310\nHFW\t\tRTI\n')
    L.append('\t\tORG\t\t$FFF0\n\t\tFDB\t\t$0000,HS3,HS2,HF,$0000,HS,$0000,$0010\n\t\tEND\n')
    return ''.join(L)


def check(native, got):
    errs = []
    mem = lambda a: got[a - 0xF100]
    word = lambda a: (mem(a) << 8) | mem(a + 1)
    exp_regs = [0x11, 0x22, 0x33 if native else 0, 0x44 if native else 0]
    for n in range(3):
        o = 0xF100 + 16 * n
        cc = 0xCA if n == 0 else 0xC0                    # LDA #$11 after the first return clears N and V
        regs = [mem(o), mem(o + 1), mem(o + 2), mem(o + 3)]
        if native and regs != exp_regs:
            errs.append('swi%d: A B E F = %s' % (n, regs))
        if not native and regs[:2] != exp_regs[:2]:
            errs.append('swi%d: A B = %s' % (n, regs[:2]))
        if (word(o + 4), word(o + 6), word(o + 8), word(o + 10)) != (0x5678, 0x9ABC, 0xDEF0, 0xEEFF):
            errs.append('swi%d: X Y U S = %04X %04X %04X %04X' % (n, word(o + 4), word(o + 6), word(o + 8), word(o + 10)))
        if mem(o + 12) != cc:
            errs.append('swi%d: CC after RTI = %02X (%02X expected)' % (n, mem(o + 12), cc))
        f = 0xF200 + 32 * n
        frame = bytes(mem(f + i) for i in range(14 if native else 12))
        want = bytes([cc, 0x11, 0x22] + ([0x33, 0x44] if native else []) + [0x00, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0])
        if frame[:len(want)] != want:
            errs.append('swi%d: frame %s, expected %s then PC' % (n, frame.hex(), want.hex()))
    if mem(0xF280) & 0x80:
        errs.append('FIRQ: E set in the fast frame (%02X)' % mem(0xF280))
    if mem(0xF281) == 0 and mem(0xF282) == 0:
        errs.append('FIRQ: no frame saved (the interrupt was never taken)')
    if mem(0xF1F0) != 0x77:
        errs.append('FIRQ: the program did not come back (F1F0 = %02X)' % mem(0xF1F0))
    return errs


bad = 0
for native in (0, 1):
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'p.asm'), 'w').write(program(native))
    r = subprocess.run([A09, '-oH63' if native else '-oM09', '-S', 'p.asm'], cwd=tmp, capture_output=True, text=True)
    if not os.path.exists(os.path.join(tmp, 'p.s09')):
        print('a09 failed:', (r.stdout + r.stderr)[:400]); bad += 1; continue
    elf = os.path.join(tmp, 'p.elf')
    subprocess.run(['python3', CONV, os.path.join(tmp, 'p.s09'), elf, 'FFFF0000'], capture_output=True)
    out = os.path.join(tmp, 'o.bin')
    open(os.path.join(tmp, 's.ini'), 'w').write('set cpu romwr\nload %s\nreset\ndep IRQLINES 1\nbreak FFFF0310\ngo\ndep IRQLINES 0\nnobreak FFFF0310\ngo\n'
                                                 'dump %s FFFFFF100-FFFFFF2FF\n' % (elf, out))
    rs = subprocess.run([SIM, os.path.join(tmp, 's.ini')], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60)
    if not os.path.exists(out):
        print('%s: no dump\n%s' % ('6309' if native else '6809', rs.stdout[-400:])); bad += 1; continue
    errs = check(native, open(out, 'rb').read())
    print('%s: %s' % ('6309' if native else '6809', 'OK' if not errs else '; '.join(errs)))
    bad += len(errs)
    if verbose:
        print(rs.stdout[-300:])
sys.exit(1 if bad else 0)
