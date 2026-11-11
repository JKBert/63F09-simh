#!/usr/bin/env python3
"""run_irq.py [-v] : interrupt lines (IRQ, FIRQ, NMI, EIRQ), CWAI and SYNC in 32-bit mode, on the simulator only.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

The lines are driven by the script (registers IRQLINES and NMIPEND of the CPU device) : the RTL bench has no device that raises one without
`--pins`.  The handlers count their calls in RAM ($FFFC0300 IRQ, 301 FIRQ, 302 NMI, 303 EIRQ3) ; the script drops the line when the handler
reaches its RTI.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
verbose = '-v' in sys.argv

HANDLERS = '\n'.join('\t\tORG\t\t$FFFF%04X\n\t\tINC\t\t>$FFFC%04X\nR%d\t\tRTI' % (a, c, i) for i, (a, c) in enumerate(
    [(0x0200, 0x0300), (0x0240, 0x0301), (0x0280, 0x0302), (0x02C0, 0x0303)]))
VECT = '\t\tORG\t\t$FFFFFFC0\n' + '\n'.join('\t\tFDB\t\t%s' % {3: '$02C0', 0x1b: '$0240', 0x1c: '$0200', 0x1e: '$0280', 0x1f: '$0010'}.get(i, '$0000')
                                         for i in range(32)) + '\n\t\tEND\n'

# name, body, ini commands before the first go, line to drop at the RTI (address), expected bytes at $FFFC0300..0303 and 0310
PHASES = [
    ('IRQ taken by ANDCC', '\t\tANDCC\t#$EF\n\t\tLDA\t\t#$11\n\t\tSTA\t\t>$FFFC0310', ['dep IRQLINES 2'], 0x0205, [1, 0, 0, 0, 0x11]),
    ('CWAI + IRQ', '\t\tCWAI\t#$000000EF\n\t\tLDA\t\t#$22\n\t\tSTA\t\t>$FFFC0310', ['dep IRQLINES 2'], 0x0205, [1, 0, 0, 0, 0x22]),
    ('SYNC with IRQ masked: no vector', '\t\tSYNC\n\t\tLDA\t\t#$33\n\t\tSTA\t\t>$FFFC0310', ['dep IRQLINES 2'], None, [0, 0, 0, 0, 0x33]),
    ('IRQ masked by I stays pending', '\t\tLDA\t\t#$44\n\t\tSTA\t\t>$FFFC0310', ['dep IRQLINES 2'], None, [0, 0, 0, 0, 0x44]),
    ('FIRQ (fast frame)', '\t\tANDCC\t#$BF\n\t\tLDA\t\t#$55\n\t\tSTA\t\t>$FFFC0310', ['dep IRQLINES 1'], 0x0245, [0, 1, 0, 0, 0x55]),
    ('NMI armed by the first S load', '\t\tLDA\t\t#$66\n\t\tSTA\t\t>$FFFC0310', ['dep NMIPEND 1'], None, [0, 0, 1, 0, 0x66]),
    ('EIRQ3 on level', '\t\tLDA\t\t#$77\n\t\tSTA\t\t>$FFFC0310', ['break FFFF002E', 'go', 'nobreak FFFF002E', 'dep IRQLINES 20'], 0x02C5, [0, 0, 0, 1, 0x77]),
]

bad = 0
for name, body, pre, rti, want in PHASES:
    tmp = tempfile.mkdtemp()
    if verbose: print('tmp', tmp)
    prog = ('\t\tORG\t\t$FFFF0010\n\t\tLDMD\t#$31\n\t\tCLRA\n\t\tSTA\t\t>$FFFC0300\n\t\tSTA\t\t>$FFFC0301\n'
            '\t\tSTA\t\t>$FFFC0302\n\t\tSTA\t\t>$FFFC0303\n\t\tLDS\t\t#$FFFD0000\n' + body + '\nEND\t\tBRA\t\tEND\n' + HANDLERS + '\n' + VECT)
    open(os.path.join(tmp, 'p.asm'), 'w').write(prog)
    r = subprocess.run([A09, '-oF09', '-S', 'p.asm'], cwd=tmp, capture_output=True, text=True)
    if not os.path.exists(os.path.join(tmp, 'p.s09')):
        print('%s: a09 failed %s' % (name, (r.stdout + r.stderr)[:300])); bad += 1; continue
    elf = os.path.join(tmp, 'p.elf')
    subprocess.run(['python3', CONV, os.path.join(tmp, 'p.s09'), elf], capture_output=True)
    out = os.path.join(tmp, 'o.bin')
    ini = ['load %s' % elf, 'reset'] + pre
    if rti:
        ini += ['break FFFF%04X' % rti, 'go', 'dep IRQLINES 0', 'dep NMIPEND 0', 'nobreak FFFF%04X' % rti]
    ini += ['go', 'dump %s FFFFC0300-FFFFC0310' % out]
    open(os.path.join(tmp, 's.ini'), 'w').write('\n'.join(ini) + '\n')
    rs = subprocess.run([SIM, os.path.join(tmp, 's.ini')], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60)
    got = open(out, 'rb').read() if os.path.exists(out) else b''
    have = [got[i] for i in range(4)] + [got[16]] if len(got) >= 17 else None
    ok = have == want
    print('%-34s %s %s' % (name, 'OK  ' if ok else 'FAIL', '' if ok else 'got %s want %s\n%s' % (have, want, rs.stdout[-300:])))
    bad += 0 if ok else 1
sys.exit(1 if bad else 0)
