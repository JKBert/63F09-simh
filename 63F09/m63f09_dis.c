/* m63f09_dis.c: 63F09 disassembler for the simulator

   Copyright (c) 2026, Dr. BERTRAND Joël and Claude (Anthropic)

       Permission is hereby granted, free of charge, to any person obtaining a
       copy of this software and associated documentation files (the "Software"),
       to deal in the Software without restriction, including without limitation
       the rights to use, copy, modify, merge, publish, distribute, sublicense,
       and/or sell copies of the Software, and to permit persons to whom the
       Software is furnished to do so, subject to the following conditions:

       The above copyright notice and this permission notice shall be included in
       all copies or substantial portions of the Software.

       THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
       IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
       FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
       THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
       IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
       CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Port of opcodes/m6809-dis.c (binutils-gdb), which is verified against the assembler (as-new) by round trips: same
   syntax, same fields, same table (m63f09_optab.h is generated from opcodes/m6809-opc.c).  Differences: no BFD (the
   bytes are given in a buffer), and branch targets are printed as plain addresses.  Keep the two in step when the
   binutils disassembler changes.
*/

#include <string.h>
#include "m63f09_defs.h"

static const char *const tfr_reg_name[16] = {
    "D", "X", "Y", "U", "S", "PC", "W", "V", "A", "B", "CC", "DP", "Q", "DS", "E", "F"
};

static const char *const idx_base_reg[4] = { "X", "Y", "U", "S" };

typedef struct {
    const uint8 *p;
    int          avail;
    int          len;                                   /* bytes consumed */
    int          failed;
    int          wide;
    FILE        *of;
    t_addr       addr;
} rd_t;

static uint32 rd_byte (rd_t *st)
{
    if (st->failed || st->len >= st->avail) {
        st->failed = 1;
        return 0;
    }
    return st->p[st->len++];
}

static t_uint64 rd_be (rd_t *st, int n)
{
    t_uint64 v = 0;
    int i;

    for (i = 0; i < n; i++)
        v = (v << 8) | rd_byte (st);
    return v;
}

static t_int64 sign_ext (t_uint64 v, int bits)
{
    t_uint64 m = 1ULL << (bits - 1);

    return (t_int64) ((v ^ m) - m);
}

static void print_target (rd_t *st, t_int64 delta)
{
    t_addr target = st->addr + st->len + delta;

    if (st->wide)
        fprintf (st->of, "$%08llx", (unsigned long long) (target & 0xFFFFFFFFULL));
    else
        fprintf (st->of, "$%04llx", (unsigned long long) (target & 0xFFFFULL));
}

