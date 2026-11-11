#!/usr/bin/env python3
"""run_acia.py : ACIA0 input (stdin through a pipe) and the receive interrupt (EIRQ23), on the simulator only.

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Two programs: a polling echo (waits for RDRF, TDRE) and an interrupt echo (RIE set, the handler of EIRQ23 reads and sends the byte).
Both stop on a 'q'.  The text is fed through a pipe: SCP polls the console only when stdin is a terminal, the model reads a non-terminal
stdin itself.  Shows the characters sent by the program (the echo) and checks them.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, '..', '..', 'BIN', 'm63f09')
A09 = os.path.expanduser('~/cvs/gcc/binutils-gdb/test/a09_reference/a09')
CONV = os.path.join(HERE, '..', 'tools', 's09_to_elf.py')

SETUP = '\t\tLDX\t\t#$FFFEE000\n\t\tCLR\t\t2,X\n\t\tLDA\t\t#12\n\t\tSTA\t\t3,X\n'     # divisor 12: 115200 baud at 24 MHz
VECT = lambda h: ''.join('\t\tFDB\t\t%s\n' % ({23: h, 31: '$0010'}.get(i, '$0000')) for i in range(32))
POLL = ('\t\tORG\t\t$FFFF0010\n\t\tLDMD\t#$11\n\t\tLDS\t\t#$FFFD0000\n' + SETUP + '\t\tLDA\t\t#$15\n\t\tSTA\t\t,X\n'
        'WAIT\tLDA\t\t,X\n\t\tBITA\t#$01\n\t\tBEQ\t\tWAIT\n\t\tLDB\t\t1,X\n'
        'TX\t\tLDA\t\t,X\n\t\tBITA\t#$02\n\t\tBEQ\t\tTX\n\t\tSTB\t\t1,X\n\t\tCMPB\t#\'q\'\n\t\tBNE\t\tWAIT\nEND\t\tBRA\t\tEND\n'
        '\t\tORG\t\t$FFFFFFC0\n' + VECT('$0000') + '\t\tEND\n')
IRQ = ('\t\tORG\t\t$FFFF0010\n\t\tLDMD\t#$11\n\t\tCLRA\n\t\tSTA\t\t>$FFFC0300\n\t\tLDS\t\t#$FFFD0000\n' + SETUP +
       '\t\tLDA\t\t#$95\n\t\tSTA\t\t,X\nLOOP\tLDA\t\t>$FFFC0300\n\t\tCMPA\t#\'q\'\n\t\tBNE\t\tLOOP\nEND\t\tBRA\t\tEND\n'
       '\t\tORG\t\t$FFFF0100\n\t\tLDX\t\t#$FFFEE000\n\t\tLDB\t\t1,X\n\t\tSTB\t\t>$FFFC0300\n'
       'HT\t\tLDA\t\t,X\n\t\tBITA\t#$02\n\t\tBEQ\t\tHT\n\t\tSTB\t\t1,X\n\t\tRTI\n'
       '\t\tORG\t\t$FFFFFFC0\n' + VECT('$0100') + '\t\tEND\n')

bad = 0
for name, prog, text in (('polling echo, piped stdin', POLL, 'bonjour 63F09 q'), ('interrupt echo (EIRQ23), piped stdin', IRQ, 'abc 63F09 q')):
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, 'p.asm'), 'w').write(prog)
    subprocess.run([A09, '-oF09', '-S', 'p.asm'], cwd=tmp, capture_output=True)
    elf = os.path.join(tmp, 'p.elf')
    subprocess.run(['python3', CONV, os.path.join(tmp, 'p.s09'), elf], capture_output=True)
    open(os.path.join(tmp, 's.ini'), 'w').write('load %s\nreset\ngo\nquit\n' % elf)
    r = subprocess.run([SIM, os.path.join(tmp, 's.ini')], input=text, capture_output=True, text=True, timeout=30)
    echoed = text[:-1]
    ok = echoed in r.stdout and 'Branch to self' in r.stdout
    print('%-40s %s' % (name, 'OK' if ok else 'FAIL  ' + repr(r.stdout[-120:])))
    bad += 0 if ok else 1
sys.exit(1 if bad else 0)
