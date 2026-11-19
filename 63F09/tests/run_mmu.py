#!/usr/bin/env python3
"""run_mmu.py [-v] [--rtl] [name...] : MMU 63F29 (m63f09_mmu.c), 32-bit mode, self-checking scenarios on the simulator.
With --rtl the RTL emulator soc63f09 (one core) runs the same programs and the results are compared too.
(Before the fix of the NCPUS = 1 branch of 63F81.vhd, 2026-11-17, its MMU stayed in remote mode : --ncpus 2 was needed.)

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Each scenario is a program (A09, -oF09) that leaves its observations in the internal RAM at $FFFC0100..: both emulators run it and the bytes
are compared.  The tables are filled with the MMU off, from ring 0 ; RING = $81 turns it on.
  fuse    : FUSE = k then RING = $81 (ring 1), then a sled of 64 bytes that is NOP in ring 0 and INCB in ring 1 : B tells at which cycle
            the ring changed (the whole point of FUSE : it counts bus cycles).
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
RTL = os.environ.get('SOC63F09') or os.path.expanduser('~/cvs/gcc/github/63F09-emulator/build/soc63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
verbose = '-v' in sys.argv
use_rtl = '--rtl' in sys.argv
names = [a for a in sys.argv[1:] if not a.startswith('-')]

AM, X, W, R = 8, 4, 2, 1
RESULT = 0xFFFFC0100                                    # physical address of $FFFC0100 seen through the identity map of the tests
NRES = 32

EQUS = '''MMU0        EQU     $FFFD0000
RING        EQU     MMU0
FUSE        EQU     MMU0+1
STATUS      EQU     MMU0+2
MAXRING     EQU     MMU0+3
TAG_STAGING EQU     MMU0+4
FADDR       EQU     MMU0+8
NIND        EQU     MMU0+12
IND_RING0   EQU     MMU0+$800
IND_RING1   EQU     MMU0+$A00
IND_RING2   EQU     MMU0+$C00
IND_RING3   EQU     MMU0+$E00
RES         EQU     $FFFC0100
'''


def mapping(ring, logical, ppn, flags):
    """ring 0..3, logical address (page), physical page number (24 bits), flags AM X W R"""
    return ('\t\tLDQ\t\t#$%08X\n\t\tSTQ\t\t>TAG_STAGING\n\t\tLDQ\t\t#$%08X\n\t\tSTQ\t\t>IND_RING%d\n'
            % (logical & 0xFFFFF000, (flags << 24) | (ppn & 0xFFFFFF), ring))


def common_maps(rings):
    s = ''
    for r in rings:
        s += mapping(r, 0xFFFF0000, 0xFFFFF0, AM | X | R)      # the code
        s += mapping(r, 0xFFFFC000 - 0x4000, 0xFFFFC0, AM | W | R) if False else ''
        s += mapping(r, 0xFFFC0000, 0xFFFFC0, AM | W | R)       # the result area
        s += mapping(r, 0xFFFCF000, 0xFFFFCF, AM | W | R)       # the stack
    s += mapping(0, 0xFFFFF000, 0xFFFFFF, AM | R)               # the vectors (ring 0 only)
    return s


PRELUDE = '\t\tORG\t\t$FFFF0010\n\t\tLDMD\t#$31\n\t\tLDS\t\t#$FFFD0000\n'
VECT = '\t\tORG\t\t$FFFFFFC0\n' + '\n'.join('\t\tFDB\t\t%s' % ('$0010' if i == 0x1f else '$0000') for i in range(32)) + '\n\t\tEND\n'


def vect_with(entries):
    return ('\t\tORG\t\t$FFFFFFC0\n' + '\n'.join('\t\tFDB\t\t%s' % entries.get(i, '$0010' if i == 0x1f else '$0000') for i in range(32)) +
            '\n\t\tEND\n')


def fuse_prog(k):
    body = PRELUDE + common_maps([0, 1])
    body += mapping(0, 0x00001000, 0xFFFFF1, AM | X | R) + mapping(1, 0x00001000, 0xFFFFF2, AM | X | R)
    body += '\t\tLDA\t\t#$EE\n\t\tSTA\t\t>RES\n\t\tSTA\t\t>RES+1\n\t\tSTA\t\t>RES+2\n'
    body += '\t\tCLRB\n\t\tLDA\t\t#%d\n\t\tSTA\t\t>FUSE\n\t\tLDA\t\t#$81\n\t\tSTA\t\t>RING\n\t\tJMP\t\t>$00001000\n' % k
    body += '\t\tORG\t\t$FFFF0800\nDONE\tSTB\t\t>RES\n\t\tLDA\t\t>FUSE\n\t\tSTA\t\t>RES+1\n\t\tLDA\t\t#$5A\n\t\tSTA\t\t>RES+2\nEND\t\tBRA\t\tEND\n'
    body += '\t\tORG\t\t$FFFF1000\n' + '\t\tFCB\t\t$12\n' * 64 + '\t\tJMP\t\t>$FFFF0800\n'
    body += '\t\tORG\t\t$FFFF2000\n' + '\t\tFCB\t\t$5C\n' * 64 + '\t\tJMP\t\t>$FFFF0800\n'
    return body + EQUS + VECT


SCENARIOS = {}


def scenario(name):
    def deco(f):
        SCENARIOS[name] = f
        return f
    return deco


def fuse_expect(k):
    """B = number of sled bytes run in ring 1.  After JMP the first fetch of the sled comes 15 cycles after the write of FUSE
    (LDA # 2 + STA 7 + JMP 6, regress/asm/mmuU1.asm) ; NOP and INCB take one cycle each."""
    return max(0, min(64, 64 - (k - 15)))


@scenario('fuse')
def sc_fuse():
    return [('fuse k=%d' % k, fuse_prog(k), bytes([fuse_expect(k), 0x00 if k else 0x00, 0x5A])) for k in
            list(range(0, 24)) + [30, 40, 50, 60, 70, 78, 79, 80]]


def rti_prog():
    """Ring 0 enters ring 1 (FUSE = 15), ring 1 does SWI, the handler (ring 0) sets FUSE = 10 and RING = $81 then RTI : the opcode of RTI is
    fetched in ring 0, the frame is popped in ring 1 (its stack is a page that only ring 1 maps) and the next instruction runs in ring 1."""
    body = PRELUDE + common_maps([0, 1])
    body += mapping(0, 0xFFFF8000, 0xFFFFF8, AM | X | R)
    body += mapping(1, 0x00000000, 0xFFFFC5, AM | W | R) + mapping(1, 0x00001000, 0xFFFFF3, AM | X | R)
    body += '\t\tLDA\t\t#$EE\n\t\tSTA\t\t>RES\n\t\tSTA\t\t>RES+1\n\t\tSTA\t\t>RES+2\n\t\tSTA\t\t>RES+3\n'
    body += '\t\tLDA\t\t#15\n\t\tSTA\t\t>FUSE\n\t\tLDA\t\t#$81\n\t\tSTA\t\t>RING\n\t\tJMP\t\t>$00001000\n'
    body += '\t\tORG\t\t$FFFF3000\n\t\tLDS\t\t#$00001000\n\t\tLDA\t\t>RING\n\t\tSTA\t\t>RES\n\t\tSWI\n'
    body += '\t\tLDA\t\t#$77\n\t\tSTA\t\t>RES+1\n\t\tLDA\t\t>STATUS\n\t\tSTA\t\t>RES+2\n\t\tLDA\t\t#$5A\n\t\tSTA\t\t>RES+3\nEND\t\tBRA\t\tEND\n'
    body += '\t\tORG\t\t$FFFF8000\nSWH\t\tLDA\t\t#10\n\t\tSTA\t\t>FUSE\n\t\tLDA\t\t#$81\n\t\tSTA\t\t>RING\n\t\tRTI\n'
    return body + EQUS + vect_with({0x1d: '$8000'})


@scenario('rti')
def sc_rti():
    # RES = RING as seen in ring 1 (the SWI entry has not run yet : $81), marker after the RTI, STATUS (0: no fault), done
    return [('RTI back to ring 1 (FUSE = 10)', rti_prog(), bytes([0x81, 0x77, 0x00, 0x5A]))]


def pf_prog():
    """A read of an unmapped page in ring 0 : TRAP, the handler records STATUS and the stacked PC, maps the page and returns : the
    instruction is replayed and succeeds.  A write to a page that has no write bit, then an execute in a page with no X, do the same."""
    body = PRELUDE + common_maps([0])
    body += mapping(0, 0xFFFF8000, 0xFFFFF8, AM | X | R)
    body += '\t\tLDA\t\t#$EE\n\t\tSTA\t\t>RES\n\t\tSTA\t\t>RES+1\n\t\tSTA\t\t>RES+2\n\t\tSTA\t\t>RES+3\n'
    body += '\t\tSTA\t\t>RES+4\n\t\tSTA\t\t>RES+5\n\t\tSTA\t\t>RES+6\n\t\tSTA\t\t>RES+7\n'
    body += '\t\tCLR\t\t>COUNT\n\t\tCLR\t\t>PHASE\n'
    body += '\t\tLDA\t\t#$80\n\t\tSTA\t\t>RING\n'                       # MMU on, ring 0
    body += '\t\tLDA\t\t>$00005000\n\t\tSTA\t\t>RES+4\n'                  # page fault (not mapped), replayed after the handler mapped the page
    body += '\t\tINC\t\t>PHASE\n\t\tSTA\t\t>$00006000\n'                  # write denied (page mapped read only), replayed once it can be written
    body += '\t\tINC\t\t>PHASE\n\t\tJSR\t\t>$00007000\n'                  # execute denied (page without X), replayed
    body += '\t\tLDA\t\t#$5A\n\t\tSTA\t\t>RES+7\n\t\tLDA\t\t>NIND\n\t\tSTA\t\t>RES+20\nEND\t\tBRA\t\tEND\n'
    # handler : RES+0.. = STATUS of the three faults, RES+3 = number of faults, the page is repaired according to PHASE
    body += '\t\tORG\t\t$FFFF8000\nPFH\t\tLDA\t\t>PHASE\n\t\tLDB\t\t>STATUS\n\t\tLDX\t\t#RES\n\t\tLEAX\tA,X\n\t\tSTB\t\t,X\n'
    body += '\t\tINC\t\t>RES+3\n'
    # FAULT_ADDR (4 bytes, MMU0+8) of this fault goes to RES+8+4*PHASE
    body += '\t\tLDB\t\t#4\n\t\tMUL\n\t\tLDX\t\t#RES+8\n\t\tLEAX\tD,X\n\t\tLDQ\t\t>FADDR\n\t\tSTQ\t\t,X\n\t\tLDA\t\t>PHASE\n'
    body += '\t\tTSTA\n\t\tBEQ\t\tP0\n\t\tCMPA\t#1\n\t\tBEQ\t\tP1\n'
    body += '\t\tLDQ\t\t#$00007000\n\t\tSTQ\t\t>TAG_STAGING\n\t\tLDQ\t\t#$0DFFFFF9\n\t\tSTQ\t\t>IND_RING0\n\t\tRTI\n'
    body += 'P0\t\tLDQ\t\t#$00005000\n\t\tSTQ\t\t>TAG_STAGING\n\t\tLDQ\t\t#$0BFFFFC6\n\t\tSTQ\t\t>IND_RING0\n\t\tRTI\n'
    body += 'P1\t\tLDQ\t\t#$00006000\n\t\tSTQ\t\t>TAG_STAGING\n\t\tLDQ\t\t#$0BFFFFC7\n\t\tSTQ\t\t>IND_RING0\n\t\tRTI\n'
    # page $00006000 is mapped read only before the MMU is on, page $00007000 without X : done by the first lines of the program
    pre = mapping(0, 0x00006000, 0xFFFFC7, AM | R) + mapping(0, 0x00007000, 0xFFFFF9, AM | R)
    body = body.replace('\t\tLDA\t\t#$80\n\t\tSTA\t\t>RING\n', pre + '\t\tLDA\t\t#$80\n\t\tSTA\t\t>RING\n', 1)
    body += '\t\tORG\t\t$FFFF9000\n\t\tRTS\n'
    return body + EQUS + 'COUNT\tEQU\t$FFFC0200\nPHASE\tEQU\t$FFFC0201\n' + vect_with({0x18: '$8000'})


@scenario('pf')
def sc_pf():
    # RES+0 = STATUS after the unmapped read (ring 0, code 1) ; RES+1 = write denied (code 4) ; RES+2 = execute denied (code 2) ;
    # RES+3 = $EE + 3 faults ; RES+4 = the replayed load got its data (the page mapped at the fault is internal RAM, zero) ; RES+7 = done
    return [('page faults replayed after the handler', pf_prog(), bytes([0x01, 0x04, 0x02, 0xF1, 0x00, 0xEE, 0xEE, 0x5A,
                                                                       0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x60, 0x00,
                                                                       0x00, 0x00, 0x70, 0x00, 0x40]))]


def build(asm):
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'p.asm'), 'w').write(asm)
    r = subprocess.run([A09, '-oF09', '-S', 'p.asm'], cwd=tmp, capture_output=True, text=True)
    if not os.path.exists(os.path.join(tmp, 'p.s09')):
        return None, 'a09 failed %s' % (r.stdout + r.stderr)[:400]
    elf = os.path.join(tmp, 'p.elf')
    subprocess.run(['python3', CONV, os.path.join(tmp, 'p.s09'), elf], capture_output=True)
    return tmp, elf