static void print_indexed (rd_t *st)
{
    uint32 pb = rd_byte (st);
    int indirect;
    const char *reg;
    FILE *of = st->of;

    if (!(pb & 0x80)) {
        fprintf (of, "%ld,%s", (long) sign_ext (pb & 0x1f, 5), idx_base_reg[(pb >> 5) & 3]);
        return;
    }
    indirect = (pb & 0x10) != 0;
    if ((pb & 0x0f) <= 0x07 || (pb & 0x0f) == 0x0a || (pb & 0x0f) == 0x0b || (pb & 0x0f) == 0x0e) {
        reg = idx_base_reg[(pb >> 5) & 3];
        switch (pb & 0x0f) {
            case 0x00: fprintf (of, ",%s+", reg); return;
            case 0x01: fprintf (of, indirect ? "[,%s++]" : ",%s++", reg); return;
            case 0x02: fprintf (of, ",-%s", reg); return;
            case 0x03: fprintf (of, indirect ? "[,--%s]" : ",--%s", reg); return;
            case 0x04: fprintf (of, indirect ? "[,%s]" : ",%s", reg); return;
            case 0x05: fprintf (of, indirect ? "[B,%s]" : "B,%s", reg); return;
            case 0x06: fprintf (of, indirect ? "[A,%s]" : "A,%s", reg); return;
            case 0x07: fprintf (of, indirect ? "[E,%s]" : "E,%s", reg); return;
            case 0x0a: fprintf (of, indirect ? "[F,%s]" : "F,%s", reg); return;
            case 0x0b: fprintf (of, indirect ? "[D,%s]" : "D,%s", reg); return;
            case 0x0e: fprintf (of, indirect ? "[W,%s]" : "W,%s", reg); return;
        }
    }
    switch (pb & 0x0f) {
        case 0x08: {
            unsigned long long n = rd_be (st, 1);
            fprintf (of, indirect ? "[$%llx,%s]" : "$%llx,%s", n, idx_base_reg[(pb >> 5) & 3]);
            return;
        }
        case 0x09: {
            unsigned long long n = rd_be (st, st->wide ? 4 : 2);
            fprintf (of, indirect ? "[$%llx,%s]" : "$%llx,%s", n, idx_base_reg[(pb >> 5) & 3]);
            return;
        }
        case 0x0c: {
            t_int64 n = sign_ext (rd_be (st, 1), 8);
            if (indirect) fprintf (of, "[");
            print_target (st, n);
            fprintf (of, indirect ? ",PCR]" : ",PCR");
            return;
        }
        case 0x0d: {
            int w = st->wide ? 4 : 2;
            t_int64 n = sign_ext (rd_be (st, w), w * 8);
            if (indirect) fprintf (of, "[");
            print_target (st, n);
            fprintf (of, indirect ? ",PCR]" : ",PCR");
            return;
        }
        case 0x0f: {
            unsigned long long n = rd_be (st, st->wide ? 4 : 2);
            fprintf (of, "[$%llx]", n);
            return;
        }
        default:
            fprintf (of, "<bad indexed postbyte $%02x>", pb);
            return;
    }
}

static void print_register_list (FILE *of, unsigned int mask, int is_u_stack)
{
    static const struct { const char *name; unsigned int bit; } bits[] = {
        { "CC", 0x0001 }, { "A", 0x0002 }, { "B", 0x0004 }, { "DP", 0x0008 }, { "X", 0x0010 }, { "Y", 0x0020 },
        { "PC", 0x0080 }, { "E", 0x0100 }, { "F", 0x0200 }, { "V", 0x0400 }, { "DS", 0x0800 }
    };
    int first = 1;
    size_t i;

    /* W is E and F together: indistinguishable from "push E,F" (known limitation of the encoding). */
    if ((mask & 0x0300) == 0x0300) {
        fprintf (of, "W");
        mask &= ~0x0300u;
        first = 0;
    }
    for (i = 0; i < sizeof (bits) / sizeof (bits[0]); i++)
        if (mask & bits[i].bit) {
            fprintf (of, "%s%s", first ? "" : ",", bits[i].name);
            first = 0;
        }
    if (mask & 0x0040)
        fprintf (of, "%s%s", first ? "" : ",", is_u_stack ? "S" : "U");
}

/* Shift and rotate of a register by n (opcodes 42 45 4B 4E 5E and 52 55 5B 87 8F): the register is in the operand byte. */
static int shn_decode (uint8 opcode, uint8 b, char *reg, unsigned int *count)
{
    if (opcode == 0x52 || opcode == 0x55 || opcode == 0x5b || opcode == 0x87 || opcode == 0x8f) {
        if ((b >> 5) > 5)
            return 0;
        *reg = "xyusvq"[b >> 5];
        *count = b & 0x1f;
        return 1;
    }
    if ((b >> 6) == 2) {
        *reg = 'o';
        *count = b & 0x3f;
    } else if ((b >> 5) == 0) {
        *reg = "abef"[(b >> 3) & 3];
        *count = b & 7;
    } else if ((b >> 5) == 2) {
        *reg = (b & 0x10) ? 'w' : 'd';
        *count = b & 0xf;
    } else
        return 0;
    return 1;
}

