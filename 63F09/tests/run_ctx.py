#!/usr/bin/env python3
"""run_ctx.py [-v] [--emu PATH] : copies of the contexts of a MMU 63F29, by TFM and by the context engine (RTL emulator soc63f09, one core).

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Needs an emulator built from a VHDL tree that has the context engine of the MMU and the context store of the BAC (proto/mmu_final merged
on rtl/, LEGACY_WINDOWS = true : the old windows on the tables are the way to read them back).  The simulator SIMH has no engine, so it does not run
this test.  A 16 MiB memory on the external 64-bit bus starts at the physical address $0:0000:0000 (--ext 0x0:0x1000000) : the program maps
it in ring 0 at the logical address $00100000 and writes the dumps of the tables there.

Part A, by TFM (the method of the manual, "Sauvegarder et restaurer une MMU") : the three windows of the tables of ring 1 (tags $100.., ages $680..,
payloads $A00..) are copied by TFM to the external memory ; the entries are wiped (an entry rewritten with a payload without R and W is free) ; a
second dump shows them free ; the entries are recreated by the normal sequence (tag then payload) ; a third dump must hold the same entries.
Part B, by the engine (the second mechanism, the context store of the BAC) : SAVE of the task A of ring 1, CLEAR for the task B, two entries
for B, SWAP to A : the tables of ring 1 must be byte for byte those of before the SAVE ; SWAP to B : those of B ; a LOAD of an unknown task
gives MISS and changes nothing.
Not covered : the copy of a context from the store to the main memory by the DMA 63F44 : the master side of the DMA is not connected yet.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
EMU = os.environ.get('SOC63F09') or os.path.expanduser('~/cvs/gcc/github/63F09-emulator/build/soc63f09')
verbose = '-v' in sys.argv
if '--emu' in sys.argv:
    EMU = sys.argv[sys.argv.index('--emu') + 1]

AM, X, W, R = 8, 4, 2, 1
N = 64                                                  # entries per ring
BUF = 0x00100000                                        # logical address of the dumps (ring 0), physical $0:0010:0000 + ... below
DUMP = 0x1000                                           # size reserved for one dump (tags 640 + payload 256 + ages 64, rounded)
RES_OFF = 0x10000                                       # the result bytes are in the external memory too (one --peek only) : logical BUF + RES_OFF

EQUS = '''MMU0        EQU     $FFFD0000
RING        EQU     MMU0
STATUS      EQU     MMU0+2
MAXRING     EQU     MMU0+3
TAG_STAGING EQU     MMU0+4
CTX_CMD     EQU     MMU0+$20
CTX_STATUS  EQU     MMU0+$21
TASK_CUR    EQU     MMU0+$2D
TASK_NEXT   EQU     MMU0+$2F
IND_RING0   EQU     MMU0+$800
IND_RING1   EQU     MMU0+$A00
W_TAGS      EQU     MMU0+$100
W_AGES1     EQU     MMU0+$680
W_PAY1      EQU     MMU0+$A00
RES         EQU     $00110000
'''

VECT = '\t\tORG\t\t$FFFFFFC0\n' + '\n'.join('\t\tFDB\t\t%s' % ('$0010' if i == 0x1f else '$0000') for i in range(32)) + '\n\t\tEND\n'


def mapping(ring, logical, ppn, flags):
    return ('\t\tLDQ\t\t#$%08X\n\t\tSTQ\t\t>TAG_STAGING\n\t\tLDQ\t\t#$%08X\n\t\tSTQ\t\t>IND_RING%d\n'
            % (logical & 0xFFFFF000, (flags << 24) | (ppn & 0xFFFFFF), ring))


def dump(slot):
    """TFM of the windows of the tables of ring 1 to the dump number `slot` of the external memory."""
    dst = BUF + slot * DUMP
    s = ''
    for src, off, n in (('W_TAGS', 0x000, 10 * N), ('W_PAY1', 0x400, 4 * N), ('W_AGES1', 0x500, N)):
        s += '\t\tLDX\t\t#%s\n\t\tLDY\t\t#$%08X\n\t\tLDW\t\t#%d\n\t\tTFM\t\tX+,Y+\n' % (src, dst + off, n)
    return s


RING1 = [(0x00200000, 0x000020, W | R), (0x00201000, 0x000021, W | R), (0x00202000, 0x000022, R)]
TASK_B = [(0x00300000, 0x000030, R), (0x00301000, 0x000031, W | R)]


def engine(cmd, task=None, cur=None, status_slot=0):
    """Optionally TASK_CUR and TASK_NEXT, then the command, then wait for BUSY = 0 and keep CTX_STATUS and TASK_CUR in RES."""
    s = ''
    if cur is not None:
        s += '\t\tLDD\t\t#$%04X\n\t\tSTD\t\t>TASK_CUR\n' % cur
    if task is not None:
        s += '\t\tLDD\t\t#$%04X\n\t\tSTD\t\t>TASK_NEXT\n' % task
    lab = 'W%d' % status_slot
    s += '\t\tLDA\t\t#%d\n\t\tSTA\t\t>CTX_CMD\n' % cmd
    s += '%s\t\tLDA\t\t>CTX_STATUS\n\t\tBITA\t#$01\n\t\tBNE\t\t%s\n' % (lab, lab)
    s += '\t\tSTA\t\t>RES+%d\n\t\tLDD\t\t>TASK_CUR\n\t\tSTD\t\t>RES+%d\n' % (status_slot, 16 + 2 * status_slot)
    return s


def program():
    b = '\t\tORG\t\t$FFFF0010\n\t\tLDMD\t#$31\n\t\tLDS\t\t#$FFFD0000\n'
    # ring 0 : code, result area, stack, vectors, the external memory (8 pages) ; ring 1 : its entries
    b += mapping(0, 0xFFFF0000, 0xFFFFF0, AM | X | R) + mapping(0, 0xFFFC0000, 0xFFFFC0, AM | W | R)
    b += mapping(0, 0xFFFCF000, 0xFFFFCF, AM | W | R) + mapping(0, 0xFFFFF000, 0xFFFFFF, AM | R)
    for i in range(32):
        b += mapping(0, BUF + 0x1000 * i, i, AM | W | R)
    for t, p, f in RING1:
        b += mapping(1, t, p, f)
    b += '\t\tLDA\t\t#$80\n\t\tSTA\t\t>RING\n'
    b += '\t\tLDA\t\t#$EE\n' + ''.join('\t\tSTA\t\t>RES+%d\n' % i for i in range(32))
    # part A
    b += dump(0)
    for t, p, f in RING1:                               # wipe : the same tag with a payload that has neither R nor W
        b += '\t\tLDQ\t\t#$%08X\n\t\tSTQ\t\t>TAG_STAGING\n\t\tCLRD\n\t\tCLRW\n\t\tSTQ\t\t>IND_RING1\n' % t
    b += dump(1)
    for t, p, f in RING1:
        b += mapping(1, t, p, f)
    b += dump(2)
    # part B
    b += dump(3)
    b += engine(1, cur=0x4001, status_slot=0)                       # SAVE the task A
    b += engine(5, task=0x4002, status_slot=1)                      # CLEAR for the task B (TASK_CUR = B)
    b += dump(4)
    for t, p, f in TASK_B:
        b += mapping(1, t, p, f)
    b += dump(5)                                                    # tables of B
    b += engine(3, task=0x4001, status_slot=2)                      # SWAP to A : B is saved, A is loaded
    b += dump(6)
    b += engine(3, task=0x4002, status_slot=3)                      # SWAP to B
    b += dump(7)
    b += engine(2, task=0x4009, status_slot=4)                      # LOAD of an unknown task : MISS
    b += dump(8)
    b += '\t\tLDA\t\t#$5A\n\t\tSTA\t\t>RES+15\nEND\t\tBRA\t\tEND\n'
    return b + EQUS + VECT


def tags_of(blob):
    """The 64 entries of ring 1 from the flat stream of tags : [(index, tag)]."""
    out = {}
    for ring in range(4):
        for pair in range(N // 2):
            o = (ring * (N // 2) + pair) * 5
            b = blob[o:o + 5]
            out[(ring, 2 * pair)] = (b[0] << 12) | (b[1] << 4) | (b[2] >> 4)
            out[(ring, 2 * pair + 1)] = ((b[2] & 0xF) << 16) | (b[3] << 8) | b[4]
    return out


def entries(blob_all, slot):
    base = slot * DUMP
    tags = tags_of(blob_all[base:base + 10 * N])
    pay = blob_all[base + 0x400:base + 0x400 + 4 * N]
    s = set()
    for i in range(N):
        p = int.from_bytes(pay[4 * i:4 * i + 4], 'big')
        if p & ((W | R) << 24):
            s.add((tags[(1, i)] << 12, p))
    return s


def run(asm):
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'p.asm'), 'w').write(asm)
    r = subprocess.run([A09, '-oF09', '-S', 'p.asm'], cwd=tmp, capture_output=True, text=True)
    if not os.path.exists(os.path.join(tmp, 'p.s09')):
        return None, 'a09 failed ' + (r.stdout + r.stderr)[:600]
    subprocess.run(['python3', CONV, os.path.join(tmp, 'p.s09'), os.path.join(tmp, 'p.elf')], capture_output=True)
    r = subprocess.run([EMU, '--ncpus', '1', '--ext', '0x0:0x1000000', '--elf', os.path.join(tmp, 'p.elf'), '--ticks', '300000000',
                        '--peek', '0x0:%d' % (RES_OFF + 32)],
                       capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=3000)
    return tmp, r.stdout + r.stderr


def parse(txt):
    """The blocks printed by --peek, in order."""
    blocks, cur = [], None
    for line in txt.splitlines():
        parts = line.split(':', 1)
        if len(parts) == 2 and len(parts[0].strip()) in (9, 10) and all(c in '0123456789ABCDEFabcdef' for c in parts[0].strip()):
            a = int(parts[0], 16)
            if cur is None or a != cur[1]:
                cur = [a, a, bytearray()]
                blocks.append(cur)
            for t in parts[1].split():
                cur[2].append(0xFF if t == '--' else int(t, 16))
                cur[1] += 1
    return [bytes(b[2]) for b in blocks]


def main():
    tmp, txt = run(program())
    if tmp is None:
        print(txt); return 1
    if verbose:
        print(txt[-1500:])
    blocks = parse(txt)
    if len(blocks) < 1 or len(blocks[0]) < RES_OFF + 32:
        print('no memory dump in the output :', txt[-400:]); return 1
    mem = blocks[0]
    res = mem[RES_OFF:RES_OFF + 32]
    bad = 0

    def check(name, ok, info=''):
        nonlocal bad
        print('%-62s %s %s' % (name, 'OK' if ok else 'FAIL', '' if ok else info))
        bad += 0 if ok else 1
    check('program ran to its end (marker $5A)', res[15] == 0x5A, 'RES = %s' % res.hex())
    e = lambda s: entries(mem, s)
    want = {(t, (f << 24) | p) for t, p, f in RING1}
    wantb = {(t, (f << 24) | p) for t, p, f in TASK_B}
    check('A  dump 1 : the three entries of ring 1', e(0) == want, '%s' % sorted(e(0)))
    check('A  dump 2 : wiped (no entry with R or W)', e(1) == set(), '%s' % sorted(e(1)))
    check('A  dump 3 : recreated, same entries', e(2) == want, '%s' % sorted(e(2)))
    check('B  dump 4 : same entries before the SAVE', e(3) == want)
    check('B  dump 5 : ring 1 empty after CLEAR (task B)', e(4) == set(), '%s' % sorted(e(4)))
    check('B  dump 6 : the two entries of B', e(5) == wantb, '%s' % sorted(e(5)))
    same = lambda a, b: mem[a * DUMP:a * DUMP + 0x540] == mem[b * DUMP:b * DUMP + 0x540]
    check('B  SWAP to A : tables identical to those of the SAVE (tags, payloads, ages)', same(3, 6),
          'tags %s payload %s ages %s' % tuple(mem[3 * DUMP + o:3 * DUMP + o + n] == mem[6 * DUMP + o:6 * DUMP + o + n]
                                                 for o, n in ((0, 640), (0x400, 256), (0x500, 64))))
    check('B  SWAP to B : tables identical to those of B before', e(7) == wantb and same(5, 7))
    check('B  LOAD of an unknown task : tables unchanged', same(7, 8))
    st = res[0:5]
    check('B  CTX_STATUS DONE and no REFUSED / ERROR after SAVE, CLEAR, SWAP, SWAP',
          all((s & 0x02) and not (s & 0xC0) for s in st[0:4]), 'status = %s' % st.hex())
    check('B  CTX_STATUS MISS after the LOAD of an unknown task', bool(st[4] & 0x10), 'status = %02X' % st[4])
    tc = [int.from_bytes(res[16 + 2 * i:18 + 2 * i], 'big') for i in range(5)]
    check('B  TASK_CUR : A (SAVE), B (CLEAR), A, B, B', tc == [0x4001, 0x4002, 0x4001, 0x4002, 0x4002], '%s' % ['%04X' % t for t in tc])
    print('dumps kept in', tmp)
    print('%d failure(s)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
