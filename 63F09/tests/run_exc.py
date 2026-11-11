#!/usr/bin/env python3
"""run_exc.py [-v] : SWI SWI2 SWI3 RTI and the TRAPs (illegal opcode, division by 0, bad TFR, LDMD after S) in 32-bit mode, on soc63f09
and on this simulator.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Each case sets DS, DP, the registers and the CC, runs ONE triggering instruction.  The handler saves the 32 bytes at S (the frame), S, a tag and
the CC after BITMD #$C0 in the first 64 bytes of the case slot, then either RTI (SWI, SWI2, SWI3) or reloads S and jumps back (TRAP).  The
registers after the return are saved in the next 28 bytes.  The 16-bit frames cannot be compared with the RTL bench (a 16-bit core only sees the
ROM), they are checked by the self-test of the simulator (tests/exc16.ini).
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
EMU = os.path.expanduser('~/cvs/gcc/github/63F09-emulator/build/soc63f09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
RES = 0xFFFFC0400
STRIDE = 128
CMP = 92
verbose = '-v' in sys.argv

# (label, instruction lines, handler tag: 0 swi, 1 swi2, 2 swi3, 3 trap)
TRIG = [
    ('swi', ['\t\tSWI'], 'swi'),
    ('swi2', ['\t\tSWI2'], 'swi2'),
    ('swi3', ['\t\tSWI3'], 'swi3'),
    ('illegal c7', ['\t\tFCB\t\t$C7'], 'trap'),
    ('illegal cf', ['\t\tFCB\t\t$CF'], 'trap'),
    ('illegal 10 00', ['\t\tFCB\t\t$10,$00'], 'trap'),
    ('illegal 11 00', ['\t\tFCB\t\t$11,$00'], 'trap'),
    ('illegal 1b 00', ['\t\tFCB\t\t$1B,$00'], 'trap'),
    ('illegal 18 05 (SETDYN, reserved)', ['\t\tFCB\t\t$18,$05'], 'trap'),
    ('divd #0', ['\t\tLDD\t\t#$1234', '\t\tDIVD\t#$00'], 'trap'),
    ('divq #0', ['\t\tLDQ\t\t#$12345678', '\t\tDIVQ\t#$0000'], 'trap'),
    ('divdu #0', ['\t\tLDD\t\t#$1234', '\t\tDIVDU\t#$0000'], 'trap'),
    ('divd <$00', ['\t\tLDD\t\t#$1234', '\t\tDIVD\t<$20'], 'trap'),
    ('tfr x,d', ['\t\tFCB\t\t$1F,$10'], 'trap'),
    ('tfr d,w ok', ['\t\tTFR\t\tD,W'], 'none'),
    ('exg x,a', ['\t\tFCB\t\t$1E,$18'], 'trap'),
    ('ldmd after s', ['\t\tLDMD\t#$31'], 'trap'),
    ('shn bad byte', ['\t\tFCB\t\t$42,$30'], 'trap'),
    ('lsrn mem >7', ['\t\tFCB\t\t$1B,$D4,$09,$20'], 'trap'),
]
CCS = (0x00, 0x2A, 0x50)
# The RTL simulation stops (finish) in ERROR_STATE: the illegal-instruction TRAPs have no reference, they are only run on the simulator
ORACLE = ('swi', 'swi2', 'swi3', 'none')
cases = [(l + ' cc=%02X' % cc, ins, tag, cc) for l, ins, tag in TRIG for cc in CCS
         if tag in ORACLE or l.startswith('div')]
solo = [(l + ' cc=%02X' % cc, ins, tag, cc) for l, ins, tag in TRIG for cc in CCS
        if not (tag in ORACLE or l.startswith('div'))]


def handler(name, rti):
    L = [name + '\t\tTFR\t\tS,X', '\t\tBITMD\t#$C0', '\t\tPSHS\tCC', '\t\tPULS\tB', '\t\tLDY\t\t>PTR',
         '\t\tLDA\t\t#%d' % {'H_SWI': 0, 'H_SWI2': 1, 'H_SWI3': 2, 'H_TRAP': 3}[name],
         '\t\tSTA\t\t,Y', '\t\tSTB\t\t1,Y', '\t\tSTX\t\t2,Y', '\t\tLEAY\t8,Y', '\t\tLDB\t\t#32',
         name + 'C\tLDA\t\t,X+', '\t\tSTA\t\t,Y+', '\t\tDECB', '\t\tBNE\t\t' + name + 'C']
    if rti:
        L.append('\t\tRTI')
    else:
        L += ['\t\tLDS\t\t#$FFFD0000', '\t\tJMP\t\t[>RET]']
    return L


def build(sub):
    L = ['\t\tORG\t\t$FFFF0010', '\t\tLDMD\t#$31', '\t\tJMP\t\t>MAIN', '']
    L += ['\t\tORG\t\t$FFFF0100'] + handler('H_SWI', True) + ['\t\tORG\t\t$FFFF0140'] + handler('H_SWI2', True)
    L += ['\t\tORG\t\t$FFFF0180'] + handler('H_SWI3', True) + ['\t\tORG\t\t$FFFF01C0'] + handler('H_TRAP', False)
    L += ['', '\t\tORG\t\t$FFFF1000', 'MAIN\t\tLDS\t\t#$FFFD0000']
    for n, (label, ins, tag, cc) in enumerate(sub):
        L.append('* case %d : %s' % (n, label))
        L += ['\t\tLDS\t\t#$FFFD0000', '\t\tLDY\t\t#RES+%d' % (STRIDE * n), '\t\tSTY\t\t>PTR', '\t\tLDX\t\t#CONT%d' % n, '\t\tSTX\t\t>RET',
              '\t\tLDD\t\t#$FFFC', '\t\tTFR\t\tD,DS', '\t\tLDA\t\t#$03', '\t\tTFR\t\tA,DP',
              '\t\tLDX\t\t#$FFFC0300', '\t\tLDY\t\t#$FFFC0320', '\t\tLDU\t\t#$FFFC0360', '\t\tLDQ\t\t#$A55A3C96', '\t\tLDV\t\t#$0F1E2D3C']
        L += ['\t\tANDCC\t#$00', '\t\tORCC\t#$%02X' % cc] + ins
        L.append('CONT%d' % n)
        base = 'RES+%d' % (STRIDE * n + 64)
        L += ['\t\tPSHS\tCC', '\t\tSTD\t\t>%s' % base, '\t\tSTW\t\t>%s+2' % base, '\t\tSTV\t\t>%s+4' % base,
              '\t\tSTX\t\t>%s+8' % base, '\t\tSTY\t\t>%s+12' % base, '\t\tSTU\t\t>%s+16' % base,
              '\t\tLDA\t\t,S+', '\t\tSTA\t\t>%s+20' % base,
              '\t\tTFR\t\tDP,A', '\t\tSTA\t\t>%s+21' % base, '\t\tTFR\t\tDS,D', '\t\tSTD\t\t>%s+22' % base, '\t\tSTS\t\t>%s+24' % base]
    L += ['END:', '\t\tBRA\t\tEND', '', 'RES\t\tEQU\t\t$FFFC0400', 'PTR\t\tEQU\t\t$FFFC0300', 'RET\t\tEQU\t\t$FFFC0304', '',
          '\t\tORG\t\t$FFFFFFC0']
    vec = {0x18: '$01C0', 0x19: '$0180', 0x1a: '$0140', 0x1d: '$0100', 0x1f: '$0010'}
    L += ['\t\tFDB\t\t%s' % vec.get(i, '$0000') for i in range(32)] + ['', '\t\tEND']
    return L


def sh(cmd, timeout=300, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, '', 'TIMEOUT')


PART = 30
tot = bad = 0
for p in range((len(cases) + PART - 1) // PART):
    sub = cases[p * PART:(p + 1) * PART]
    tmp = tempfile.mkdtemp()
    if verbose: print('tmp', tmp)
    open(os.path.join(tmp, 'exc.asm'), 'w').write('\n'.join(build(sub)) + '\n')
    r = sh([A09, '-oF09', '-S', 'exc.asm'], cwd=tmp)
    if not os.path.exists(os.path.join(tmp, 'exc.s09')):
        print('part %d: a09 failed %s' % (p + 1, (r.stdout + r.stderr)[:300])); bad += len(sub); tot += len(sub); continue
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, os.path.join(tmp, 'exc.s09'), elf])
    size = STRIDE * len(sub)
    r = sh([EMU, '--ncpus', '2', '--fpu', 'shared', '--elf', elf, '--ticks', str(3000000 * len(sub) + 20000000), '--peek', '0x%X:%d' % (RES, size)], timeout=900)
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
            if verbose:
                d = [i for i in range(CMP) if a[i] != b[i]]
                print('  %s : bytes %s\n      ref %s\n      sim %s' % (c[0], d[:12], a.hex(), b.hex()))
    print('part %d: %d cases, %d differ  %s' % (p + 1, len(sub), nb, stop[0][:70] if stop else ''))
print('%d cases, %d differ' % (tot, bad))

# Simulator only: the frame must say TRAP (tag 3), MD(7:6) = 01, E set, and the PC just after the opcode bytes read
print('--- illegal-instruction TRAPs, simulator only')
for sub in [solo[i:i + 30] for i in range(0, len(solo), 30)]:
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'exc.asm'), 'w').write('\n'.join(build(sub)) + '\n')
    sh([A09, '-oF09', '-S', 'exc.asm'], cwd=tmp)
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, os.path.join(tmp, 'exc.s09'), elf])
    out = os.path.join(tmp, 'sim.bin')
    open(os.path.join(tmp, 's.ini'), 'w').write('load %s\nboot cpu\ngo\ndump %s %x-%x\n' % (elf, out, RES, RES + STRIDE * len(sub) - 1))
    sh([SIM, os.path.join(tmp, 's.ini')], timeout=120)
    got = open(out, 'rb').read()
    for k, c in enumerate(sub):
        b = got[STRIDE * k:STRIDE * k + CMP]
        tot += 1
        # frame at +8: CC(1) MD(4) DS(2) A B E F DP X Y U V PC ; the first byte is the CC, E (0x80) must be set
        cc, md = b[8], b[9:13]
        ok = b[0] == 3 and (cc & 0x80) and md[3] == 0x71 | 0x00 and True
        pcw = b[8 + 31 - 3:8 + 32] if False else None
        if verbose or not (b[0] == 3 and (cc & 0x80) and (md[3] & 0xC0) == 0x40):
            print('  %-24s tag %d bitmd-cc %02X frame cc %02X md %s pc %s' % (c[0], b[0], b[1], cc, md.hex(), b[8 + 28:8 + 32].hex()))
        if not (b[0] == 3 and (cc & 0x80) and (md[3] & 0xC0) == 0x40):
            bad += 1
print('%d cases, %d differ' % (tot, bad))
