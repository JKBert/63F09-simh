#!/usr/bin/env python3
"""run_fpu.py [-v] [N] [seed] : FPU instructions of the 63F09HF on soc63f09 and on this simulator.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Each case clears the FPU stack, sets a rounding mode, pushes operands (LDSFP/LDDFP immediate), runs ONE FPU operation and dumps the CC
(PSHS CC) and the FPU stack (PSHSNFP #4 : FPCC then FP3..FP0) into a 64-byte slot, so that the stack effect, the result, the flags
and the CC are all compared.
"""
import os, random, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
EMU = os.path.expanduser('~/cvs/gcc/github/63F09-emulator/build/soc63f09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
RES = 0xFFFFC0400
STRIDE = 64
CMP = 34
args = [a for a in sys.argv[1:] if not a.startswith('-')]
verbose = '-v' in sys.argv
N = int(args[0]) if args else 400
rnd = random.Random(int(args[1]) if len(args) > 1 else 1)

SV = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x40490FDB, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0x00000001,
      0x007FFFFF, 0x7F7FFFFF, 0x3F000000, 0x40000000, 0x4B000001, 0x4EFFFFFF, 0xCF000000, 0x5F000000, 0x00800000, 0xC0A00000, 0x41200000]
DV = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x400921FB54442D18, 0x7FF0000000000000,
      0xFFF0000000000000, 0x7FF8000000000000, 0x7FF0000000000001, 0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF,
      0x3FE0000000000000, 0x4000000000000000, 0x4330000000000001, 0x43DFFFFFFFFFFFFF, 0xC3E0000000000000, 0x43F0000000000000,
      0x0010000000000000, 0xC014000000000000, 0x4024000000000000, 0x41DFFFFFFFC00000, 0xC1E0000000000000, 0x41EFFFFFFFE00000]
INTS = [0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x12345678, 0xFFFFFFFE, 0x01000001, 0x00FFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
        0xFFFFFFFFFFFFFFFF, 0x0123456789ABCDEF, 0x0020000000000001, 0xFFFFFFFF80000000]
RMS = ['SETRNE', 'SETRTZ', 'SETRDN', 'SETRUP', 'SETRMM']

ARITH = ['FMADD', 'FMSUB', 'FNMADD', 'FNMSUB', 'FADD', 'FSUB', 'FMUL', 'FDIV', 'FSQRT', 'FSGNJ', 'FCMP', 'FMAX', 'FCLASS']
NARGS = {'FMADD': 3, 'FMSUB': 3, 'FNMADD': 3, 'FNMSUB': 3, 'FSQRT': 1, 'FCLASS': 1}
CVT = [('CVTD2S', 'd'), ('CVTS2D', 's'), ('CVTI2D', 'i'), ('CVTU2D', 'i'), ('CVTI2S', 'i'), ('CVTU2S', 'i'), ('CVTS2I', 's'), ('CVTS2U', 's'),
       ('CVTD2I', 'd'), ('CVTD2U', 'd'), ('CVTOI2D', 'o'), ('CVTOU2D', 'o'), ('CVTOI2S', 'o'), ('CVTOU2S', 'o'), ('CVTS2OI', 's'),
       ('CVTS2OU', 's'), ('CVTD2OI', 'd'), ('CVTD2OU', 'd')]


def push(kind, v):
    if kind == 's': return '\t\tLDSFP\t#$%08X' % (v & 0xFFFFFFFF)
    if kind == 'd': return '\t\tLDDFP\t#$%016X' % v
    if kind == 'i': return '\t\tLDSFP\t#$%08X' % (v & 0xFFFFFFFF)
    return '\t\tLDDFP\t#$%016X' % (v & 0xFFFFFFFFFFFFFFFF)


def gen():
    kind = rnd.random()
    rm = rnd.choice(RMS)
    if kind < 0.6:
        base = rnd.choice(ARITH)
        pop = rnd.random() < 0.5
        dbl = rnd.random() < 0.5
        name = base + ('D' if dbl else 'S') if pop else base + 'N' + ('D' if dbl else 'S')
        vals = DV if dbl else SV
        n = NARGS.get(base, 2)
        ops = [rnd.choice(vals) if rnd.random() < 0.75 else (rnd.getrandbits(64) if dbl else rnd.getrandbits(32)) for _ in range(n)]
        extra = [rnd.choice(vals) for _ in range(rnd.randrange(3))]
        lines = [push('d' if dbl else 's', v) for v in extra + ops]
        if base in ('FSGNJ', 'FCMP', 'FMAX'):
            rm = rnd.choice(RMS[:3])
        return ('%s rm=%s %s' % (name, rm, ' '.join('%X' % v for v in ops)), rm, lines, '\t\t%s' % name)
    name, src = rnd.choice(CVT)
    if src == 's': v = rnd.choice(SV) if rnd.random() < 0.8 else rnd.getrandbits(32); lines = [push('s', v)]
    elif src == 'd': v = rnd.choice(DV) if rnd.random() < 0.8 else rnd.getrandbits(64); lines = [push('d', v)]
    elif src == 'i': v = rnd.choice(INTS); lines = [push('i', v)]
    else: v = rnd.choice(INTS); lines = [push('o', v)]
    return ('%s rm=%s %X' % (name, rm, v), rm, lines, '\t\t%s' % name)


