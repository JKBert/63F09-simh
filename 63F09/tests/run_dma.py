#!/usr/bin/env python3
"""run_dma.py [-v] : DMA 63F44 (m63f09_dma.c), 32-bit mode, on the simulator only.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

A program programs the registers of the DMA (control page $FFFE8000, channel c at $FFFE7000 - c x $1000), waits for the channel, stores the
STATE it read at $FFFC0100 + c, and the script compares the external memory ($F00001000...) with a Python model of the copy.  Status codes:
$02 done, $14 cause 1 (source), $24 cause 2 (destination), $34 cause 3 (count / mode), $44 cause 4 (chain), $54 cause 5 (bus), $64 cause 6
(abort), $74 cause 7 (disabled).
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
verbose = '-v' in sys.argv

CTRL = 0xFFFE8000
def CH(c): return 0xFFFE7000 - c * 0x1000
E = 0xF00000000                                         # physical address of the external memory
MEMLO, MEMHI = E, E + 0x10000000                        # memory window used unless a test says otherwise
SRC0, DST0, SIZE = 0x1000, 0x3000, 0x400                # regions of the external memory used by the tests (offsets), size of each
OUT = 0xFFFC0100


def wr(addr, val):
    return '\t\tLDA\t\t#$%02X\n\t\tSTA\t\t>$%08X\n' % (val & 0xFF, addr)

def w36(base, v):
    return ''.join(wr(base + i, (v >> (56 - 8 * i)) & 0xFF) for i in range(8))

def w32(base, v):
    return ''.join(wr(base + i, (v >> (24 - 8 * i)) & 0xFF) for i in range(4))

def setup_ctrl(enable=True, mem=(MEMLO, MEMHI), dev=(0, 0)):
    s = w36(CTRL + 0x08, mem[0]) + w36(CTRL + 0x10, mem[1]) + w36(CTRL + 0x18, dev[0]) + w36(CTRL + 0x20, dev[1])
    return s + wr(CTRL + 0, 1 if enable else 0)

def prog_ch(c, src, dst, cnt, sm=0, dm=0, burst=False, chain=False, ie=False, go=True, nxt=None):
    ctl = (2 if burst else 0) | (sm << 2) | (dm << 4) | (0x40 if chain else 0) | (0x80 if ie else 0)
    s = w36(CH(c) + 0x00, src) + w36(CH(c) + 0x08, dst) + w32(CH(c) + 0x10, cnt)
    if nxt:
        s += w36(CH(c) + 0x20, nxt[0]) + w36(CH(c) + 0x28, nxt[1]) + w32(CH(c) + 0x30, nxt[2])
    return s + wr(CH(c) + 0x14, ctl | (1 if go else 0))

def wait_ch(c):
    return ('W%d\t\tLDA\t\t>$%08X\n\t\tSTA\t\t>$%08X\n\t\tBITA\t#$01\n\t\tBNE\t\tW%d\n' % (c, CH(c) + 0x15, OUT + c, c))

def copy(mem, src, dst, cnt, sm=0, dm=0):
    """Model of the copy on a dict-like bytearray indexed by physical offset from E"""
    s, d = src, dst
    for _ in range(cnt):
        mem[d] = mem[s]
        s += 1 if sm == 0 else (-1 if sm == 1 else 0)
        d += 1 if dm == 0 else (-1 if dm == 1 else 0)

def fresh():
    m = bytearray(0x4000)
    for i in range(SIZE):
        m[SRC0 + i] = i & 0xFF
        m[DST0 + i] = 0xEE
    return m

TESTS = []
def test(name, body, model, status):
    TESTS.append((name, body, model, status))

def ma(f):
    def g():
        m = fresh()
        f(m)
        return m
    return g

# ---- the tests --------------------------------------------------------------------------------------------------------------------------
test('copy 1000 bytes, aligned (2 blocks)', setup_ctrl() + prog_ch(0, E + SRC0, E + DST0, 0x3E8) + wait_ch(0),
     ma(lambda m: copy(m, SRC0, DST0, 0x3E8)), {0: 0x02})
test('copy 37 bytes, unaligned (src+3, dst+5)', setup_ctrl() + prog_ch(0, E + SRC0 + 3, E + DST0 + 5, 37) + wait_ch(0),
     ma(lambda m: copy(m, SRC0 + 3, DST0 + 5, 37)), {0: 0x02})
test('source decrement', setup_ctrl() + prog_ch(0, E + SRC0 + 15, E + DST0, 16, sm=1) + wait_ch(0),
     ma(lambda m: copy(m, SRC0 + 15, DST0, 16, sm=1)), {0: 0x02})
test('destination decrement', setup_ctrl() + prog_ch(0, E + SRC0, E + DST0 + 15, 16, dm=1) + wait_ch(0),
     ma(lambda m: copy(m, SRC0, DST0 + 15, 16, dm=1)), {0: 0x02})
test('fixed source (fill)', setup_ctrl() + prog_ch(0, E + SRC0 + 9, E + DST0, 40, sm=2) + wait_ch(0),
     ma(lambda m: copy(m, SRC0 + 9, DST0, 40, sm=2)), {0: 0x02})
test('fixed destination (device-like)', setup_ctrl() + prog_ch(0, E + SRC0, E + DST0, 20, dm=2) + wait_ch(0),
     ma(lambda m: copy(m, SRC0, DST0, 20, dm=2)), {0: 0x02})
test('burst mode, 700 bytes', setup_ctrl() + prog_ch(0, E + SRC0, E + DST0, 0x2BC, burst=True) + wait_ch(0),
     ma(lambda m: copy(m, SRC0, DST0, 0x2BC)), {0: 0x02})
test('cause 1: source outside the windows', setup_ctrl(mem=(E, E + 0x2000)) + prog_ch(0, E + 0x5000, E + DST0, 16) + wait_ch(0),
     ma(lambda m: None), {0: 0x14})
test('cause 2: destination outside the windows', setup_ctrl(mem=(E, E + 0x2000)) + prog_ch(0, E + SRC0, E + DST0, 16) + wait_ch(0),
     ma(lambda m: None), {0: 0x24})
test('cause 3: count 0', setup_ctrl() + prog_ch(0, E + SRC0, E + DST0, 0) + wait_ch(0), ma(lambda m: None), {0: 0x34})
test('cause 3: reserved mode', setup_ctrl() + prog_ch(0, E + SRC0, E + DST0, 8, sm=3) + wait_ch(0), ma(lambda m: None), {0: 0x34})
test('cause 3: decrement past address 0', setup_ctrl(mem=(0, E + 0x10000000)) + prog_ch(0, 3, E + DST0, 8, sm=1) + wait_ch(0),
     ma(lambda m: None), {0: 0x34})
test('cause 5: bus error (destination in the internal RAM)',
     setup_ctrl(mem=(0, 0xFFFFFFFFF)) + prog_ch(0, E + SRC0, 0xFFFFC0200, 16) + wait_ch(0), ma(lambda m: None), {0: 0x54})
test('cause 7: not enabled', setup_ctrl(enable=False) + prog_ch(0, E + SRC0, E + DST0, 16) + wait_ch(0), ma(lambda m: None), {0: 0x74})
test('abort of a long transfer', setup_ctrl(mem=(E, E + 0x10000000)) + prog_ch(0, E + 0x100000, E + 0x200000, 0x80000) +
     wr(CTRL + 0, 3) + wait_ch(0), ma(lambda m: None), {0: 0x64})
test('two channels, round robin', setup_ctrl() + prog_ch(0, E + SRC0, E + DST0, 0x300) + prog_ch(1, E + SRC0 + 0x300, E + DST0 + 0x300, 0x80)
     + wait_ch(0) + wait_ch(1), ma(lambda m: (copy(m, SRC0, DST0, 0x300), copy(m, SRC0 + 0x300, DST0 + 0x300, 0x80))), {0: 0x02, 1: 0x02})
test('chaining (second transfer from the shadow registers)',
     setup_ctrl() + prog_ch(0, E + SRC0, E + DST0, 100, chain=True, nxt=(E + SRC0 + 0x100, E + DST0 + 0x100, 50)) + wait_ch(0),
     ma(lambda m: (copy(m, SRC0, DST0, 100), copy(m, SRC0 + 0x100, DST0 + 0x100, 50))), {0: 0x02})
test('chaining with an invalid second transfer (cause 4)',
     setup_ctrl(mem=(E, E + 0x4000)) + prog_ch(0, E + SRC0, E + DST0, 20, chain=True, nxt=(E + 0x5000, E + 0x100, 8)) + wait_ch(0),
     ma(lambda m: copy(m, SRC0, DST0, 20)), {0: 0x44})

PRELUDE = ('\t\tORG\t\t$FFFF0010\n\t\tLDMD\t#$31\n\t\tLDS\t\t#$FFFD0000\n'
           '\t\tLDX\t\t#$%08X\n\t\tCLRB\nF1\t\tSTB\t\t,X+\n\t\tINCB\n\t\tCMPX\t#$%08X\n\t\tBNE\t\tF1\n'
           '\t\tLDX\t\t#$%08X\n\t\tLDA\t\t#$EE\nF2\t\tSTA\t\t,X+\n\t\tCMPX\t#$%08X\n\t\tBNE\t\tF2\n' % (SRC0, SRC0 + SIZE, DST0, DST0 + SIZE))
VECT = '\t\tORG\t\t$FFFFFFC0\n' + '\n'.join('\t\tFDB\t\t%s' % ('$0010' if i == 0x1f else '$0000') for i in range(32)) + '\n\t\tEND\n'

def run(name, asm, ini_extra=()):
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'p.asm'), 'w').write(asm)
    r = subprocess.run([A09, '-oF09', '-S', 'p.asm'], cwd=tmp, capture_output=True, text=True)
    if not os.path.exists(os.path.join(tmp, 'p.s09')):
        return None, None, 'a09 failed %s' % (r.stdout + r.stderr)[:400], tmp
    elf = os.path.join(tmp, 'p.elf')
    subprocess.run(['python3', CONV, os.path.join(tmp, 'p.s09'), elf], capture_output=True)
    o1, o2 = os.path.join(tmp, 'res.bin'), os.path.join(tmp, 'ext.bin')
    ini = ['set cpu extmem=4', 'load %s' % elf, 'reset'] + list(ini_extra) + [
        'go', 'dump %s FFFFC0100-FFFFC0108' % o1, 'dump %s F00000000-F00003FFF' % o2]
    open(os.path.join(tmp, 's.ini'), 'w').write('\n'.join(ini) + '\n')
    rs = subprocess.run([SIM, os.path.join(tmp, 's.ini')], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120)
    res = open(o1, 'rb').read() if os.path.exists(o1) else b''
    ext = open(o2, 'rb').read() if os.path.exists(o2) else b''
    return res, ext, rs.stdout[-300:], tmp

bad = 0
for name, body, model, status in TESTS:
    asm = PRELUDE + body + 'END\t\tBRA\t\tEND\n' + VECT
    res, ext, info, tmp = run(name, asm)
    if verbose: print('tmp', tmp)
    if res is None:
        print('%-52s FAIL %s' % (name, info)); bad += 1; continue
    want_ext = bytes(model())
    ok_st = all(len(res) > c and res[c] == v for c, v in status.items())
    ok_ext = ext[:len(want_ext)] == want_ext
    ok = ok_st and ok_ext
    why = ''
    if not ok_st: why += ' status got %s want %s' % ([hex(b) for b in res[:max(status) + 1]], {c: hex(v) for c, v in status.items()})
    if not ok_ext:
        first = next((i for i in range(len(want_ext)) if i >= len(ext) or ext[i] != want_ext[i]), None)
        why += ' memory differs at +%X (got %s want %02X)' % (first, '%02X' % ext[first] if first is not None and first < len(ext) else 'none',
                                                           want_ext[first] if first is not None else 0)
    print('%-52s %s%s' % (name, 'OK  ' if ok else 'FAIL', why + ('\n' + info if not ok and why == '' else '')))
    bad += 0 if ok else 1

# interrupt : IE on channel 0, output wired to EIRQ3, the handler reads STATE (acknowledges) and counts at $FFFC0108
HANDLER = ('\t\tORG\t\t$FFFF02C0\n\t\tLDA\t\t>$%08X\n\t\tSTA\t\t>$%08X\n\t\tINC\t\t>$FFFC0108\n\t\tRTI\n' % (CH(0) + 0x15, OUT))
vect = VECT.replace('FDB\t\t$0000', 'FDB\t\t$0000', 1)
lines = VECT.split('\n')
lines[1 + 3] = '\t\tFDB\t\t$02C0'
body = (setup_ctrl() + wr(0xFFFC0108, 0) + '\t\tANDCC\t#$EF\n' + prog_ch(0, E + SRC0, E + DST0, 100, ie=True) +
        'WI\t\tLDA\t\t>$FFFC0108\n\t\tBEQ\t\tWI\n')
asm = PRELUDE + body + 'END\t\tBRA\t\tEND\n' + HANDLER + '\n'.join(lines)
res, ext, info, tmp = run('irq', asm, ['dep DMA IRQLINE 3'])
m = fresh(); copy(m, SRC0, DST0, 100)
ok = res is not None and len(res) >= 9 and res[8] == 1 and res[0] == 0x02 and ext[:len(m)] == bytes(m)
print('%-52s %s' % ('interrupt on EIRQ3, acknowledged by reading STATE', 'OK  ' if ok else 'FAIL got %s %s' % (list(res[:9]) if res else res, info)))
bad += 0 if ok else 1
sys.exit(1 if bad else 0)
