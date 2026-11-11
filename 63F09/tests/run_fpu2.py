#!/usr/bin/env python3
"""run_fpu2.py [-v] [N] [seed] : FPU stack, load/store, transfer, push/pull and branch instructions on soc63f09 and on this simulator.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Each case fills the FPU stack with 10 distinct values, X with $FFFC0800 and U with $FFFC0A00, runs a short sequence, then dumps in a 256-byte
slot: CC and the FPU stack (PSHSNFP #8 : FPCC, FP7..FP0), X, A B, the 130 bytes below U (frames) and the 32 bytes at $FFFC0800.
"""
import os, random, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
EMU = os.path.expanduser('~/cvs/gcc/github/63F09-emulator/build/soc63f09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
RES = 0xFFFFC0400
STRIDE = 256
CMP = 66 + 4 + 2 + 130 + 32
args = [a for a in sys.argv[1:] if not a.startswith('-')]
verbose = '-v' in sys.argv
N = int(args[0]) if args else 300
rnd = random.Random(int(args[1]) if len(args) > 1 else 1)

STACK = [0x1111111111111111 * (i + 1) & 0xFFFFFFFFFFFFFFFF ^ (i << 3) for i in range(10)]
VALS = [0x3F800000, 0x7F800001, 0x00000000, 0x40000000, 0xFF800000, 0x7FC00000]


def gen():
    k = rnd.randrange(9)
    pre = ['\t\tLDDFP\t#$%016X' % v for v in STACK]
    rm = rnd.choice(['SETRNE', 'SETRTZ', 'SETRDN', 'SETRUP', 'SETRMM'])
    if k == 0:
        i = rnd.choice(['DROPFP', 'DROP2FP', 'DUPFP', 'DUP2FP', 'SWAPFP', 'OVERFP', 'ROTFP', 'CLRFP'])
        return (i, rm, pre, ['\t\t' + i])
    if k == 1:
        i = rnd.choice(['DUPNFP', 'DROPNFP', 'ROLLFP', 'ROLLDFP', 'PICKFP'])
        n = rnd.randrange(1, 17)
        return ('%s #%d' % (i, n), rm, pre, ['\t\t%s\t#%d' % (i, n)])
    if k == 2:
        a, b = rnd.randrange(1, 17), rnd.randrange(1, 17)
        return ('EXGFP #%d,#%d' % (a, b), rm, pre, ['\t\tEXGFP\t#%d,#%d' % (a, b)])
    if k == 3:
        if rnd.random() < 0.5:
            return ('TFROFP', rm, pre, ['\t\tLDQ\t\t#$%08X' % rnd.getrandbits(32), '\t\tLDV\t\t#$%08X' % rnd.getrandbits(32), '\t\tTFROFP'])
        return ('TFRFPO', rm, pre, ['\t\tTFRFPO'])
    if k == 4:
        f = rnd.choice(['LDSFP ,X', 'LDDFP ,X', 'LDSFP 4,X', 'LDDFP 8,X', 'LDDFP ,X++', 'LDSFP ,X++', 'LDDFP ,--X', 'LDSFP ,-X', 'LDSFP <$04',
                        'LDDFP <$08', 'LDSFP >$FFFC0808', 'LDDFP >$FFFC0804', 'LDSFP #$%08X' % rnd.getrandbits(32), 'LDDFP #$%016X' % rnd.getrandbits(64),
                        'LDDFP ,X+', 'LDSFP ,X+'])
        return (f, rm, pre, ['\t\t' + f])
    if k == 5:
        f = rnd.choice(['STSFP ,X', 'STDFP ,X', 'STSFP 4,X', 'STDFP 8,X', 'STDFP ,X++', 'STSFP ,X++', 'STDFP ,--X', 'STSFP <$04', 'STDFP <$08',
                        'STSFP >$FFFC0808', 'STDFP >$FFFC0804', 'STDFP ,X+', 'STSFP ,-X'])
        return (f, rm, pre, ['\t\t' + f])
    if k == 6:
        n = rnd.randrange(1, 17)
        a = rnd.choice(['PSHSFP', 'PSHUFP', 'PSHSNFP #%d' % n, 'PSHUNFP #%d' % n])
        b = rnd.choice(['', 'PULSFP', 'PULUFP', 'PULSNFP #%d' % rnd.randrange(1, 17), 'PULUNFP #%d' % rnd.randrange(1, 17)])
        seq = ['\t\t' + a]
        if b:
            seq += ['\t\tDUPFP', '\t\t' + b]
        return ('%s then %s' % (a, b), rm, pre, seq)
    if k == 7:
        # branches on the flags : an operation sets them, the branch skips an INCA when taken
        op = rnd.choice([('LDSFP #$7F800001', 'CVTS2I'), ('LDSFP #$3F800000', 'LDSFP #$00000000', 'FDIVS'), ('LDSFP #$7F7FFFFF', 'LDSFP #$7F7FFFFF', 'FADDS'),
                         ('LDSFP #$00000001', 'LDSFP #$3F000000', 'FMULS'), ('LDSFP #$3F800001', 'LDSFP #$40000000', 'FDIVS'),
                         ('LDSFP #$3F800000', 'LDSFP #$3F800000', 'FADDS')])
        br = rnd.choice(['BNV', 'BDZ', 'BOV', 'BUN', 'BIX', 'LBNV', 'LBDZ', 'LBOV', 'LBUN', 'LBIX'])
        seq = ['\t\t' + o for o in op] + ['\t\tCLRA', '\t\t%s\tSK0' % br, '\t\tINCA', 'SK0\t\tINCB']
        return ('%s after %s' % (br, ' '.join(op)), rm, pre, seq)
    r = rnd.choice(['SETRNE', 'SETRTZ', 'SETRDN', 'SETRUP', 'SETRMM'])
    return (r, rm, pre, ['\t\t' + r])


cases = [gen() for _ in range(N)]
for i, c in enumerate(cases):
    c[3][:] = [l.replace('SK0', 'SK%d' % i) if True else l for l in c[3]]


def build(sub):
    L = ['\t\tORG\t\t$FFFF0010', '\t\tLDMD\t#$31', '\t\tLDS\t\t#$FFFD0000', '\t\tLDY\t\t#RES']
    for n, (label, rm, pre, seq) in enumerate(sub):
        L.append('* case %d : %s' % (n, label))
        L += ['\t\tLDQ\t\t#$0123456789ABCDEF'[:0] or '\t\tLDQ\t\t#$11223344',
              '\t\tSTQ\t\t>$FFFC0800', '\t\tLDQ\t\t#$55667788', '\t\tSTQ\t\t>$FFFC0804', '\t\tLDQ\t\t#$99AABBCC', '\t\tSTQ\t\t>$FFFC0808',
              '\t\tLDQ\t\t#$DDEEFF01', '\t\tSTQ\t\t>$FFFC080C', '\t\tLDQ\t\t#$02030405', '\t\tSTQ\t\t>$FFFC0810',
              '\t\tLDQ\t\t#$06070809', '\t\tSTQ\t\t>$FFFC0814', '\t\tLDQ\t\t#$0A0B0C0D', '\t\tSTQ\t\t>$FFFC0818',
              '\t\tLDQ\t\t#$0E0F1011', '\t\tSTQ\t\t>$FFFC081C',
              '\t\tLDD\t\t#$FFFC', '\t\tTFR\t\tD,DS', '\t\tLDA\t\t#$08', '\t\tTFR\t\tA,DP', '\t\tLDX\t\t#$FFFC0800', '\t\tLDU\t\t#$FFFC0A00', '\t\tLDS\t\t#$FFFC8000',
              '\t\tLDD\t\t#$0000', '\t\t%s' % rm, '\t\tCLRFP', '\t\tANDCC\t#$00', '\t\tCLRB', '\t\tCLRA'] + pre + seq
        L += ['\t\tPSHS\tCC', '\t\tPSHSNFP\t#8', '\t\tSTX\t\t>$FFFC0900', '\t\tSTA\t\t>$FFFC0904', '\t\tSTB\t\t>$FFFC0905',
              '\t\tLDW\t\t#66', '\t\tTFM\t\tS+,Y+', '\t\tLDW\t\t#4', '\t\tLDX\t\t#$FFFC0900', '\t\tTFM\t\tX+,Y+',
              '\t\tLDW\t\t#2', '\t\tTFM\t\tX+,Y+', '\t\tLDW\t\t#130', '\t\tLDX\t\t#$FFFC0980', '\t\tTFM\t\tX+,Y+',
              '\t\tLDW\t\t#32', '\t\tLDX\t\t#$FFFC0800', '\t\tTFM\t\tX+,Y+', '\t\tLEAY\t%d,Y' % (STRIDE - CMP)]
    L += ['END:', '\t\tBRA\t\tEND', '', 'RES\t\tEQU\t\t$FFFC0400', '', '\t\tORG\t\t$FFFFFFF0'] + ['\t\tFDB\t\t$0000'] * 7 + ['\t\tFDB\t\t$0010', '', '\t\tEND']
    return L


def sh(cmd, timeout=300, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, '', 'TIMEOUT')


def fmt(b):
    return 'fpcc %02x fp7..0 %s cc %02x X %s AB %s mem0800 %s frame %s' % (b[0], ' '.join(b[1 + 8 * i:9 + 8 * i].hex() for i in range(8)), b[65], b[66:70].hex(),
                                                                          b[70:72].hex(), b[202:234].hex(), b[72:202].hex())


PART = 30
tot = bad = 0
for p in range((len(cases) + PART - 1) // PART):
    sub = cases[p * PART:(p + 1) * PART]
    tmp = tempfile.mkdtemp()
    if verbose: print('tmp', tmp)
    open(os.path.join(tmp, 'fpu.asm'), 'w').write('\n'.join(build(sub)) + '\n')
    r = sh([A09, '-oF09', '-S', 'fpu.asm'], cwd=tmp)
    if not os.path.exists(os.path.join(tmp, 'fpu.s09')):
        print('part %d: a09 failed %s' % (p + 1, (r.stdout + r.stderr)[:400])); bad += len(sub); tot += len(sub); continue
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, os.path.join(tmp, 'fpu.s09'), elf])
    size = STRIDE * len(sub)
    r = sh([EMU, '--ncpus', '2', '--fpu', 'shared', '--elf', elf, '--ticks', str(8000000 * len(sub) + 20000000), '--peek', '0x%X:%d' % (RES, size)], timeout=1800)
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
