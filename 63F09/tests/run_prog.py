#!/usr/bin/env python3
"""run_prog.py [-v] name [name...] : a program of ~/vhdl/63F09/regress on the RTL emulator and on this simulator, watched memory compared.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

name = an entry of regress/tests_*.json (asm, duration_us and watched range are taken from it).  The program is assembled with A09,
converted to ELF, run by soc63f09 for duration_us and by BIN/m63f09 until it branches to itself ; the watched bytes are compared.
usage: run_prog.py [-v] dir_mulou dir_mulflags ...
"""
import glob, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
EMU = os.path.expanduser('~/cvs/gcc/github/63F09-emulator/build/soc63f09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')
REG = os.path.expanduser('~/vhdl/63F09/regress')
names = [a for a in sys.argv[1:] if not a.startswith('-')]
tests = {}
for f in glob.glob(REG + '/tests_*.json') + glob.glob(REG + '/proto/dmcnew/tests_*.json'):
    try:
        for e in json.load(open(f)):
            tests.setdefault(e['name'], e)
    except Exception:
        pass


def sh(cmd, timeout=300, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=timeout, **kw)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(cmd, 124, '', 'TIMEOUT')


bad = 0
for name in names:
    e = tests.get(name)
    if not e:
        print('%-16s unknown test' % name); bad += 1; continue
    lo, hi = [int(x, 16) for x in e['watched'].split('-')]
    size = hi - lo + 1
    tmp = tempfile.mkdtemp()
    asm = os.path.join(REG, 'asm', e['asm'] + '.asm')
    sh(['cp', '-f', asm, tmp])
    sh([A09, '-o' + e.get('a09', 'F09'), '-S', e['asm'] + '.asm'], cwd=tmp)
    s09 = os.path.join(tmp, e['asm'] + '.s09')
    if not os.path.exists(s09):
        print('%-16s a09 failed' % name); bad += 1; continue
    elf = os.path.join(tmp, 'p.elf')
    sh(['python3', CONV, s09, elf])
    ticks = int(e['duration_us'] * 144000 * 1.3) + 5000000
    r = sh([EMU, '--ncpus', '1', '--elf', elf, '--ticks', str(ticks), '--peek', '0x%X:%d' % (lo, size)], timeout=900)
    ref = bytearray()
    for line in r.stdout.splitlines() + r.stderr.splitlines():
        m = re.match(r'([0-9A-F]{9}): ((?:[0-9A-F-]{2} ?)+)', line)
        if m and int(m.group(1), 16) >= lo:
            ref += bytes(0 if t == '--' else int(t, 16) for t in m.group(2).split())
    ref = bytes(ref[:size])
    ini = os.path.join(tmp, 's.ini'); out = os.path.join(tmp, 'sim.bin')
    open(ini, 'w').write('load %s\nboot cpu\ngo\ndump %s %x-%x\n' % (elf, out, lo, hi))
    rs = sh([SIM, ini], timeout=120)
    got = open(out, 'rb').read() if os.path.exists(out) else b''
    msg = [l for l in rs.stdout.splitlines() if 'Not implemented' in l or 'Nonexistent' in l or 'Branch to self' in l]
    if rs.returncode == 124:
        msg = ['simulator TIMEOUT']
    if len(ref) != size or len(got) != size:
        print('%-16s RUN FAILED (reference %d bytes, simulator %d, %d expected) %s' % (name, len(ref), len(got), size, msg[:1])); bad += 1; continue
    if not any(ref) or not any(got) or not any('Branch to self' in l for l in msg):
        # an area that stayed zero, or a simulator that did not reach the end of the program, proves nothing
        print('%-16s PROBLEM: reference %s, simulator %s, stop: %s' % (name, 'non-zero' if any(ref) else 'all zero', 'non-zero' if any(got) else 'all zero', msg[:1]))
        bad += 1
        continue
    diff = [i for i in range(size) if ref[i] != got[i]]
    if diff:
        bad += 1
    print('%-16s %s  %d bytes, %d differ  %s' % (name, 'OK  ' if not diff else 'DIFF', size, len(diff), (msg[0][:80] if msg else '')))
    if diff and '-v' in sys.argv:
        print('   first differences at +%s: ref %s sim %s' % (hex(diff[0]), ref[diff[0]:diff[0] + 8].hex(), got[diff[0]:diff[0] + 8].hex()))
print('%d programs, %d with a problem' % (len(names), bad))