def run_simh(tmp, elf):
    out = os.path.join(tmp, 'simh.bin')
    open(os.path.join(tmp, 's.ini'), 'w').write('set cpu extmem=1\nload %s\nreset\ngo\ndump %s %X-%X\n' % (elf, out, RESULT, RESULT + NRES - 1))
    subprocess.run([SIM, os.path.join(tmp, 's.ini')], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120)
    return open(out, 'rb').read() if os.path.exists(out) else None


def run_rtl(tmp, elf):
    r = subprocess.run([RTL, '--ncpus', '1', '--elf', elf, '--ticks', '30000000', '--peek', '0x%X:%d' % (RESULT, NRES)],
                       capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=600)
    txt = r.stdout + r.stderr
    hexb = []
    for line in txt.splitlines():
        parts = line.split(':', 1)
        if len(parts) == 2 and len(parts[0]) in (9, 10) and all(c in '0123456789ABCDEFabcdef' for c in parts[0]):
            hexb += [None if t == '--' else int(t, 16) for t in parts[1].split()]
    return bytes(hexb[:NRES]) if len(hexb) >= NRES and None not in hexb[:NRES] else (None, txt[-300:])


bad = 0
for name, f in SCENARIOS.items():
    if names and name not in names:
        continue
    for label, asm, want in f():
        tmp, elf = build(asm)
        if tmp is None:
            print('%-40s %s' % (label, elf)); bad += 1; continue
        a = run_simh(tmp, elf)
        ok = a is not None and a[:len(want)] == want
        msg = ''
        if use_rtl:
            b = run_rtl(tmp, elf)
            if isinstance(b, tuple):
                msg = ' RTL run failed'
            else:
                msg = ' rtl %s %s' % (b[:len(want)].hex(), 'same' if b[:len(want)] == a[:len(want)] else 'DIFFERENT')
        if verbose or not ok:
            print('%-40s %s  got %s want %s%s' % (label, 'OK  ' if ok else 'FAIL', a[:len(want)].hex() if a else None, want.hex(), msg))
        else:
            print('%-40s OK  %s' % (label, msg))
        bad += 0 if ok else 1
sys.exit(1 if bad else 0)
