#!/usr/bin/env python3
"""run_idx.py [-v] : scaled index addressing R*n,B (postbyte %1RR01111 + byte S I sss rrr, 63F09 only) on this simulator.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

idx/gen_idx.py writes 1985 cases (8 registers, signed and unsigned, scales 1 to 16, bases X Y U S, LEA direct and indirect, memory
accesses) and their expected values (independent Python model).  The program compares on the target (idx/idx_main.c) and prints
"OK 000007C1 checks, 00000000".  Run here, and on soc63f09 too when EMU names a binary built from a VHDL that has the mode (the
prototype of work_63f09/idx_scaled_20261119).  Needs the new as-new (AS, default ~/cvs/gcc/binutils-gdb/build/gas/as-new).
Environment: CC (default m6809-unknown-elf-gcc), AS, LD, EMU.
"""
import os, re, shlex, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
BIN = os.path.expanduser('~/cvs/gcc/binutils-gdb/build')
CC = shlex.split(os.environ.get('CC', 'm6809-unknown-elf-gcc'))
AS = os.environ.get('AS', BIN + '/gas/as-new')
LD = os.environ.get('LD', BIN + '/ld/ld-new')
EMU = os.environ.get('EMU')
tmp = tempfile.mkdtemp()

subprocess.check_call([sys.executable, os.path.join(HERE, 'idx', 'gen_idx.py')], cwd=tmp, stdout=subprocess.DEVNULL)
for flags in (['-mno-vindex'], []):          # the harness itself must not use the new forms
    r = subprocess.run(CC + ['-m63f09hf', '-O2', '-msoc'] + flags + ['-I', tmp, '-c', os.path.join(HERE, 'idx', 'idx_main.c'),
                       '-o', tmp + '/main.o'], capture_output=True)
    if r.returncode == 0:
        break
else:
    sys.exit('cannot compile idx_main.c: ' + r.stderr.decode())
subprocess.check_call([AS, '-m63f09hf', tmp + '/idx_all.s', '-o', tmp + '/idx_all.o'])
crt0 = subprocess.check_output(CC + ['-m63f09hf', '-msoc', '-print-file-name=crt0-soc.o']).decode().strip()
libgcc = subprocess.check_output(CC + ['-m63f09hf', '-msoc', '-print-file-name=libgcc.a']).decode().strip()
subprocess.check_call([LD, '-m', 'm6809soc', '-e', '_start', '-nostdlib', crt0, tmp + '/main.o', tmp + '/idx_all.o', libgcc,
                       '-o', tmp + '/idx.elf'])
ini = tmp + '/idx.ini'
open(ini, 'w').write('load %s/idx.elf\nreset\ngo\n' % tmp)
ok = True
out = subprocess.run([SIM, ini], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=600).stdout
m = re.search(r'^(OK|FAIL) [0-9A-F]{8} checks, [0-9A-F]{8}$', out, re.M)
print('simulator:', m.group(0) if m else 'no result')
ok = ok and bool(m) and m.group(1) == 'OK'
if EMU:
    out = subprocess.run([EMU, '--elf', tmp + '/idx.elf', '--acia', '0,baud=1500000,out=-', '--ticks', '2000000000'],
                         capture_output=True, text=True).stdout
    m = re.search(r'^(OK|FAIL) [0-9A-F]{8} checks, [0-9A-F]{8}$', out, re.M)
    print('soc63f09 :', m.group(0) if m else 'no result')
    ok = ok and bool(m) and m.group(1) == 'OK'
sys.exit(0 if ok else 1)