EDGE_S = [0xBF000000, 0xBE99999A, 0x80000000, 0xBF800000, 0xBFC00000, 0xC0000000, 0x3F000000, 0x3FC00000, 0x4EFFFFFF, 0x4F000000, 0x4F000001, 0xCF000000, 0xCF000001,
          0x4F800000, 0x4F7FFFFF, 0x5F000000, 0x5F800000, 0xDF000000, 0xDF000001, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0x00000001]
EDGE_D = [0xBFE0000000000000, 0xBFB999999999999A, 0x8000000000000000, 0xBFF0000000000000, 0xBFF8000000000000, 0x41DFFFFFFFC00000, 0x41DFFFFFFFE00000, 0x41E0000000000000,
          0xC1E0000000000000, 0xC1E0000000200000, 0x41EFFFFFFFE00000, 0x41EFFFFFFFF00000, 0x41F0000000000000, 0x43DFFFFFFFFFFFFF, 0x43E0000000000000, 0xC3E0000000000000,
          0xC3E0000000000001, 0x43EFFFFFFFFFFFFF, 0x43F0000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0x7FF0000000000001, 0x3FE0000000000000]
cases = []
for name, src in CVT:
    if src not in ('s', 'd'):
        continue
    for v in (EDGE_S if src == 's' else EDGE_D):
        for rm in RMS:
            cases.append(('%s rm=%s %X' % (name, rm, v), rm, [push(src, v)], '\t\t%s' % name))


def build(sub):
    L = ['\t\tORG\t\t$FFFF0010', '\t\tLDMD\t#$31', '\t\tLDS\t\t#$FFFD0000', '\t\tLDY\t\t#RES']
    for n, (label, rm, lines, ins) in enumerate(sub):
        L.append('* case %d : %s' % (n, label))
        L += ['\t\t%s' % rm, '\t\tCLRFP', '\t\tANDCC\t#$00', '\t\tORCC\t#$04'] + lines + [ins]
        L += ['\t\tPSHS\tCC', '\t\tPSHSNFP\t#4', '\t\tTFR\t\tS,X', '\t\tLDW\t\t#34', '\t\tTFM\t\tX+,Y+', '\t\tLEAS\t34,S',
              '\t\tLEAY\t%d,Y' % (STRIDE - 34)]
    L += ['END:', '\t\tBRA\t\tEND', '', 'RES\t\tEQU\t\t$FFFC0400', '', '\t\tORG\t\t$FFFFFFF0'] + ['\t\tFDB\t\t$0000'] * 7 + ['\t\tFDB\t\t$0010', '', '\t\tEND']
    return L


def sh(cmd, timeout=300, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, '', 'TIMEOUT')


def fmt(b):
    return 'fpcc %02x fp3 %s fp2 %s fp1 %s fp0 %s cc %02x' % (b[0], b[1:9].hex(), b[9:17].hex(), b[17:25].hex(), b[25:33].hex(), b[33])


PART = 60
tot = bad = 0
for p in range((len(cases) + PART - 1) // PART):
    sub = cases[p * PART:(p + 1) * PART]
    tmp = tempfile.mkdtemp()
    if verbose: print('tmp', tmp)
    open(os.path.join(tmp, 'fpu.asm'), 'w').write('\n'.join(build(sub)) + '\n')
    r = sh([A09, '-oF09', '-S', 'fpu.asm'], cwd=tmp)
    if not os.path.exists(os.path.join(tmp, 'fpu.s09')):
        print('part %d: a09 failed %s' % (p + 1, (r.stdout + r.stderr)[:300])); bad += len(sub); tot += len(sub); continue
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, os.path.join(tmp, 'fpu.s09'), elf])
    size = STRIDE * len(sub)
    r = sh([EMU, '--ncpus', '2', '--fpu', 'shared', '--elf', elf, '--ticks', str(6000000 * len(sub) + 20000000), '--peek', '0x%X:%d' % (RES, size)], timeout=1800)
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
        a, b = ref[STRIDE * k:STRIDE * k + CMP], got[STRIDE * k:STRIDE * k + CMP]
        tot += 1
        if a != b:
            nb += 1; bad += 1
            if verbose or nb <= 6:
                print('  %s\n      ref %s\n      sim %s' % (c[0], fmt(a), fmt(b)))
    print('part %d: %d cases, %d differ  %s' % (p + 1, len(sub), nb, stop[0][:70] if stop else ''))
print('%d cases, %d differ' % (tot, bad))
