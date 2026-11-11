#!/usr/bin/env python3
"""gen_tfmcc.py : the condition codes right after TFM, W in {0,1,2,6}, 5 initial CC, 4 variants (80 cases).

  Copyright (C) 2026 Dr. BERTRAND Joël, Claude

Writes tfmcc.asm (A09, F09) : each case loads X, Y, W and CC, runs the TFM, reads CC straight away (TFR CC,A) and stores it at
RES+n ($FFFC0400).  Run it on soc63f09 and on the simulator like run_isa.py does and compare the 80 bytes.  Result (2026-11-10): the
two agree ; with W >= 1 the final CC has Z = 1 and N = V = C = 0 (the decrement W - 1 reloads the flags, last value 0) and H, I, F, E
unchanged ; with W = 0 the CC is unchanged.
"""
cases = [(w, cc, v) for w in (0, 1, 2, 6) for cc in (0x00, 0x0F, 0x2A, 0x05, 0x34) for v in ('x+,y+', 'x-,y-', 'x+,y', 'x,y+')]
L = ['\t\tORG\t\t$FFFF0010', '\t\tLDMD\t#$31', '\t\tLDS\t\t#$FFFD0000', '\t\tLDD\t\t#$FFFC', '\t\tTFR\t\tD,DS', '\t\tCLRA', '\t\tTFR\t\tA,DP']
for n, (w, cc, v) in enumerate(cases):
    L += ['* case %d : W=%d cc=%02X tfm %s' % (n, w, cc, v), '\t\tLDX\t\t#$FFFC0800', '\t\tLDY\t\t#$FFFC0900', '\t\tLDW\t\t#%d' % w,
          '\t\tLDA\t\t#$%02X' % cc, '\t\tTFR\t\tA,CC', '\t\ttfm\t%s' % v, '\t\tTFR\t\tCC,A', '\t\tSTA\t\t>RES+%d' % n]
L += ['END:', '\t\tBRA\t\tEND', 'RES\t\tEQU\t\t$FFFC0400', '', '\t\tORG\t\t$FFFFFFF0'] + ['\t\tFDB\t\t$0000'] * 7 + ['\t\tFDB\t\t$0010', '', '\t\tEND']
open('tfmcc.asm', 'w').write('\n'.join(L) + '\n')
print(len(cases), 'cases')
