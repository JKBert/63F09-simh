#!/usr/bin/env python3
"""coverage.py : which mnemonics of the opcode table the executor (m63f09_exec.c) handles.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Mirrors classify() of m63f09_exec.c (the families recognised from the name) and adds the mnemonics that have an explicit
`case MN_xxx` in the source ; everything else is reported as NOT implemented, grouped by theme.
"""
import os, re
HERE = os.path.dirname(os.path.abspath(__file__))
tab = open(os.path.join(HERE, '..', 'm63f09_optab.h')).read()
src = open(os.path.join(HERE, '..', 'm63f09_exec.c')).read()
rows = re.findall(r'\{ "(\w+)", MN_\w+, 0x(\w+), 0x(\w+), \d+, \d+, \d+, \d+, 0x(\w+), 0x(\w+) \}', tab)
names = sorted(set(r[0] for r in rows))
page1b = {}
for n, op, pg, arch, mode in rows:
    if pg == '1b': page1b.setdefault(n, set()).add(mode)
REG = set('abdefwxyusvqo')
ALU = ['add', 'adc', 'sub', 'sbc', 'and', 'or', 'eor', 'cmp', 'bit', 'ld', 'st']
UN = ['neg', 'com', 'lsr', 'ror', 'asr', 'asl', 'lsl', 'rol', 'dec', 'inc', 'tst', 'clr']
CONDS = 'ra rn hi ls cc cs ne eq vc vs pl mi ge lt gt le'.split()


def family(n):
    if len(n) == 4 and n[3] == 'n' and n in page1b:
        return None                                     # shifts of a byte in memory (page 1B) : skipped by classify()
    for a in ALU:
        if len(n) == len(a) + 1 and n.startswith(a) and n[len(a)] in REG:
            return 'alu'
    for u in UN:
        if n.startswith(u) and (len(n) == len(u) or (len(n) == len(u) + 1 and n[len(u)] in REG)):
            return 'unary'
    if len(n) == 4 and n.startswith('lea') and n[3] in 'xyus': return 'lea'
    if n in ('jmp', 'jsr', 'bsr', 'lbsr', 'bhs', 'blo', 'lbhs', 'lblo', 'tfr', 'exg'): return 'misc-family'
    if n in ('addr', 'adcr', 'subr', 'sbcr', 'andr', 'orr', 'eorr', 'cmpr'): return 'regreg'
    if n[0] == 'b' and n[1:] in CONDS or n[:2] == 'lb' and n[2:] in CONDS: return 'branch'
    return None


fpu = set(n for n, op, pg, arch, mode in rows if pg == '18')       # all handled by do_fpu() (decoded by opcode)
cases = set(m.upper() for m in re.findall(r'case MN_(\w+)', src))
SHN = re.compile(r'^(lsr|asr|asl|lsl|ror|rol)n[a-z]?$')       # shifts and rotates by n: K_SHN and K_SHNMEM (classify())
done, todo = [], []
for n in names:
    if family(n) or SHN.match(n) or n in fpu or n.upper() in cases or (n.upper().startswith('TFM_') and 'TFM_II' in cases):
        done.append(n)
    else:
        todo.append(n)
print('%d mnemonics in the table, %d handled, %d NOT handled\n' % (len(names), len(done), len(todo)))
groups = [('FPU (page 18)', lambda n: any(rows_ for rows_ in [1]) and n in fpu), ]
fpu = set(n for n, op, pg, arch, mode in rows if pg == '18')
themes = [
    ('FPU', lambda n: n in fpu),
    ('64-bit O family', lambda n: n.endswith('o') and n[:-1] in ALU + UN + ['mul', 'div', 'sex'] or n in ('mulo', 'divo', 'mulou', 'divou', 'sexo')),
    ('unsigned / signed multiply and divide (MUL.., DIV..)', lambda n: n.startswith('mul') or n.startswith('div')),
    ('shifts and rotates by n', lambda n: n.endswith('n') and n[:3] in ('lsr', 'asr', 'asl', 'lsl', 'ror', 'rol')),
    ('interrupts, calls and control (RTI, SWI, CWAI, SYNC, LOCK...)', lambda n: n in ('rti', 'swi', 'swi2', 'swi3', 'cwai', 'sync', 'lock', 'unlock', 'eni', 'dii', 'tfm', 'bitmd', 'ldmd')),
    ('stack of the FPU', lambda n: 'fp' in n),
]
left = list(todo)
for title, f in themes:
    g = [n for n in left if f(n)]
    left = [n for n in left if n not in g]
    if g: print('%-60s %3d : %s' % (title, len(g), ' '.join(g)))
if left: print('%-60s %3d : %s' % ('other', len(left), ' '.join(left)))