static const char *shn_name (uint8 opcode)
{
    switch (opcode) {
        case 0x42: case 0x52: return "lsr";
        case 0x45: case 0x55: return "asr";
        case 0x4b: case 0x5b: return "asl";
        case 0x5e: case 0x87: return "ror";
        default:              return "rol";
    }
}

const m63f09_op_t *m63f09_find_op (uint8 page, uint8 opcode, uint16 arch)
{
    int i;

    if (page == 0x11 && opcode == 0x3d)                 /* LDMD: decoded whatever MD says (63F09.vhd, 113D) */
        arch = M63_OPCODE_ARCH_ALL;
    for (i = 0; i < M63F09_NOPS; i++)
        if (m63f09_optab[i].page == page && m63f09_optab[i].opcode == opcode && (m63f09_optab[i].arch & arch))
            return &m63f09_optab[i];
    return NULL;
}

int m63f09_disasm (FILE *of, t_addr addr, const uint8 *mem, int avail, uint16 arch, int wide)
{
    rd_t st;
    uint8 b0, opcode = 0, page = 0;
    const m63f09_op_t *match = NULL;

    memset (&st, 0, sizeof (st));
    st.p = mem;
    st.avail = avail;
    st.wide = wide;
    st.of = of;
    st.addr = addr;

    b0 = rd_byte (&st);
    if (st.failed) {
        fprintf (of, "?");
        return 1;
    }
    if (b0 == 0x10 || b0 == 0x11 || b0 == 0x18 || b0 == 0x1b) {
        page = b0;
        opcode = rd_byte (&st);
        if (st.failed) {
            fprintf (of, ".byte $%02x", b0);
            return 1;
        }
    } else
        opcode = b0;

    match = m63f09_find_op (page, opcode, arch);
    if (!match) {
        fprintf (of, ".byte $%02x", b0);
        return 1;
    }

    if (match->mode == M63_ADDR_SHN) {
        uint8 b = rd_byte (&st);
        char reg;
        unsigned int count;

        if (st.failed) {
            fprintf (of, ".byte $%02x", b0);
            return 1;
        }
        if (!shn_decode (opcode, b, &reg, &count)) {
            fprintf (of, ".byte $%02x", b0);
            return 1;
        }
        fprintf (of, "%sn%c\t#%u", shn_name (opcode), reg, count);
        return st.len;
    }

    if (!strcmp (match->name, "tfm_ii") || !strcmp (match->name, "tfm_dd")
            || !strcmp (match->name, "tfm_id") || !strcmp (match->name, "tfm_di")) {
        uint8 mask = rd_byte (&st);
        const char *s1 = "", *s2 = "";

        if (!strcmp (match->name, "tfm_ii")) { s1 = "+"; s2 = "+"; }
        else if (!strcmp (match->name, "tfm_dd")) { s1 = "-"; s2 = "-"; }
        else if (!strcmp (match->name, "tfm_id")) { s1 = "+"; }
        else { s2 = "+"; }
        fprintf (of, "tfm\t%s%s,%s%s", tfr_reg_name[(mask >> 4) & 0xf], s1, tfr_reg_name[mask & 0xf], s2);
        return st.failed ? 1 : st.len;
    }

    fprintf (of, "%s", match->name);

    /* OIM/AIM/EIM/TIM and the shifts of a byte in memory: "#mask,address" */
    if (!strcmp (match->name, "oim") || !strcmp (match->name, "aim") || !strcmp (match->name, "eim")
            || !strcmp (match->name, "tim")
            || (match->page == 0x1b && strlen (match->name) == 4 && match->name[3] == 'n')) {
        unsigned long long mask = rd_be (&st, 1);

        fprintf (of, "\t#$%02llx,", mask);
        switch (match->mode) {
            case M63_ADDR_DIR:   fprintf (of, "<$%02llx", rd_be (&st, 1)); break;
            case M63_ADDR_EXT16: fprintf (of, "$%04llx", rd_be (&st, 2)); break;
            case M63_ADDR_EXT32: fprintf (of, "$%08llx", rd_be (&st, 4)); break;
            case M63_ADDR_IDX:
            case M63_ADDR_IDXIND: print_indexed (&st); break;
            default: break;
        }
        return st.failed ? 1 : st.len;
    }

    switch (match->mode) {
        case M63_ADDR_INH:
            break;
        case M63_ADDR_IMM8:   fprintf (of, "\t#$%02llx", rd_be (&st, 1)); break;
        case M63_ADDR_IMM16:  fprintf (of, "\t#$%04llx", rd_be (&st, 2)); break;
        case M63_ADDR_IMM32:  fprintf (of, "\t#$%08llx", rd_be (&st, 4)); break;
        case M63_ADDR_IMM64: {
            unsigned long long hi = rd_be (&st, 4);
            unsigned long long lo = rd_be (&st, 4);
            fprintf (of, "\t#$%08llx%08llx", hi, lo);
            break;
        }
        case M63_ADDR_DIR:    fprintf (of, "\t<$%02llx", rd_be (&st, 1)); break;
        case M63_ADDR_EXT16:  fprintf (of, "\t$%04llx", rd_be (&st, 2)); break;
        case M63_ADDR_EXT32:  fprintf (of, "\t$%08llx", rd_be (&st, 4)); break;
        case M63_ADDR_REL8:   { t_int64 n = sign_ext (rd_be (&st, 1), 8);  fprintf (of, "\t"); print_target (&st, n); break; }
        case M63_ADDR_REL16:  { t_int64 n = sign_ext (rd_be (&st, 2), 16); fprintf (of, "\t"); print_target (&st, n); break; }
        case M63_ADDR_REL32:  { t_int64 n = sign_ext (rd_be (&st, 4), 32); fprintf (of, "\t"); print_target (&st, n); break; }
        case M63_ADDR_IDX:
        case M63_ADDR_IDXIND:
            fprintf (of, "\t");
            print_indexed (&st);
            break;
        case M63_ADDR_REGLIST: {
            int is_u = (match->name[3] == 'u' || match->name[3] == 'U');
            unsigned int mask = (unsigned int) rd_be (&st, wide ? 2 : 1);

            fprintf (of, "\t");
            print_register_list (of, mask, is_u);
            break;
        }
        case M63_ADDR_REG: {
            static const char *const bit_xfer[] = { "band", "biand", "bor", "bior", "beor", "bieor", "ldbt", "stbt", NULL };
            int is_bit_xfer = 0, j;

            for (j = 0; bit_xfer[j]; j++)
                if (!strcmp (match->name, bit_xfer[j]))
                    is_bit_xfer = 1;
            if (is_bit_xfer) {
                static const char *const bitreg[4] = { "CC", "A", "B", "?" };
                uint8 pb = rd_byte (&st);
                uint8 ab = rd_byte (&st);

                /* "r,sBit,dBit,addr" (the order of the 6309 manual), as in the binutils disassembler */
                fprintf (of, "\t%s,%d,%d,$%02x", bitreg[(pb >> 6) & 3], (pb >> 3) & 7, pb & 7, ab);
            } else {
                uint8 mask = rd_byte (&st);

                fprintf (of, "\t%s,%s", tfr_reg_name[(mask >> 4) & 0xf], tfr_reg_name[mask & 0xf]);
            }
            break;
        }
        case M63_ADDR_FPREG1:  fprintf (of, "\t#%u", (unsigned) rd_byte (&st) + 1); break;
        case M63_ADDR_FPCOUNT: fprintf (of, "\t#%u", (unsigned) rd_byte (&st)); break;
        case M63_ADDR_FPREG2: {
            uint8 v = rd_byte (&st);

            fprintf (of, "\t#%u,#%u", ((v >> 4) & 0xf) + 1u, (v & 0xf) + 1u);
            break;
        }
        default:
            fprintf (of, "\t; <unhandled mode $%x>", match->mode);
            break;
    }
    if (st.failed) {
        /* not enough bytes for the operand: show the first byte only, the rest is shown as the following units */
        return 1;
    }
    return st.len;
}
