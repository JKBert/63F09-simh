#!/usr/bin/env python3
# Test exhaustif du mode indexe a echelle (postoctet %1RR01111 + octet S I sss rrr).
# Genere idx_all.s (cas) + idx_expect.txt (modele independant). EA = base + (ext(reg) << sc) mod 2^32.
M = 0xFFFFFFFF
REGS = {  # nom: (largeur bits, valeurs de test, instruction de chargement)
  'a': (8,  [0x00, 0x7F, 0x80, 0xFF], 'lda'),
  'b': (8,  [0x00, 0x7F, 0x80, 0xFF], 'ldb'),
  'e': (8,  [0x00, 0x7F, 0x80, 0xFF], 'lde'),
  'f': (8,  [0x00, 0x7F, 0x80, 0xFF], 'ldf'),
  'd': (16, [0x0000, 0x7FFF, 0x8000, 0xFFFF, 0x1234], 'ldd'),
  'w': (16, [0x0000, 0x7FFF, 0x8000, 0xFFFF, 0x1234], 'ldw'),
  'q': (32, [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF], 'ldq'),
  'v': (32, [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF], 'ldv'),
}
BASES = {'x': 0x00100000, 'y': 0x00180000, 'u': 0x00200000, 's': 0x00300000}
DEST = {'x': 'y', 'y': 'x', 'u': 'x', 's': 'x'}
SCALES = [1, 2, 4, 8, 16]
def ext(v, w, signed):
    if w < 32 and signed and (v >> (w - 1)) & 1:
        v -= 1 << w
    return v
out = []; exp = []
n = 0
out.append('\t.text\n\t.globl idx_run\nidx_run:\n\tsts\tsaved_s\n\tstu\tsaved_u')
# --- cas LEA (adresse effective), formes directes
for r, (w, vals, ld) in REGS.items():
    for b, bv in BASES.items():
        for sg in (True, False):
            for sc in SCALES:
                for v in vals:
                    if w == 32 and not sg: continue          # sans effet pour Q et V
                    d = DEST[b]
                    suffix = '' if sg else '(u)'
                    if b == 's': out.append(f'\tlds\t#{bv}')
                    else: out.append(f'\tld{b}\t#{bv}')
                    out.append(f'\t{ld}\t#{v}')
                    out.append(f'\tlea{d}\t{r}{suffix}*{sc},{b}')
                    out.append(f'\tst{d}\tres+{4*n}')
                    if b == 's': out.append('\tlds\tsaved_s')
                    ea = (bv + (ext(v, w, sg) << (sc.bit_length() - 1))) & M
                    exp.append(ea); n += 1
# --- cas LEA indirect : [R*n,B] ; EA = adresse du pointeur, lecture 32 bits dans win (octets (i*7+3)&255), base = win+4096
WIN = 8192
def winbyte(i): return (i * 7 + 3) & 255
IREGS = {'a': (8, [0x00, 0x04, 0xFC, 0x08]), 'd': (16, [0x0000, 0x0004, 0xFFFC, 0x0008]),
         'q': (32, [0, 4, 8, 0xFFFFFFFC]), 'v': (32, [0, 4, 8, 0xFFFFFFFC]),
         'b': (8, [0x00, 0x04, 0xFC, 0x08]), 'w': (16, [0x0000, 0x0004, 0xFFFC, 0x0008]),
         'e': (8, [0x00, 0x04, 0xFC, 0x08]), 'f': (8, [0x00, 0x04, 0xFC, 0x08])}
for r, (w, vals) in IREGS.items():
    ld = REGS[r][2]
    for b in 'xyus':
        for sg in (True, False):
            for sc in (1, 2, 4):
                for v in vals:
                    if w == 32 and not sg: continue
                    off = ext(v, w, sg) << (sc.bit_length() - 1)
                    if w == 32 and v >= 0x80000000: off = (v - (1 << 32)) << (sc.bit_length() - 1)   # -4 sur 32 bits, puis echelle
                    if not (-4096 <= off <= 4092): continue
                    d = DEST[b]; suffix = '' if sg else '(u)'
                    if b == 's': out.append('\tlds\t#win+4096')
                    else: out.append(f'\tld{b}\t#win+4096')
                    out.append(f'\t{ld}\t#{v}')
                    out.append(f'\tlea{d}\t[{r}{suffix}*{sc},{b}]')
                    out.append(f'\tst{d}\tres+{4*n}')
                    if b == 's': out.append('\tlds\tsaved_s')
                    i = 4096 + off
                    exp.append(int.from_bytes(bytes(winbyte(i + k) for k in range(4)), 'big')); n += 1
# --- cas d'acces memoire directs : lda/ldq avec index (octet / mot 32 bits) depuis win+4096
for r in ('b', 'd', 'v', 'q'):
    w, vals, ld = REGS[r]
    for sc in (1, 2, 4, 8, 16):
        for v in ([0, 1, 3, 0x7F, 0xFF] if w == 8 else [0, 1, 3, 0x7F, 0x100] if w == 16 else [0, 1, 3, 0x7F, 0x100]):
            if not (0 <= 4096 + ext(v, w, True) * sc < WIN): continue
            out.append('\tldy\t#win+4096'); out.append(f'\t{ld}\t#{v}')
            out.append(f'\tlda\t{r}*{sc},y' if r not in ('b',) else f'\tlda\tb*{sc},y')
            out.append('\ttfr\ta,b\n\tclrw\n\tclra' if False else '')
            out.append(f'\tclrx\n\ttfr\tx,v\n' if False else '')
            # le resultat est lu depuis A : on le copie dans X par un chemin sans toucher aux registres testes
            out.append('\tsta\tres+%d' % (4 * n + 3))
            out.append(f'\tclr\tres+{4*n}\n\tclr\tres+{4*n+1}\n\tclr\tres+{4*n+2}')
            i = 4096 + ext(v, w, True) * sc
            exp.append(winbyte(i)); n += 1
out.append('\tlds\tsaved_s\n\tldu\tsaved_u\n\trts')
out.append(f'\t.bss\n\t.globl res\n\t.globl win\n\t.balign 4\nres:\t.skip {4*n}\nwin:\t.skip {WIN}\nsaved_s:\t.skip 4\nsaved_u:\t.skip 4')
out = [l for l in out if l.strip() != '']
open('idx_all.s', 'w').write('\n'.join(out) + '\n')
open('idx_expect.txt', 'w').write('\n'.join('%08X' % e for e in exp) + '\nEND\n')
open('idx_exp.h', 'w').write('static const unsigned long want[%d] = {\n%s\n};\n' % (n, ',\n'.join('0x%08XUL' % e for e in exp)))
open('idx_n.h', 'w').write('#define NRES %d\n#define WIN %d\n' % (n, WIN))
print('cas :', n)
