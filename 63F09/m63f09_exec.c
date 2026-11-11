/* m63f09_exec.c: 63F09 instruction execution

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

   Executes ONE instruction of a core.  The table (m63f09_optab.h) gives the rows; each row is classified once from its
   mnemonic (classify()), so the code below is organised by family (ALU, read-modify-write, loads and stores, branches...)
   and by register width, not by opcode.  Rules taken from 63F09.vhd (the source of truth) are quoted where they are not the
   usual 6809 ones: the auto-increment of ,R++ and ,--R follows the width of the data in the 32-bit state (1, 2, 4 or 8:
   LDO ,X++), the pointer of an indirect mode is 4 bytes in the 32-bit state, 2 otherwise, LEA uses the pointer width.
*/

#include <string.h>
#include "m63f09_defs.h"

#ifndef M63_RTL_TFM_LOADS_CC
#define M63_RTL_TFM_LOADS_CC 1
#endif

/* 1 : LDSFP sign-extends the 32 bits into the 64-bit register (63F09.vhd of 2026-11-11 and the soc63f09 generated from it).  Proposed fix, 4 sites of
   the page 18 decode of 63F09.vhd (IMMEDIATE8_UNSIGNED and the *32_UNSIGNED_MODE_READ_STATE that already exist) : set to 0 once soc63f09 is regenerated. */
#ifndef M63_RTL_LDSFP_SIGNEXT
#define M63_RTL_LDSFP_SIGNEXT 0
#endif

#ifndef M63_RTL_ADCR8_NO_CARRY
#define M63_RTL_ADCR8_NO_CARRY 0
#endif

int m63f09_why;                                         /* line of the last refusal (debugging aid) */
#define UNIMPL()        (m63f09_why = __LINE__, STOP_UNIMPL)

/* A TRAP is not a stop of the simulator: step_inner returns STOP_TRAP_INT and the cause, m63f09_step then takes the exception.
   The PC pushed is the one of the decoder when the cause is found (to be checked against the RTL). */
#define STOP_TRAP_INT   100
static uint32 trap_pc;
static int trap_cause;                                  /* 1 illegal instruction, 2 division by 0 */
#define TRAP_ILL()      (trap_pc = x->pc, trap_cause = 1, STOP_TRAP_INT)
#define TRAP_DIV()      (trap_pc = x->pc, trap_cause = 2, STOP_TRAP_INT)

/* Register identifiers: the TFR/EXG nibbles, then O (not encodable) */
enum { R_D, R_X, R_Y, R_U, R_S, R_PC, R_W, R_V, R_A, R_B, R_CC, R_DP, R_Q, R_DS, R_E, R_F, R_O, R_NONE };

/* Families */
enum { K_NONE, K_ALU, K_UN, K_LEA, K_BRA, K_JMP, K_JSR, K_REGREG, K_TFR, K_EXG, K_MISC, K_SHN, K_SHNMEM };

/* Operations of K_ALU and K_UN */
enum { O_ADD, O_ADC, O_SUB, O_SBC, O_AND, O_OR, O_EOR, O_CMP, O_BIT, O_LD, O_ST,
       O_NEG, O_COM, O_LSR, O_ROR, O_ASR, O_ASL, O_ROL, O_DEC, O_INC, O_TST, O_CLR };

typedef struct {
    uint8 kind, op, reg;
    uint8 cond;                                         /* K_BRA: 0 always .. 15 */
    uint8 lng;                                          /* K_BRA: long form */
} xd_t;

static xd_t xdesc[M63F09_NOPS];
static const m63f09_op_t *dtab[4][5][256];
static int dtab_ready;

/* ---------------------------------------------------------------------------------------------------------------- */
/* Classification of a row from its mnemonic                                                                         */
/* ---------------------------------------------------------------------------------------------------------------- */

static int reg_of_letter (char ch)
{
    switch (ch) {
        case 'a': return R_A;  case 'b': return R_B;  case 'd': return R_D;  case 'e': return R_E;
        case 'f': return R_F;  case 'w': return R_W;  case 'x': return R_X;  case 'y': return R_Y;
        case 'u': return R_U;  case 's': return R_S;  case 'v': return R_V;  case 'q': return R_Q;
        case 'o': return R_O;
        default:  return R_NONE;
    }
}

static const struct { const char *name; int op; } alu_ops[] = {
    { "add", O_ADD }, { "adc", O_ADC }, { "sub", O_SUB }, { "sbc", O_SBC }, { "and", O_AND }, { "or", O_OR },
    { "eor", O_EOR }, { "cmp", O_CMP }, { "bit", O_BIT }, { "ld", O_LD }, { "st", O_ST }
};
static const struct { const char *name; int op; } un_ops[] = {
    { "neg", O_NEG }, { "com", O_COM }, { "lsr", O_LSR }, { "ror", O_ROR }, { "asr", O_ASR }, { "asl", O_ASL },
    { "lsl", O_ASL }, { "rol", O_ROL }, { "dec", O_DEC }, { "inc", O_INC }, { "tst", O_TST }, { "clr", O_CLR }
};
static const char *const conds[] = { "ra", "rn", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le" };

static void classify (const m63f09_op_t *row, xd_t *d)
{
    const char *n = row->name;
    size_t len = strlen (n);
    size_t i;

    memset (d, 0, sizeof (*d));
    d->reg = R_NONE;
    if (row->mode == M63_ADDR_SHN) {                    /* lsrna... : the register is in the operand byte */
        d->kind = K_SHN;
        return;
    }
    if (row->page == 0x1b && len == 4 && n[3] == 'n') {  /* lsrn... : a byte in memory */
        d->kind = K_SHNMEM;
        d->op = !strncmp (n, "lsr", 3) ? O_LSR : !strncmp (n, "asr", 3) ? O_ASR
              : (!strncmp (n, "asl", 3) || !strncmp (n, "lsl", 3)) ? O_ASL : !strncmp (n, "ror", 3) ? O_ROR : O_ROL;
        return;
    }
    for (i = 0; i < sizeof (alu_ops) / sizeof (alu_ops[0]); i++) {
        size_t l = strlen (alu_ops[i].name);

        if (len == l + 1 && !strncmp (n, alu_ops[i].name, l) && reg_of_letter (n[l]) != R_NONE
                && !(alu_ops[i].op == O_ST && reg_of_letter (n[l]) == R_NONE)) {
            d->kind = K_ALU;
            d->op = alu_ops[i].op;
            d->reg = reg_of_letter (n[l]);
            return;
        }
    }
    for (i = 0; i < sizeof (un_ops) / sizeof (un_ops[0]); i++) {
        size_t l = strlen (un_ops[i].name);

        if (!strncmp (n, un_ops[i].name, l) && (len == l || (len == l + 1 && reg_of_letter (n[l]) != R_NONE))) {
            d->kind = K_UN;
            d->op = un_ops[i].op;
            d->reg = len == l ? R_NONE : reg_of_letter (n[l]);
            return;
        }
    }
    if (len == 4 && !strncmp (n, "lea", 3) && reg_of_letter (n[3]) >= R_X && reg_of_letter (n[3]) <= R_S) {
        d->kind = K_LEA;
        d->reg = reg_of_letter (n[3]);
        return;
    }
    if (!strcmp (n, "jmp")) { d->kind = K_JMP; return; }
    if (!strcmp (n, "jsr")) { d->kind = K_JSR; return; }
    if (!strcmp (n, "bsr") || !strcmp (n, "lbsr")) { d->kind = K_BRA; d->cond = 16; d->lng = (n[0] == 'l'); return; }
    if (!strcmp (n, "bhs")) { d->kind = K_BRA; d->cond = 4; return; }
    if (!strcmp (n, "blo")) { d->kind = K_BRA; d->cond = 5; return; }
    if (!strcmp (n, "lbhs")) { d->kind = K_BRA; d->cond = 4; d->lng = 1; return; }
    if (!strcmp (n, "lblo")) { d->kind = K_BRA; d->cond = 5; d->lng = 1; return; }
    if (n[0] == 'b' || (n[0] == 'l' && n[1] == 'b')) {
        const char *c = (n[0] == 'l') ? n + 2 : n + 1;

        for (i = 0; i < 16; i++)
            if (!strcmp (c, conds[i])) {
                d->kind = K_BRA;
                d->cond = (uint8) i;
                d->lng = (n[0] == 'l');
                return;
            }
    }
    if (!strcmp (n, "tfr")) { d->kind = K_TFR; return; }
    if (!strcmp (n, "exg")) { d->kind = K_EXG; return; }
    if (!strcmp (n, "addr") || !strcmp (n, "adcr") || !strcmp (n, "subr") || !strcmp (n, "sbcr") || !strcmp (n, "andr")
            || !strcmp (n, "orr") || !strcmp (n, "eorr") || !strcmp (n, "cmpr")) {
        d->kind = K_REGREG;
        d->op = !strcmp (n, "addr") ? O_ADD : !strcmp (n, "adcr") ? O_ADC : !strcmp (n, "subr") ? O_SUB
              : !strcmp (n, "sbcr") ? O_SBC : !strcmp (n, "andr") ? O_AND : !strcmp (n, "orr") ? O_OR
              : !strcmp (n, "eorr") ? O_EOR : O_CMP;
        return;
    }
    d->kind = K_MISC;
}

static int arch_index (uint16 arch)
{
    if (arch == M63_OPCODE_ARCH_6809) return 0;
    if (arch == M63_OPCODE_ARCH_6309) return 1;
    if (arch == M63_OPCODE_ARCH_63F09_32BITS) return 2;
    return 3;
}

static int page_index (uint8 page)
{
    return page == 0x00 ? 0 : page == 0x10 ? 1 : page == 0x11 ? 2 : page == 0x18 ? 3 : 4;
}

static void build_tables (void)
{
    static const uint16 archs[4] = { M63_OPCODE_ARCH_6809, M63_OPCODE_ARCH_6309,
                                     M63_OPCODE_ARCH_63F09_32BITS, M63_OPCODE_ARCH_63F09_32BITS_FPU };
    int a, i;

    for (i = 0; i < M63F09_NOPS; i++)
        classify (&m63f09_optab[i], &xdesc[i]);
    for (a = 0; a < 4; a++)
        for (i = 0; i < M63F09_NOPS; i++) {
            const m63f09_op_t *r = &m63f09_optab[i];
            const m63f09_op_t **slot = &dtab[a][page_index (r->page)][r->opcode];

            if (r->arch & archs[a])
                if (!*slot)
                    *slot = r;
        }
    /* LDMD is decoded in every state (63F09.vhd, 113D) */
    for (i = 0; i < M63F09_NOPS; i++)
        if (m63f09_optab[i].page == 0x11 && m63f09_optab[i].opcode == 0x3d)
            for (a = 0; a < 4; a++)
                if (!dtab[a][2][0x3d])
                    dtab[a][2][0x3d] = &m63f09_optab[i];
    dtab_ready = 1;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Execution context                                                                                                 */
/* ---------------------------------------------------------------------------------------------------------------- */

typedef struct {
    m63f09_core_t *c;
    int wide;                                           /* 32-bit state */
    uint32 amask;                                       /* address mask: 0xFFFF or 0xFFFFFFFF */
    int ptrw;                                           /* pointer width in bytes: 2 or 4 */
    uint32 pc;                                          /* running PC (fetch) */
    uint32 upper;                                       /* bits of PC above the address width: kept (LDMD changes the width) */
    int stop;                                           /* stop reason found during the instruction */
    uint32 badaddr;
} ctx_t;

static t_addr la_to_pa (const ctx_t *x, uint32 la)
{
    return PA_PREFIX | (x->wide ? la : (0xFFFF0000u | (la & 0xFFFF)));
}

static uint8 rd8 (ctx_t *x, uint32 la)
{
    uint8 v = 0xFF;

    if (m63f09_read8 (la_to_pa (x, la), &v)) {
        x->stop = STOP_NXM;
        x->badaddr = la;
    }
    return v;
}

static void wr8 (ctx_t *x, uint32 la, uint8 v)
{
    if (m63f09_write8 (la_to_pa (x, la), v) && !x->stop) {
        /* a write to the ROM is ignored by the hardware; only an unmapped address is an error */
        uint8 t;

        if (m63f09_read8 (la_to_pa (x, la), &t)) {
            x->stop = STOP_NXM;
            x->badaddr = la;
        }
    }
}

static t_uint64 rdn (ctx_t *x, uint32 la, int n)
{
    t_uint64 v = 0;
    int i;

    for (i = 0; i < n; i++)
        v = (v << 8) | rd8 (x, la + i);
    return v;
}

static void wrn (ctx_t *x, uint32 la, int n, t_uint64 v)
{
    int i;

    for (i = n - 1; i >= 0; i--) {
        wr8 (x, la + i, (uint8) v);
        v >>= 8;
    }
}

static uint8 f8 (ctx_t *x)
{
    uint8 v = rd8 (x, x->pc);

    x->pc = (x->pc + 1) & x->amask;
    return v;
}

static t_uint64 fn (ctx_t *x, int n)
{
    t_uint64 v = 0;
    int i;

    for (i = 0; i < n; i++)
        v = (v << 8) | f8 (x);
    return v;
}

static t_int64 sext (t_uint64 v, int bits)
{
    t_uint64 m = 1ULL << (bits - 1);

    return (t_int64) ((v ^ m) - m);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Registers                                                                                                         */
/* ---------------------------------------------------------------------------------------------------------------- */

static int reg_width (const ctx_t *x, int r)             /* bytes */
{
    switch (r) {
        case R_A: case R_B: case R_E: case R_F: case R_CC: case R_DP: return 1;
        case R_D: case R_W: case R_DS: return 2;
        case R_Q: return 4;
        case R_O: return 8;
        default: return x->ptrw;                         /* X Y U S PC V */
    }
}

static t_uint64 getreg (ctx_t *x, int r)
{
    m63f09_core_t *c = x->c;

    switch (r) {
        case R_A: return c->a;
        case R_B: return c->b;
        case R_E: return c->e;
        case R_F: return c->f;
        case R_CC: return c->cc;
        case R_DP: return c->dp;
        case R_DS: return c->ds;
        case R_D: return ((uint32) c->a << 8) | c->b;
        case R_W: return ((uint32) c->e << 8) | c->f;
        case R_Q: return ((t_uint64) c->a << 24) | ((uint32) c->b << 16) | ((uint32) c->e << 8) | c->f;
        case R_O: return (getreg (x, R_Q) << 32) | c->v;
        case R_X: return c->x & x->amask;
        case R_Y: return c->y & x->amask;
        case R_U: return c->u & x->amask;
        case R_S: return c->s & x->amask;
        case R_PC: return x->pc & x->amask;
        case R_V: return c->v & x->amask;
    }
    return 0;
}

static void setreg (ctx_t *x, int r, t_uint64 v)
{
    m63f09_core_t *c = x->c;

    switch (r) {
        case R_A: c->a = (uint8) v; break;
        case R_B: c->b = (uint8) v; break;
        case R_E: c->e = (uint8) v; break;
        case R_F: c->f = (uint8) v; break;
        case R_CC: c->cc = (uint8) v; break;
        case R_DP: c->dp = (uint8) v; break;
        case R_DS: c->ds = (uint16) v; break;
        case R_D: c->a = (uint8) (v >> 8); c->b = (uint8) v; break;
        case R_W: c->e = (uint8) (v >> 8); c->f = (uint8) v; break;
        case R_Q: c->a = (uint8) (v >> 24); c->b = (uint8) (v >> 16); c->e = (uint8) (v >> 8); c->f = (uint8) v; break;
        case R_O: setreg (x, R_Q, v >> 32); c->v = (uint32) v; break;
        case R_X: c->x = (uint32) v & x->amask; break;
        case R_Y: c->y = (uint32) v & x->amask; break;
        case R_U: c->u = (uint32) v & x->amask; break;
        case R_S: c->s = (uint32) v & x->amask; c->s_loaded = 1; break;
        case R_PC: x->pc = (uint32) v & x->amask; break;
        case R_V: c->v = (uint32) v & x->amask; break;
    }
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* ALU                                                                                                               */
/* ---------------------------------------------------------------------------------------------------------------- */

static t_uint64 wmask (int bytes)
{
    return bytes >= 8 ? ~0ULL : ((1ULL << (bytes * 8)) - 1);
}

static t_uint64 wsign (int bytes)
{
    return 1ULL << (bytes * 8 - 1);
}

static void set_nz (ctx_t *x, t_uint64 r, int bytes)
{
    uint8 cc = x->c->cc & ~(CC_N | CC_Z);

    if (r & wsign (bytes)) cc |= CC_N;
    if (!(r & wmask (bytes))) cc |= CC_Z;
    x->c->cc = cc;
}

static void set_flag (ctx_t *x, uint8 bit, int on)
{
    if (on) x->c->cc |= bit; else x->c->cc &= (uint8) ~bit;
}

/* r = a + b + cin, flags H N Z V C (H for bytes only, as the 6809) */
static t_uint64 alu_add (ctx_t *x, t_uint64 a, t_uint64 b, int cin, int bytes)
{
    t_uint64 m = wmask (bytes), s = wsign (bytes);
    t_uint64 r = (a + b + (t_uint64) cin) & m;
    int carry = bytes >= 8 ? ((a & m) + (b & m) < (a & m)) || (((a & m) + (b & m) + cin) < ((a & m) + (b & m)))
                           : ((((a & m) + (b & m) + (t_uint64) cin) >> (bytes * 8)) != 0);

    set_nz (x, r, bytes);
    set_flag (x, CC_C, carry);
    set_flag (x, CC_V, ((a ^ r) & (b ^ r) & s) != 0);
    if (bytes == 1)
        set_flag (x, CC_H, (((a & 0xF) + (b & 0xF) + (t_uint64) cin) & 0x10) != 0);
    return r;
}

/* r = a - b - cin, flags N Z V C (C = borrow) */
static t_uint64 alu_sub (ctx_t *x, t_uint64 a, t_uint64 b, int cin, int bytes)
{
    t_uint64 m = wmask (bytes), s = wsign (bytes);
    t_uint64 r = (a - b - (t_uint64) cin) & m;
    int borrow = ((a & m) < (b & m)) || (((a & m) == (b & m)) && cin);

    set_nz (x, r, bytes);
    set_flag (x, CC_C, borrow);
    set_flag (x, CC_V, ((a ^ b) & (a ^ r) & s) != 0);
    return r;
}

static t_uint64 alu_logic (ctx_t *x, t_uint64 r, int bytes)
{
    r &= wmask (bytes);
    set_nz (x, r, bytes);
    set_flag (x, CC_V, 0);
    return r;
}

/* read-modify-write operations on a value of the given width */
static t_uint64 alu_un (ctx_t *x, int op, t_uint64 v, int bytes)
{
    t_uint64 m = wmask (bytes), s = wsign (bytes), r;
    int cin = (x->c->cc & CC_C) != 0, old;

    v &= m;
    switch (op) {
        case O_NEG:
            r = alu_sub (x, 0, v, 0, bytes);
            return r;
        case O_COM:
            r = (~v) & m;
            set_nz (x, r, bytes);
            set_flag (x, CC_V, 0);
            set_flag (x, CC_C, 1);
            return r;
        case O_LSR:
            set_flag (x, CC_C, (v & 1) != 0);
            r = v >> 1;
            set_nz (x, r, bytes);
            return r;
        case O_ASR:
            set_flag (x, CC_C, (v & 1) != 0);
            r = (v >> 1) | (v & s);
            set_nz (x, r, bytes);
            return r;
        case O_ASL:
            old = (v & s) != 0;
            r = (v << 1) & m;
            set_nz (x, r, bytes);
            set_flag (x, CC_C, old);
            set_flag (x, CC_V, old != ((r & s) != 0));
            return r;
        case O_ROR:
            set_flag (x, CC_C, (v & 1) != 0);
            r = (v >> 1) | (cin ? s : 0);
            set_nz (x, r, bytes);
            return r;
        case O_ROL:
            old = (v & s) != 0;
            r = ((v << 1) | (t_uint64) cin) & m;
            set_nz (x, r, bytes);
            set_flag (x, CC_C, old);
            set_flag (x, CC_V, old != ((r & s) != 0));
            return r;
        case O_DEC:
            r = (v - 1) & m;
            set_nz (x, r, bytes);
            set_flag (x, CC_V, v == s);
            return r;
        case O_INC:
            r = (v + 1) & m;
            set_nz (x, r, bytes);
            set_flag (x, CC_V, v == s - 1);
            return r;
        case O_TST:
            set_nz (x, v, bytes);
            set_flag (x, CC_V, 0);
            return v;
        case O_CLR:
            set_nz (x, 0, bytes);
            set_flag (x, CC_V, 0);
            set_flag (x, CC_C, 0);
            return 0;
    }
    return v;
}

/* result of an ALU operation `op` of reg and operand; returns 1 when the result is written back */
static int alu_binop (ctx_t *x, int op, t_uint64 a, t_uint64 b, int bytes, t_uint64 *res)
{
    int cin = (x->c->cc & CC_C) != 0;

    switch (op) {
        case O_ADD: *res = alu_add (x, a, b, 0, bytes); return 1;
        case O_ADC: *res = alu_add (x, a, b, cin, bytes); return 1;
        case O_SUB: *res = alu_sub (x, a, b, 0, bytes); return 1;
        case O_SBC: *res = alu_sub (x, a, b, cin, bytes); return 1;
        case O_CMP: (void) alu_sub (x, a, b, 0, bytes); return 0;
        case O_AND: *res = alu_logic (x, a & b, bytes); return 1;
        case O_OR:  *res = alu_logic (x, a | b, bytes); return 1;
        case O_EOR: *res = alu_logic (x, a ^ b, bytes); return 1;
        case O_BIT: (void) alu_logic (x, a & b, bytes); return 0;
        case O_LD:  *res = alu_logic (x, b, bytes); return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Effective address                                                                                                 */
/* ---------------------------------------------------------------------------------------------------------------- */

static uint32 *index_reg (ctx_t *x, int rr)
{
    switch (rr) {
        case 0: return &x->c->x;
        case 1: return &x->c->y;
        case 2: return &x->c->u;
        default: return &x->c->s;
    }
}

/* dw: width in bytes of the datum (1, 2, 4, 8): the step of ,R++ and ,--R in the 32-bit state */
static uint32 indexed_ea (ctx_t *x, int dw)
{
    uint8 pb = f8 (x);
    uint32 *r = index_reg (x, (pb >> 5) & 3);
    uint32 ea = 0, base = *r & x->amask;
    int ind = 0;

    if (!(pb & 0x80)) {
        return (base + (uint32) sext (pb & 0x1f, 5)) & x->amask;
    }
    ind = (pb & 0x10) != 0;
    switch (pb & 0x0f) {
        case 0x00: ea = base; *r = (base + 1) & x->amask; break;
        case 0x01:
            ea = base;
            *r = (base + (ind ? x->ptrw : (x->wide ? dw : 2))) & x->amask;
            break;
        case 0x02: *r = (base - 1) & x->amask; ea = *r; break;
        case 0x03:
            *r = (base - (x->wide ? (ind ? 4 : dw) : 2)) & x->amask;
            ea = *r;
            break;
        case 0x04: ea = base; break;
        case 0x05: ea = base + (uint32) sext (x->c->b, 8); break;
        case 0x06: ea = base + (uint32) sext (x->c->a, 8); break;
        case 0x07: ea = base + (uint32) sext (x->c->e, 8); break;
        case 0x0a: ea = base + (uint32) sext (x->c->f, 8); break;
        case 0x0b: ea = base + (uint32) sext (getreg (x, R_D), 16); break;
        case 0x0e: ea = base + (uint32) sext (getreg (x, R_W), 16); break;
        case 0x08: ea = base + (uint32) sext (f8 (x), 8); break;
        case 0x09: ea = base + (uint32) sext (fn (x, x->ptrw), x->ptrw * 8); break;
        case 0x0c: { t_int64 o = sext (f8 (x), 8); ea = x->pc + (uint32) o; break; }
        case 0x0d: { t_int64 o = sext (fn (x, x->ptrw), x->ptrw * 8); ea = x->pc + (uint32) o; break; }
        case 0x0f: ea = (uint32) fn (x, x->ptrw); if (!ind) { x->stop = TRAP_ILL (); } break;
        default: { x->stop = TRAP_ILL (); } break;
    }
    ea &= x->amask;
    if (ind)
        ea = (uint32) rdn (x, ea, x->ptrw) & x->amask;
    return ea;
}

static uint32 direct_ea (ctx_t *x)
{
    uint32 off = f8 (x), dp = x->c->dp;

    return x->wide ? (((uint32) x->c->ds << 16) | (dp << 8) | off) : ((dp << 8) | off);
}

/* ea of a memory mode of a row */
static uint32 get_ea (ctx_t *x, const m63f09_op_t *row, int dw)
{
    switch (row->mode) {
        case M63_ADDR_DIR:   return direct_ea (x);
        case M63_ADDR_EXT16: return (uint32) fn (x, 2) & x->amask;
        case M63_ADDR_EXT32: return (uint32) fn (x, 4);
        case M63_ADDR_IDX:
        case M63_ADDR_IDXIND: return indexed_ea (x, dw);
    }
    { x->stop = TRAP_ILL (); }
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Branch conditions                                                                                                 */
/* ---------------------------------------------------------------------------------------------------------------- */

static int cond_true (uint8 cc, int cond)
{
    int c = (cc & CC_C) != 0, z = (cc & CC_Z) != 0, n = (cc & CC_N) != 0, v = (cc & CC_V) != 0;

    switch (cond) {
        case 0: return 1;
        case 1: return 0;
        case 2: return !(c || z);
        case 3: return c || z;
        case 4: return !c;
        case 5: return c;
        case 6: return !z;
        case 7: return z;
        case 8: return !v;
        case 9: return v;
        case 10: return !n;
        case 11: return n;
        case 12: return n == v;
        case 13: return n != v;
        case 14: return !z && (n == v);
        case 15: return z || (n != v);
    }
    return 0;
}

static void push_ptr (ctx_t *x, uint32 v)
{
    x->c->s = (x->c->s - x->ptrw) & x->amask;
    wrn (x, x->c->s, x->ptrw, v);
}


/* ---------------------------------------------------------------------------------------------------------------- */
/* Stack, block transfer, bit operations                                                                              */
/* ---------------------------------------------------------------------------------------------------------------- */

static void push1 (ctx_t *x, uint32 *sp, uint8 v)
{
    if (sp == &x->c->s)
        x->c->s_loaded = 1;
    *sp = (*sp - 1) & x->amask;
    wr8 (x, *sp, v);
}

static void pushn (ctx_t *x, uint32 *sp, t_uint64 v, int n)
{
    int i;

    for (i = 0; i < n; i++)                             /* low byte first: the register ends up big-endian at the new S */
        push1 (x, sp, (uint8) (v >> (8 * i)));
}

static t_uint64 pulln (ctx_t *x, uint32 *sp, int n)
{
    if (sp == &x->c->s)
        x->c->s_loaded = 1;
    t_uint64 v = rdn (x, *sp, n);

    *sp = (*sp + n) & x->amask;
    return v;
}

/* Mask bits: 11 DS, 10 V, 9 F, 8 E, 7 PC, 6 U/S, 5 Y, 4 X, 3 DP, 2 B, 1 A, 0 CC.  Pushed from bit 11 down to bit 0 (63F09.vhd,
   PUSH_SYSTEM_STATE), pulled from bit 0 up.  The stack pointer is S (PSHS/PULS) or U (PSHU/PULU), bit 6 names the other one. */
static void do_push (ctx_t *x, int use_u, uint32 mask)
{
    m63f09_core_t *c = x->c;
    uint32 *sp = use_u ? &c->u : &c->s;
    uint32 *other = use_u ? &c->s : &c->u;
    int pw = x->ptrw;

    if (mask & 0x800) pushn (x, sp, c->ds, 2);
    if (mask & 0x400) pushn (x, sp, c->v, pw);
    if (mask & 0x200) push1 (x, sp, c->f);
    if (mask & 0x100) push1 (x, sp, c->e);
    if (mask & 0x080) pushn (x, sp, x->pc, pw);
    if (mask & 0x040) pushn (x, sp, *other, pw);
    if (mask & 0x020) pushn (x, sp, c->y, pw);
    if (mask & 0x010) pushn (x, sp, c->x, pw);
    if (mask & 0x008) push1 (x, sp, c->dp);
    if (mask & 0x004) push1 (x, sp, c->b);
    if (mask & 0x002) push1 (x, sp, c->a);
    if (mask & 0x001) push1 (x, sp, c->cc);
}

static void do_pull (ctx_t *x, int use_u, uint32 mask)
{
    m63f09_core_t *c = x->c;
    uint32 *sp = use_u ? &c->u : &c->s;
    uint32 *other = use_u ? &c->s : &c->u;
    int pw = x->ptrw;

    if (mask & 0x001) c->cc = (uint8) pulln (x, sp, 1);
    if (mask & 0x002) c->a = (uint8) pulln (x, sp, 1);
    if (mask & 0x004) c->b = (uint8) pulln (x, sp, 1);
    if (mask & 0x008) c->dp = (uint8) pulln (x, sp, 1);
    if (mask & 0x010) c->x = (uint32) pulln (x, sp, pw);
    if (mask & 0x020) c->y = (uint32) pulln (x, sp, pw);
    if (mask & 0x040) *other = (uint32) pulln (x, sp, pw);
    if (mask & 0x080) x->pc = (uint32) pulln (x, sp, pw) & x->amask;
    if (mask & 0x100) c->e = (uint8) pulln (x, sp, 1);
    if (mask & 0x200) c->f = (uint8) pulln (x, sp, 1);
    if (mask & 0x400) c->v = (uint32) pulln (x, sp, pw);
    if (mask & 0x800) c->ds = (uint16) pulln (x, sp, 2);
}


/* ---------------------------------------------------------------------------------------------------------------- */
/* Exceptions: SWI, TRAP, interrupts, RTI, CWAI, SYNC (63F09.vhd, INTERRUPT_*, RTI_*, CWAI_*, SYNC_STATE)             */
/* ---------------------------------------------------------------------------------------------------------------- */

/* Vector numbers: the vector is the 16-bit word at 0xFFFFFFC0 + 2 * n, the core puts 0xFFFF in front */
#define V_TRAP  0x18
#define V_SWI3  0x19
#define V_SWI2  0x1a
#define V_FIRQ  0x1b
#define V_IRQ   0x1c
#define V_SWI   0x1d
#define V_NMI   0x1e

static uint32 md_full (const m63f09_core_t *c)
{
    return ((uint32) c->imask << 8) | c->md;
}

/* Frame pushed at S: E set and every register (full), or E clear and PC, [MD], CC only (fast).  PC and S first, CC last. */
static void push_frame (ctx_t *x, int full)
{
    m63f09_core_t *c = x->c;
    uint32 *sp = &c->s;
    int pw = x->ptrw;

    if (full)
        c->cc |= CC_E;
    else
        c->cc &= (uint8) ~CC_E;
    pushn (x, sp, x->pc, pw);
    if (full) {
        if (x->wide)
            pushn (x, sp, c->v, 4);
        pushn (x, sp, c->u, pw);
        pushn (x, sp, c->y, pw);
        pushn (x, sp, c->x, pw);
        push1 (x, sp, c->dp);
        if (c->md & MD_NATIVE) {
            push1 (x, sp, c->f);
            push1 (x, sp, c->e);
        }
        push1 (x, sp, c->b);
        push1 (x, sp, c->a);
        if (x->wide)
            pushn (x, sp, c->ds, 2);
    }
    if (x->wide)
        pushn (x, sp, md_full (c), 4);
    push1 (x, sp, c->cc);
}

/* Masks set once the CC is stacked (INTERRUPT_PUSH_CC_STATE, CWAI_VECTOR_STATE) */
static void exc_masks (m63f09_core_t *c, int vec)
{
    switch (vec) {
        case V_IRQ: c->cc |= CC_I; break;
        case V_SWI: case V_FIRQ: case V_NMI: c->cc |= CC_I | CC_F; break;
        case V_SWI2: case V_SWI3: case V_TRAP: break;
        default:                                        /* EIRQ n: masked while it is active on level */
            if (!(c->md & MD_ED))
                c->imask |= 1u << (23 - vec);
    }
}

static void take_vector (ctx_t *x, int vec)
{
    uint32 v = (uint32) rdn (x, 0xFFFFFFC0u + 2 * (uint32) vec, 2);

    if (x->wide) {
        x->pc = 0xFFFF0000u | v;
        x->upper = 0;
    } else
        x->pc = v;
}

/* Full exception: frame, masks, vector.  x->pc is the PC to stack. */
static void do_exception (ctx_t *x, int vec, int full)
{
    push_frame (x, full);
    exc_masks (x->c, vec);
    take_vector (x, vec);
}

/* RTI: CC, then E tells a full frame from a fast one (63F09.vhd RTI_STATE...).  In 32 bits MD comes back too. */
static void do_rti (ctx_t *x)
{
    m63f09_core_t *c = x->c;
    uint32 *sp = &c->s;
    int pw = x->ptrw, full;

    c->cc = (uint8) pulln (x, sp, 1);
    full = (c->cc & CC_E) != 0;
    if (x->wide) {
        uint32 m = (uint32) pulln (x, sp, 4);

        c->md = (uint8) m;
        c->imask = m >> 8;
    }
    if (full) {
        if (x->wide)
            c->ds = (uint16) pulln (x, sp, 2);
        c->a = (uint8) pulln (x, sp, 1);
        c->b = (uint8) pulln (x, sp, 1);
        if (c->md & MD_NATIVE) {
            c->e = (uint8) pulln (x, sp, 1);
            c->f = (uint8) pulln (x, sp, 1);
        }
        c->dp = (uint8) pulln (x, sp, 1);
        c->x = (uint32) pulln (x, sp, pw);
        c->y = (uint32) pulln (x, sp, pw);
        c->u = (uint32) pulln (x, sp, pw);
        if (x->wide)
            c->v = (uint32) pulln (x, sp, 4);
    }
    if (x->wide)
        c->md &= (uint8) ~(MD_IE | MD_DZ);              /* CLEAR_TRAP_MD, in RTI_PC_STATE: the 32-bit sequence only */
    x->pc = (uint32) pulln (x, sp, pw) & x->amask;
}

/* Interrupt lines ------------------------------------------------------------------------------------------------- */

void m63f09_set_line (m63f09_core_t *c, int line, int level)
{
    uint32 bit = 1u << line;

    if (level && !(c->irq_lines & bit)) {
        if (line == M63_LINE_NMI)
            c->nmi_pend = 1;
        else if (line >= M63_LINE_EIRQ)
            c->eirq_edge |= 1u << (line - M63_LINE_EIRQ);
    }
    if (level)
        c->irq_lines |= bit;
    else
        c->irq_lines &= ~bit;
}

/* The interrupt the core would take now, or 0: sets *vec and *full.  Priority NMI, FIRQ, IRQ, EIRQ23 .. EIRQ0 (EIRQ0 last) */
static int find_int (m63f09_core_t *c, int *vec, int *full)
{
    int i;

    if (c->nmi_pend && c->s_loaded) {
        *vec = V_NMI;
        *full = 1;
        return 1;
    }
    if ((c->irq_lines & 1) && !(c->cc & CC_F)) {
        *vec = V_FIRQ;
        *full = (c->md & MD_FM) != 0;
        return 1;
    }
    if ((c->irq_lines & 2) && !(c->cc & CC_I)) {
        *vec = V_IRQ;
        *full = 1;
        return 1;
    }
    if (c->md & MD_32) {
        for (i = 23; i >= 0; i--) {
            int masked = (c->imask >> (23 - i)) & 1;

            if (c->md & MD_ED) {
                if (!((c->eirq_edge >> i) & 1))
                    continue;
                if (masked) {                           /* a request on edge that finds its mask set is lost */
                    c->eirq_edge &= ~(1u << i);
                    continue;
                }
            } else if (!((c->irq_lines >> (M63_LINE_EIRQ + i)) & 1) || masked)
                continue;
            *vec = i;
            *full = (c->md & MD_FM) != 0;
            return 1;
        }
    }
    return 0;
}

static void ack_int (m63f09_core_t *c, int vec)
{
    if (vec == V_NMI)
        c->nmi_pend = 0;
    else if (vec < 24)
        c->eirq_edge &= ~(1u << vec);
}

static void init_ctx (ctx_t *x, m63f09_core_t *c)
{
    memset (x, 0, sizeof (*x));
    x->c = c;
    x->wide = (c->md & MD_32) != 0;
    x->amask = x->wide ? 0xFFFFFFFFu : 0xFFFFu;
    x->ptrw = x->wide ? 4 : 2;
    x->pc = c->pc & x->amask;
    x->upper = c->pc & ~x->amask;
}

static int any_line (m63f09_core_t *c)
{
    return c->nmi_pend || (c->irq_lines & 0x3FFFFFFu);
}

int m63f09_wake_pending (m63f09_core_t *c)
{
    int vec, full;

    return find_int (c, &vec, &full) || (c->wait == WAIT_SYNC && any_line (c));
}

/* Called before each instruction: takes an interrupt, or leaves the wait state.  Returns 1 when an exception was taken, 2 when the
   core stays idle (CWAI/SYNC), 0 to execute the next instruction. */
static int service_interrupts (m63f09_core_t *c, int *cycles, int *stop)
{
    ctx_t xs, *x = &xs;
    int vec, full;

    *stop = 0;
    if (!c->wait && !(c->nmi_pend | c->irq_lines | c->eirq_edge))
        return 0;
    init_ctx (x, c);
    if (find_int (c, &vec, &full)) {
        ack_int (c, vec);
        if (c->wait == WAIT_CWAI) {                     /* the whole state is already stacked: masks and vector only */
            exc_masks (c, vec);
            take_vector (x, vec);
        } else
            do_exception (x, vec, full);
        c->wait = 0;
        c->pc = x->upper | (x->pc & x->amask);
        *cycles = full ? 21 : 10;
        *stop = x->stop;
        return 1;
    }
    if (c->wait == WAIT_SYNC && any_line (c)) {          /* a masked interrupt: SYNC is over, the next instruction runs */
        c->wait = 0;
        c->nmi_pend = 0;
        return 0;
    }
    if (c->wait) {
        *cycles = 1;
        return 2;
    }
    return 0;
}

/* TFM r0,r1 copies W bytes; W ends at 0.  The registers move as the variant says (+ or - or none). */
static void do_tfm (ctx_t *x, int mn)
{
    uint8 pb = f8 (x);
    int r0 = pb >> 4, r1 = pb & 0xf;
    int sinc = (mn == MN_TFM_II || mn == MN_TFM_ID) ? 1 : (mn == MN_TFM_DD ? -1 : 0);
    int dinc = (mn == MN_TFM_II || mn == MN_TFM_DI) ? 1 : (mn == MN_TFM_DD ? -1 : 0);

    if (r0 > R_S || r1 > R_S) {                         /* only D X Y U S */
        x->stop = TRAP_ILL ();
        return;
    }
    while ((getreg (x, R_W) & 0xFFFF) != 0 && !x->stop) {
        t_uint64 s = getreg (x, r0), d = getreg (x, r1);

#if M63_RTL_TFM_LOADS_CC
        /* BLOCK_TRANSFER_STATE computes W - 1 on 32 bits with CC_CTRL <= LOAD_CC at every iteration: N, Z, V and C take the flags of
           that subtraction (N = V = C = 0, Z = 1 on the last iteration).  The 6309 reference says only Z is set (to 1) and N, V, C
           are unaffected: a deviation of the RTL from the manual, reproduced here until the VHDL is changed. */
        set_flag (x, CC_N, 0);
        set_flag (x, CC_Z, (getreg (x, R_W) & 0xFFFF) == 1);
        set_flag (x, CC_V, 0);
        set_flag (x, CC_C, 0);
#endif

        wr8 (x, (uint32) d, rd8 (x, (uint32) s));
        setreg (x, r0, s + sinc);
        setreg (x, r1, d + dinc);
        setreg (x, R_W, getreg (x, R_W) - 1);
    }
}

/* BAND BIAND BOR BIOR BEOR BIEOR LDBT STBT : one bit of a register (CC, A or B) and one bit of a byte of the direct page */
static void do_bittr (ctx_t *x, int mn)
{
    uint8 pb = f8 (x);
    uint32 ea = direct_ea (x);
    int reg = ((pb >> 6) & 3) == 0 ? R_CC : ((pb >> 6) & 3) == 1 ? R_A : R_B;
    int rb = pb & 7, mb = (pb >> 3) & 7;
    uint8 m;

    /* STBT is the other way round (63F09.vhd BIT_MANIPULATION3/4 and the 6309 reference: "STBT r,sBit,dBit,addr", the SOURCE
       is the register bit, in bits 5-3 of the postbyte, the DESTINATION the memory bit, in bits 2-0): the other seven read a
       memory bit from bits 5-3 and use the register bit of bits 2-0. */
    if (mn == MN_STBT) {
        rb = (pb >> 3) & 7;
        mb = pb & 7;
    }
    m = rd8 (x, ea);
    uint8 r = (uint8) getreg (x, reg);
    int rbit = (r >> rb) & 1, mbit = (m >> mb) & 1, res = rbit;

    switch (mn) {
        case MN_BAND:  res = rbit & mbit; break;
        case MN_BIAND: res = rbit & !mbit; break;
        case MN_BOR:   res = rbit | mbit; break;
        case MN_BIOR:  res = rbit | !mbit; break;
        case MN_BEOR:  res = rbit ^ mbit; break;
        case MN_BIEOR: res = rbit ^ !mbit; break;
        case MN_LDBT:  res = mbit; break;
        case MN_STBT:
            m = (uint8) ((m & ~(1 << mb)) | (rbit << mb));
            wr8 (x, ea, m);
            return;
    }
    r = (uint8) ((r & ~(1 << rb)) | (res << rb));
    setreg (x, reg, r);
}

static void do_daa (ctx_t *x)
{
    uint8 a = x->c->a, cf = 0, cc = x->c->cc;
    unsigned r;

    if ((a & 0x0F) > 9 || (cc & CC_H)) cf |= 0x06;
    if ((a & 0xF0) > 0x90 || (cc & CC_C) || ((a & 0xF0) > 0x80 && (a & 0x0F) > 9)) cf |= 0x60;
    r = (unsigned) a + cf;
    x->c->a = (uint8) r;
    set_nz (x, (uint8) r, 1);
    if (cf & 0x60)
        set_flag (x, CC_C, 1);
}

/* MULD, DIVD, DIVQ (63F09.vhd, the DIVD and DIVQ states ; the 6309 reference for the 16-bit state).
   MULD : Q = D * M16 (signed), N and Z from the 32-bit product, V and C unchanged.
   32-bit state : DIVD = D / M16, quotient in W, remainder in D ; DIVQ = Q / M32, quotient in Q, remainder in V ; N Z from the
   quotient, C = its bit 0, V cleared.  The remainder has the sign of the dividend.
   16-bit state : DIVD = D / M8 -> B (quotient), A (remainder) ; DIVQ = Q / M16 -> W, D.  A quotient that does not fit in twice
   its width aborts the instruction (registers kept, V set, N Z C cleared) ; otherwise V = "true quotient negative" xor the stored
   sign bit.  Division by zero is a TRAP (not implemented here).  Returns 0 or a STOP_* code. */
static int do_muldiv (ctx_t *x, const m63f09_op_t *row)
{
    m63f09_core_t *c = x->c;
    int w = x->wide;
    int mn = row->mn;
    int sz = mn == MN_MULD ? 2 : mn == MN_DIVD ? (w ? 2 : 1) : mn == MN_DIVQ ? (w ? 4 : 2)
           : (mn == MN_MULDU || mn == MN_DIVDU) ? 2 : (mn == MN_MULQ || mn == MN_MULQU || mn == MN_DIVQU) ? 4 : 8;
    t_uint64 v;
    t_int64 m, dividend, q, r;

    switch (row->mode) {
        case M63_ADDR_IMM8:  v = fn (x, 1); break;
        case M63_ADDR_IMM16: v = fn (x, 2); break;
        case M63_ADDR_IMM32: v = fn (x, 4); break;
        case M63_ADDR_IMM64: v = fn (x, 8); break;
        default: {
            uint32 ea = get_ea (x, row, sz);

            if (x->stop) return x->stop;
            v = rdn (x, ea, sz);
        }
    }
    if (x->stop) return x->stop;
    m = sext (v, sz * 8);

    /* 63F09 only (32-bit state): the unsigned and the 64-bit families.  Flags of the multiplications (owner's rule, regress/gen_mulflags.py):
       signed MULD MULQ MULO: Z and N from the result, C unchanged, V unchanged except MULO (V = the signed product does not fit) ;
       unsigned MULDU MULQU MULOU: Z from the result, C = bit (w-1) of the result (MULDU bit 15, MULQU bit 31, MULOU bit 63), N unchanged,
       V unchanged except MULOU (V = the product does not fit).  Divisions: quotient in W (DIVDU), Q (DIVQU) or O (DIVO, DIVOU) and
       remainder in D, V or PUSHED ON THE U STACK (8 bytes) for DIVO and DIVOU ; N Z from the quotient, C = its bit 0, V = 0. */
    switch (mn) {
        case MN_MULDU: {
            t_uint64 p = (getreg (x, R_D) & 0xFFFF) * (v & 0xFFFF);

            setreg (x, R_Q, p);
            set_flag (x, CC_Z, p == 0);
            set_flag (x, CC_C, (p & 0x8000) != 0);
            return 0;
        }
        case MN_MULQ: {
            t_int64 p = sext (getreg (x, R_Q), 32) * m;

            setreg (x, R_O, (t_uint64) p);
            set_nz (x, (t_uint64) p, 8);
            return 0;
        }
        case MN_MULQU: {
            t_uint64 p = (getreg (x, R_Q) & 0xFFFFFFFFULL) * (v & 0xFFFFFFFFULL);

            setreg (x, R_O, p);
            set_flag (x, CC_Z, p == 0);
            set_flag (x, CC_C, (p & 0x80000000ULL) != 0);
            return 0;
        }
        case MN_MULO: {
            __int128 p = (__int128) (t_int64) getreg (x, R_O) * (__int128) m;
            t_uint64 lo = (t_uint64) p;

            setreg (x, R_O, lo);
            set_nz (x, lo, 8);
            set_flag (x, CC_V, p != (__int128) (t_int64) lo);
            return 0;
        }
        case MN_MULOU: {
            unsigned __int128 p = (unsigned __int128) getreg (x, R_O) * (unsigned __int128) v;
            t_uint64 lo = (t_uint64) p;

            setreg (x, R_O, lo);
            set_flag (x, CC_Z, lo == 0);
            set_flag (x, CC_C, (lo >> 63) != 0);
            set_flag (x, CC_V, (p >> 64) != 0);
            return 0;
        }
        case MN_DIVDU: case MN_DIVQU: case MN_DIVOU: case MN_DIVO: {
            int unsig = mn != MN_DIVO;
            int qb = mn == MN_DIVDU ? 2 : mn == MN_DIVQU ? 4 : 8;
            t_uint64 uq, ur, ud = mn == MN_DIVDU ? (getreg (x, R_D) & 0xFFFF) : mn == MN_DIVQU ? (getreg (x, R_Q) & 0xFFFFFFFFULL) : getreg (x, R_O);

            if ((v & wmask (qb)) == 0)
                return TRAP_DIV ();                       /* division by zero: TRAP */
            if (unsig) {
                uq = ud / (v & wmask (qb));
                ur = ud % (v & wmask (qb));
            } else if ((t_int64) v == -1) {
                uq = (t_uint64) 0 - ud;                 /* avoids the overflow of INT64_MIN / -1 */
                ur = 0;
            } else {
                uq = (t_uint64) ((t_int64) ud / (t_int64) v);
                ur = (t_uint64) ((t_int64) ud % (t_int64) v);
            }
            uq &= wmask (qb);
            if (mn == MN_DIVDU) {
                setreg (x, R_W, uq);
                setreg (x, R_D, ur & 0xFFFF);
            } else if (mn == MN_DIVQU) {
                setreg (x, R_Q, uq);
                c->v = (uint32) ur;
            } else {
                setreg (x, R_O, uq);
                pushn (x, &c->u, ur, 8);
            }
            set_nz (x, uq, qb);
            set_flag (x, CC_C, (uq & 1) != 0);
            set_flag (x, CC_V, 0);
            return 0;
        }
        default:
            break;
    }
    if (row->mn == MN_MULD) {
        t_int64 p = sext (getreg (x, R_D), 16) * m;

        setreg (x, R_Q, (t_uint64) p & 0xFFFFFFFFULL);
        set_nz (x, (t_uint64) p & 0xFFFFFFFFULL, 4);
        return 0;
    }
    if (m == 0)
        return TRAP_DIV ();                               /* division by zero: TRAP */
    dividend = row->mn == MN_DIVD ? sext (getreg (x, R_D), 16) : sext (getreg (x, R_Q), 32);
    q = dividend / m;
    r = dividend % m;
    if (w) {
        int qb = row->mn == MN_DIVD ? 2 : 4;
        t_uint64 qv = (t_uint64) q & wmask (qb);

        if (row->mn == MN_DIVD) {
            setreg (x, R_W, qv);
            setreg (x, R_D, (t_uint64) r & 0xFFFF);
        } else {
            setreg (x, R_Q, qv);
            c->v = (uint32) r;
        }
        set_nz (x, qv, qb);
        set_flag (x, CC_C, (qv & 1) != 0);
        set_flag (x, CC_V, 0);
        return 0;
    } else {
        int qb = row->mn == MN_DIVD ? 1 : 2;
        t_int64 lim = 1LL << (qb * 8);
        t_uint64 qv = (t_uint64) q & wmask (qb);

        if (q > lim - 1 || q < -lim) {                  /* range overflow: nothing is written */
            set_flag (x, CC_N, 0);
            set_flag (x, CC_Z, 0);
            set_flag (x, CC_C, 0);
            set_flag (x, CC_V, 1);
            return 0;
        }
        if (row->mn == MN_DIVD) {
            setreg (x, R_B, qv);
            setreg (x, R_A, (t_uint64) r & 0xFF);
        } else {
            setreg (x, R_W, qv);
            setreg (x, R_D, (t_uint64) r & 0xFFFF);
        }
        set_nz (x, qv, qb);
        set_flag (x, CC_C, (qv & 1) != 0);
        set_flag (x, CC_V, (q < 0) != ((qv & wsign (qb)) != 0));
        return 0;
    }
}

/* Shift and rotate by n bits (63F09 only; rev. 67 of binutils-gdb/CLAUDE.md, 63F09.vhd SHIFTN*_STATE).  Result, C, V:
   LSRN, ASRN, ASLN/LSLN : C = the last bit shifted out, N and Z from the result ; V = signed overflow for ASLN/LSLN, kept by the others ;
   RORN, ROLN : pure rotations (the carry does not take part), count taken modulo the width, C = the last bit moved out ;
   a count of 0 changes only N and Z (C and V are kept). */
static t_uint64 shn_apply (ctx_t *x, int op, t_uint64 v, unsigned cnt, int bytes)
{
    int wbits = bytes * 8;
    t_uint64 m = wmask (bytes), r;
    int c;

    v &= m;
    if (cnt == 0) {
        set_nz (x, v, bytes);
        return v;
    }
    switch (op) {
        case O_LSR:
            c = cnt <= (unsigned) wbits ? (int) ((v >> (cnt - 1)) & 1) : 0;
            r = cnt >= (unsigned) wbits ? 0 : v >> cnt;
            break;
        case O_ASR: {
            t_int64 sv = sext (v, wbits);

            c = cnt <= (unsigned) wbits ? (int) ((v >> (cnt - 1)) & 1) : (int) ((v >> (wbits - 1)) & 1);
            r = (t_uint64) (cnt >= (unsigned) wbits ? (sv < 0 ? -1 : 0) : (sv >> cnt)) & m;
            break;
        }
        case O_ASL: {
            __int128 sv = (__int128) sext (v, wbits);

            c = cnt <= (unsigned) wbits ? (int) ((v >> (wbits - cnt)) & 1) : 0;
            r = cnt >= (unsigned) wbits ? 0 : (v << cnt) & m;
            set_flag (x, CC_V, (sv << cnt) != (__int128) sext (r, wbits));      /* the signed value no longer fits */
            break;
        }
        case O_ROL: {
            unsigned k = cnt % wbits;

            r = k ? ((v << k) | (v >> (wbits - k))) & m : v;
            c = (int) (r & 1);
            break;
        }
        default: {                                      /* O_ROR */
            unsigned k = cnt % wbits;

            r = k ? ((v >> k) | (v << (wbits - k))) & m : v;
            c = (int) ((r >> (wbits - 1)) & 1);
            break;
        }
    }
    set_flag (x, CC_C, c);
    set_nz (x, r, bytes);
    return r;
}

/* Operation of the opcode of a register form (page 0), and the register and count of its operand byte; 0 when the byte is a TRAP. */
static int shn_decode (uint8 opc, uint8 b, int *op, int *reg, unsigned *cnt)
{
    static const int set2[6] = { R_X, R_Y, R_U, R_S, R_V, R_Q };
    static const int set1[4] = { R_A, R_B, R_E, R_F };

    switch (opc) {
        case 0x42: case 0x52: *op = O_LSR; break;
        case 0x45: case 0x55: *op = O_ASR; break;
        case 0x4b: case 0x5b: *op = O_ASL; break;
        case 0x4e: case 0x8f: *op = O_ROL; break;
        default:              *op = O_ROR; break;       /* 0x5e, 0x87 */
    }
    if (opc == 0x52 || opc == 0x55 || opc == 0x5b || opc == 0x87 || opc == 0x8f) {
        if ((b >> 5) > 5)
            return 0;
        *reg = set2[b >> 5];
        *cnt = b & 0x1f;
    } else if ((b >> 6) == 2) {
        *reg = R_O;
        *cnt = b & 0x3f;
    } else if ((b >> 5) == 0) {
        *reg = set1[(b >> 3) & 3];
        *cnt = b & 7;
    } else if ((b >> 5) == 2) {
        *reg = (b & 0x10) ? R_W : R_D;
        *cnt = b & 0xf;
    } else
        return 0;
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* One instruction                                                                                                   */
/* ---------------------------------------------------------------------------------------------------------------- */


/* ---------------------------------------------------------------------------------------------------------------- */
/* FPU (page 0x18): stack of 16 registers, 63F09.vhd FP_STACK_CTRL and 63F39.vhd                                      */
/* ---------------------------------------------------------------------------------------------------------------- */

static void fp_push (m63f09_core_t *c, t_uint64 v)
{
    memmove (&c->fp[1], &c->fp[0], 15 * sizeof (c->fp[0]));
    c->fp[0] = v;
}

static void fp_drop (m63f09_core_t *c, int n)           /* the registers uncovered at the bottom are cleared */
{
    int i;

    if (n >= 16) n = 16;
    memmove (&c->fp[0], &c->fp[n], (16 - n) * sizeof (c->fp[0]));
    for (i = 16 - n; i < 16; i++)
        c->fp[i] = 0;
}

/* PSHSFP/PSHUFP and the N forms: FP0 .. FP(n-1) then FPCC (lowest address), each register big-endian, dropped as it goes */
static void fp_pushregs (ctx_t *x, int use_u, int n)
{
    m63f09_core_t *c = x->c;
    uint32 *sp = use_u ? &c->u : &c->s;
    int i;

    for (i = 0; i < n; i++) {
        pushn (x, sp, c->fp[0], 8);
        fp_drop (c, 1);
    }
    push1 (x, sp, c->fpcc);
}

static void fp_pullregs (ctx_t *x, int use_u, int n)
{
    m63f09_core_t *c = x->c;
    uint32 *sp = use_u ? &c->u : &c->s;
    int i;

    c->fpcc = (uint8) pulln (x, sp, 1);
    for (i = 0; i < n; i++)
        fp_push (c, pulln (x, sp, 8));
}

static int do_fpu (ctx_t *x, const m63f09_op_t *row)
{
    m63f09_core_t *c = x->c;
    uint8 op = row->opcode;
    int hi = op >> 4, lo = op & 15, i;
    const char *nm = row->name;

    switch (hi) {
        case 0x0:                                       /* SETRNE SETRTZ SETRDN SETRUP SETRMM ($1805, SETDYN, is reserved) */
            if (lo > 4)
                return TRAP_ILL ();
            c->fpcc = (uint8) ((c->fpcc & 0x1F) | (lo << 5));
            return 0;
        case 0x1:
            if (lo <= 3) {
                if (lo < 2 || 1) {
                    if (lo == 0 || lo == 1)
                        fp_pushregs (x, lo == 1, 16);
                    else
                        fp_pullregs (x, lo == 3, 16);
                }
                return 0;
            }
            if (lo == 4) {                              /* TFROFP: push A:B:E:F:V */
                fp_push (c, ((t_uint64) c->a << 56) | ((t_uint64) c->b << 48) | ((t_uint64) c->e << 40) | ((t_uint64) c->f << 32) | c->v);
                return 0;
            }
            if (lo == 5) {                              /* TFRFPO */
                t_uint64 v = c->fp[0];

                c->a = (uint8) (v >> 56); c->b = (uint8) (v >> 48); c->e = (uint8) (v >> 40); c->f = (uint8) (v >> 32);
                c->v = (uint32) v;
                fp_drop (c, 1);
                return 0;
            }
            if (lo >= 6 && lo <= 9) {                   /* PSHSNFP PSHUNFP PULSNFP PULUNFP #n */
                int n = f8 (x);

                if (n > 16) n = 16;
                if (!strcmp (nm, "pshsnfp")) fp_pushregs (x, 0, n);
                else if (!strcmp (nm, "pshunfp")) fp_pushregs (x, 1, n);
                else if (!strcmp (nm, "pulsnfp")) fp_pullregs (x, 0, n);
                else fp_pullregs (x, 1, n);
                return 0;
            }
            return TRAP_ILL ();
        case 0x2: {                                     /* BNV BDZ BOV BUN BIX and their long forms */
            static const uint8 bit[5] = { 0x10, 0x08, 0x04, 0x02, 0x01 };
            t_int64 off;
            int k = lo & 7;

            if (row->mode == M63_ADDR_REL8) off = sext (fn (x, 1), 8);
            else if (row->mode == M63_ADDR_REL16) off = sext (fn (x, 2), 16);
            else off = sext (fn (x, 4), 32);
            if (k > 4)
                return TRAP_ILL ();
            if (c->fpcc & bit[k])
                x->pc = (x->pc + (uint32) off) & x->amask;
            return 0;
        }
        case 0x3: case 0xC: {                           /* conversions */
            t_uint64 res = 0;
            uint8 fl = 0;
            int rm = c->fpcc >> 5;
            t_uint64 d = c->fp[0];

            if (hi == 0xC) {
                switch (lo) {
                    case 0x0: m63f09_fpu_cvt (0, 0, 1, rm, d, &res, &fl); break;           /* CVTD2S */
                    case 0x1: m63f09_fpu_cvt (0, 0, 0, rm, d, &res, &fl); break;           /* CVTS2D */
                    case 0x4: m63f09_fpu_cvt (1, 0, 0, rm, d, &res, &fl); break;           /* CVTI2D */
                    case 0x5: m63f09_fpu_cvt (1, 1, 0, rm, d, &res, &fl); break;
                    case 0x6: m63f09_fpu_cvt (1, 0, 1, rm, d, &res, &fl); break;
                    case 0x7: m63f09_fpu_cvt (1, 1, 1, rm, d, &res, &fl); break;
                    case 0x8: m63f09_fpu_cvt (2, 0, 1, rm, d, &res, &fl); break;           /* CVTS2I */
                    case 0x9: m63f09_fpu_cvt (2, 1, 1, rm, d, &res, &fl); break;
                    case 0xA: m63f09_fpu_cvt (2, 0, 0, rm, d, &res, &fl); break;
                    case 0xB: m63f09_fpu_cvt (2, 1, 0, rm, d, &res, &fl); break;
                    default: return TRAP_ILL ();
                }
            } else {
                switch (lo) {
                    case 0x4: m63f09_fpu_cvt (1, 2, 0, rm, d, &res, &fl); break;           /* CVTOI2D */
                    case 0x5: m63f09_fpu_cvt (1, 3, 0, rm, d, &res, &fl); break;
                    case 0x6: m63f09_fpu_cvt (1, 2, 1, rm, d, &res, &fl); break;
                    case 0x7: m63f09_fpu_cvt (1, 3, 1, rm, d, &res, &fl); break;
                    case 0x8: m63f09_fpu_cvt (2, 2, 1, rm, d, &res, &fl); break;           /* CVTS2OI */
                    case 0x9: m63f09_fpu_cvt (2, 3, 1, rm, d, &res, &fl); break;
                    case 0xA: m63f09_fpu_cvt (2, 2, 0, rm, d, &res, &fl); break;
                    case 0xB: m63f09_fpu_cvt (2, 3, 0, rm, d, &res, &fl); break;
                    default: return TRAP_ILL ();
                }
            }
            fp_drop (c, 1);
            fp_push (c, res);
            c->fpcc = (uint8) ((c->fpcc & 0xE0) | fl);
            return 0;
        }
        case 0x8: case 0x9: case 0xA: case 0xB: {       /* LDSFP LDDFP STSFP STDFP */
            int dbl = (lo & 1), st = (lo >= 2);
            int bytes = dbl ? 8 : 4;

            if (!st) {
                t_uint64 v;

                if (row->mode == M63_ADDR_IMM32) v = fn (x, 4);
                else if (row->mode == M63_ADDR_IMM64) v = fn (x, 8);
                else {
                    uint32 ea = get_ea (x, row, bytes);

                    if (x->stop) return x->stop;
                    v = rdn (x, ea, bytes);
                }
                if (x->stop) return x->stop;
                if (!dbl)                               /* LDSFP: the 32 bits, sign-extended or not (see M63_RTL_LDSFP_SIGNEXT) */
                    v = M63_RTL_LDSFP_SIGNEXT ? (t_uint64) (t_int64) (int32) (uint32) v : (t_uint64) (uint32) v;
                fp_push (c, v);
            } else {
                uint32 ea = get_ea (x, row, bytes);

                if (x->stop) return x->stop;
                wrn (x, ea, bytes, c->fp[0]);
                fp_drop (c, 1);
            }
            return 0;
        }
        case 0xD: {                                     /* stack manipulation */
            t_uint64 t;

            switch (lo) {
                case 0x0: fp_drop (c, 1); break;
                case 0x1: fp_drop (c, 2); break;
                case 0x2: fp_push (c, c->fp[0]); break;
                case 0x3: memmove (&c->fp[2], &c->fp[0], 14 * sizeof (c->fp[0])); break;
                case 0x4: t = c->fp[0]; c->fp[0] = c->fp[1]; c->fp[1] = t; break;
                case 0x5: fp_push (c, c->fp[1]); break;
                case 0x6: t = c->fp[0]; c->fp[0] = c->fp[2]; c->fp[2] = c->fp[1]; c->fp[1] = t; break;
                case 0xC: memset (c->fp, 0, sizeof (c->fp)); break;
                case 0x7: case 0x8: case 0x9: case 0xA: case 0xB: case 0xD: {
                    uint8 b = f8 (x);
                    int n = b & 0xF, m = b >> 4, nc = n + 1;

                    if (lo == 0x7) {                    /* DUPN */
                        if (nc >= 1 && nc <= 15)
                            memmove (&c->fp[nc], &c->fp[0], (16 - nc) * sizeof (c->fp[0]));
                    } else if (lo == 0x8) {             /* DROPN */
                        if (nc >= 16)
                            memset (c->fp, 0, sizeof (c->fp));
                        else
                            fp_drop (c, nc);
                    } else if (lo == 0x9) {             /* ROLLN: the n-th goes to the top */
                        if (nc >= 2) {
                            t = c->fp[nc - 1];
                            memmove (&c->fp[1], &c->fp[0], (nc - 1) * sizeof (c->fp[0]));
                            c->fp[0] = t;
                        }
                    } else if (lo == 0xA) {             /* ROLLDN: the top goes to the n-th */
                        if (nc >= 2) {
                            t = c->fp[0];
                            memmove (&c->fp[0], &c->fp[1], (nc - 1) * sizeof (c->fp[0]));
                            c->fp[nc - 1] = t;
                        }
                    } else if (lo == 0xB)               /* PICKN */
                        fp_push (c, c->fp[n]);
                    else {                              /* EXGFP */
                        t = c->fp[n]; c->fp[n] = c->fp[m]; c->fp[m] = t;
                    }
                    break;
                }
                default: return TRAP_ILL ();
            }
            return 0;
        }
        case 0x6: case 0x7: case 0xE: case 0xF: {       /* arithmetic: 6x 7x keep the arguments, Ex Fx pop them */
            int keep = !(op & 0x80), s32 = (hi == 0x6 || hi == 0xE);
            int nargs = lo <= 3 ? 3 : (lo == 8 || lo == 0xC) ? 1 : 2;
            t_uint64 d1 = 0, d2 = 0, d3 = 0, res = 0;
            uint8 fl = 0;
            int rm = c->fpcc >> 5;

            if (lo > 0xC || lo == 0x0 + 0x80)
                return TRAP_ILL ();
            if (nargs == 3) { d1 = c->fp[2]; d2 = c->fp[1]; d3 = c->fp[0]; }
            else if (nargs == 2) { d1 = c->fp[1]; d2 = c->fp[0]; }
            else d1 = c->fp[0];
            if (!keep)
                fp_drop (c, nargs);
            m63f09_fpu_exec (lo, s32, rm, d1, d2, d3, &res, &fl);
            c->fpcc = (uint8) ((c->fpcc & 0xE0) | fl);
            if (lo == 0xA) {                            /* FCMP: the result goes to Z of the CPU, nothing is stacked */
                if (res)
                    c->cc |= CC_Z;
                else
                    c->cc &= (uint8) ~CC_Z;
            } else if (keep)
                c->fp[0] = res;
            else
                fp_push (c, res);
            (void) i;
            return 0;
        }
    }
    return UNIMPL ();
}

/* Returns 0 and the cycles in *cycles, or a stop code (the PC then stays on the instruction) */
static int step_inner (m63f09_core_t *c, int *cycles)
{
    ctx_t xs, *x = &xs;
    uint8 b0, opc;
    uint8 page = 0;
    const m63f09_op_t *row;
    const xd_t *d;
    uint16 arch = m63f09_arch_of_md (c->md);
    int ai = arch_index (arch);

    if (!dtab_ready)
        build_tables ();
    init_ctx (x, c);

    b0 = f8 (x);
    opc = b0;
    if (b0 == 0x10 || b0 == 0x11 || b0 == 0x18 || b0 == 0x1b) {
        page = b0;
        opc = f8 (x);
    }
    if (x->stop)
        return x->stop;
    row = dtab[ai][page_index (page)][opc];
    if (!row)
        return TRAP_ILL ();                             /* TRAP: not implemented yet */
    d = &xdesc[row - m63f09_optab];
    *cycles = (ai == 0 ? row->cycles_6809 : ai == 1 ? row->cycles_6309 : row->cycles_63f09);
    if (*cycles == 0)
        *cycles = row->cycles_63f09 ? row->cycles_63f09 : row->cycles_6309 ? row->cycles_6309 : 2;

    switch (d->kind) {
        case K_ALU: {
            int bytes = reg_width (x, d->reg);
            t_uint64 b, a = getreg (x, d->reg), res = 0;

            if (d->op == O_ST) {
                uint32 ea = get_ea (x, row, bytes);

                if (x->stop) return x->stop;
                alu_logic (x, a, bytes);
                wrn (x, ea, bytes, a);
                break;
            }
            switch (row->mode) {
                case M63_ADDR_IMM8:  b = fn (x, 1); break;
                case M63_ADDR_IMM16: b = fn (x, 2); break;
                case M63_ADDR_IMM32: b = fn (x, 4); break;
                case M63_ADDR_IMM64: b = fn (x, 8); break;
                default: {
                    uint32 ea = get_ea (x, row, bytes);

                    if (x->stop) return x->stop;
                    b = rdn (x, ea, bytes);
                    break;
                }
            }
            if (alu_binop (x, d->op, a, b, bytes, &res))
                setreg (x, d->reg, res);
            break;
        }
        case K_UN: {
            if (d->reg != R_NONE) {
                int bytes = reg_width (x, d->reg);

                setreg (x, d->reg, alu_un (x, d->op, getreg (x, d->reg), bytes));
            } else {
                uint32 ea = get_ea (x, row, 1);
                t_uint64 v;

                if (x->stop) return x->stop;
                v = rdn (x, ea, 1);
                v = alu_un (x, d->op, v, 1);
                if (d->op != O_TST)
                    wrn (x, ea, 1, v);
            }
            break;
        }
        case K_LEA: {
            uint32 ea = get_ea (x, row, x->ptrw);

            if (x->stop) return x->stop;
            setreg (x, d->reg, ea);
            if (d->reg == R_X || d->reg == R_Y)
                set_flag (x, CC_Z, ea == 0);
            break;
        }
        case K_BRA: {
            t_int64 off;

            if (row->mode == M63_ADDR_REL8) off = sext (fn (x, 1), 8);
            else if (row->mode == M63_ADDR_REL16) off = sext (fn (x, 2), 16);
            else off = sext (fn (x, 4), 32);
            if (d->cond == 16) {                        /* bsr, lbsr */
                push_ptr (x, x->pc);
                x->pc = (x->pc + (uint32) off) & x->amask;
            } else if (cond_true (c->cc, d->cond)) {
                x->pc = (x->pc + (uint32) off) & x->amask;
                if (d->cond == 0 && x->pc == (c->pc & x->amask))
                    return STOP_LOOP;                   /* bra *: how the test programs end */
            }
            break;
        }
        case K_JMP:
        case K_JSR: {
            uint32 ea = get_ea (x, row, x->ptrw);

            if (x->stop) return x->stop;
            if (d->kind == K_JSR)
                push_ptr (x, x->pc);
            x->pc = ea & x->amask;
            break;
        }
        case K_TFR:
        case K_EXG:
        case K_REGREG: {
            uint8 pb = f8 (x);
            int r0 = pb >> 4, r1 = pb & 0xf;

            if (reg_width (x, r0) != reg_width (x, r1))
                return TRAP_ILL ();                     /* TRAP in the hardware */
            if (d->kind == K_TFR)
                setreg (x, r1, getreg (x, r0));
            else if (d->kind == K_EXG) {
                t_uint64 t = getreg (x, r0);

                setreg (x, r0, getreg (x, r1));
                setreg (x, r1, t);
            } else {                                    /* OP r0,r1 : r1 = r1 OP r0 (datasheet, 63F09.vhd) */
                t_uint64 res = 0;
                int bytes = reg_width (x, r1);
                uint8 h = c->cc & CC_H;
                int op = d->op;
                t_uint64 a = getreg (x, r1), b = getreg (x, r0);

#if M63_RTL_ADCR8_NO_CARRY
                /* 63F09.vhd, ~l.5767: the carry-in is read only for ALU_ADC/ROL/ROR/SBC ; ADCR on 8 bits uses ALU_ADCR, so it
                   adds WITHOUT the carry (16 and 32 bits use ALU_ADC and are right).  Kept bit-exact with the RTL until the
                   VHDL is fixed (proposal: add ALU_ADCR to that condition), then set the macro to 0. */
                if (bytes == 1 && op == O_ADC)
                    op = O_ADD;
#endif
                if (alu_binop (x, op, a, b, bytes, &res))
                    setreg (x, r1, res);
                if (bytes == 1 && (d->op == O_ADD || d->op == O_ADC))
                    set_flag (x, CC_H, h != 0);         /* ADDR and ADCR do not modify H (63F09.vhd, EXECUTE_STATE) */
            }
            break;
        }
        case K_SHN: {
            int op, reg;
            unsigned cnt;
            uint8 b = f8 (x);

            if (!shn_decode (opc, b, &op, &reg, &cnt))
                return TRAP_ILL ();                       /* TRAP: a byte the VHDL does not decode */
            setreg (x, reg, shn_apply (x, op, getreg (x, reg), cnt, reg_width (x, reg)));
            break;
        }
        case K_SHNMEM: {
            uint8 cnt = f8 (x);
            uint32 ea;
            t_uint64 v;

            if (cnt > 7)
                return TRAP_ILL ();                       /* TRAP */
            ea = get_ea (x, row, 1);
            if (x->stop) return x->stop;
            v = shn_apply (x, d->op, rdn (x, ea, 1), cnt, 1);
            wrn (x, ea, 1, v);
            break;
        }
        case K_MISC:
            switch (row->mn) {
                case MN_NOP: break;
                case MN_LDMD: {
                    uint8 v = f8 (x);

                    if (c->s_loaded)
                        return TRAP_ILL ();             /* MD can only be loaded before S (63F09.vhd, LDMD) */
                    c->md = v;
                    break;
                }
                case MN_SWI: case MN_SWI2: case MN_SWI3:
                    do_exception (x, row->mn == MN_SWI ? V_SWI : row->mn == MN_SWI2 ? V_SWI2 : V_SWI3, 1);
                    break;
                case MN_RTI: do_rti (x); break;
                case MN_CWAI: {
                    uint32 m = (uint32) fn (x, x->wide ? 4 : 1);

                    c->cc &= (uint8) m;
                    if (x->wide)
                        c->imask = m >> 8;              /* the mask word is loaded, not and-ed (LOAD_INTERRUPT_MASK_MD) */
                    push_frame (x, 1);
                    c->wait = WAIT_CWAI;
                    break;
                }
                case MN_SYNC: c->wait = WAIT_SYNC; break;
                case MN_BITMD: {
                    uint8 m = (uint8) (f8 (x) & 0xC0), r = c->md & m;

                    set_flag (x, CC_N, (r & 0x80) != 0);
                    set_flag (x, CC_Z, r == 0 || r == 0xC0);    /* both bits: Z forced (pagefault), 63F09.vhd 1.48 */
                    set_flag (x, CC_V, 0);
                    c->md &= (uint8) ~m;
                    break;
                }
                case MN_ANDCC: c->cc &= f8 (x); break;
                case MN_ORCC: c->cc |= f8 (x); break;
                case MN_ABX: c->x = (c->x + c->b) & x->amask; break;
                case MN_SEX: {
                    int neg = (c->b & 0x80) != 0;

                    c->a = neg ? 0xFF : 0;
                    set_nz (x, getreg (x, R_D), 2);
                    break;
                }
                case MN_MUL: {
                    uint32 p = (uint32) c->a * c->b;

                    setreg (x, R_D, p);
                    set_flag (x, CC_Z, (p & 0xFFFF) == 0);
                    set_flag (x, CC_C, (p & 0x80) != 0);
                    break;
                }
                case MN_MULD: case MN_DIVD: case MN_DIVQ: case MN_MULDU: case MN_MULQ: case MN_MULQU: case MN_MULO: case MN_MULOU:
                case MN_DIVDU: case MN_DIVQU: case MN_DIVO: case MN_DIVOU: {
                    int rc = do_muldiv (x, row);

                    if (rc)
                        return rc;
                    break;
                }
                case MN_LDDP: case MN_LDDS: case MN_STDP: case MN_STDS: {
                    /* LDDP/LDDS load DP (8 bits) / DS (16 bits), STDP/STDS store them ; flags as LD and ST: N Z from the datum, V cleared */
                    int is_ds = (row->mn == MN_LDDS || row->mn == MN_STDS);
                    int bytes = is_ds ? 2 : 1;
                    t_uint64 val;

                    if (row->mn == MN_LDDP || row->mn == MN_LDDS) {
                        switch (row->mode) {
                            case M63_ADDR_IMM8:  val = fn (x, 1); break;
                            case M63_ADDR_IMM16: val = fn (x, 2); break;
                            default: {
                                uint32 ea = get_ea (x, row, bytes);

                                if (x->stop) return x->stop;
                                val = rdn (x, ea, bytes);
                            }
                        }
                        if (x->stop) return x->stop;
                        if (is_ds) c->ds = (uint16) val; else c->dp = (uint8) val;
                        alu_logic (x, val, bytes);
                    } else {
                        uint32 ea = get_ea (x, row, bytes);

                        if (x->stop) return x->stop;
                        val = is_ds ? c->ds : c->dp;
                        alu_logic (x, val, bytes);
                        wrn (x, ea, bytes, val);
                    }
                    break;
                }
                case MN_ENI: c->cc &= (uint8) ~(CC_I | CC_F); c->imask = 0; break;
                case MN_DII: c->cc |= CC_I | CC_F; c->imask = 0xFFFFFF; break;
                case MN_SEXV: {
                    t_uint64 vv = c->v;

                    setreg (x, R_Q, (vv & 0x80000000ULL) ? 0xFFFFFFFFULL : 0);
                    set_nz (x, vv, 4);
                    set_flag (x, CC_V, 0);
                    break;
                }
                case MN_LOCK: case MN_UNLOCK: break;    /* bus arbitration between cores: nothing to do with one core */
                case MN_HCF: return STOP_HALT;
                case MN_RTS: x->pc = (uint32) pulln (x, &c->s, x->ptrw) & x->amask; break;
                case MN_DAA: do_daa (x); break;
                case MN_SEXW: {
                    int neg = (getreg (x, R_W) & 0x8000) != 0;

                    setreg (x, R_D, neg ? 0xFFFF : 0);
                    set_nz (x, getreg (x, R_Q), 4);
                    set_flag (x, CC_V, 0);
                    break;
                }
                case MN_PSHS: case MN_PSHU: case MN_PULS: case MN_PULU: {
                    uint32 mask = (uint32) fn (x, x->wide ? 2 : 1);

                    if (row->mn == MN_PSHS || row->mn == MN_PSHU)
                        do_push (x, row->mn == MN_PSHU, mask);
                    else
                        do_pull (x, row->mn == MN_PULU, mask);
                    break;
                }
                case MN_PSHSW: do_push (x, 0, 0x300); break;
                case MN_PSHUW: do_push (x, 1, 0x300); break;
                case MN_PULSW: do_pull (x, 0, 0x300); break;
                case MN_PULUW: do_pull (x, 1, 0x300); break;
                case MN_TFM_II: case MN_TFM_DD: case MN_TFM_ID: case MN_TFM_DI: do_tfm (x, row->mn); break;
                case MN_BAND: case MN_BIAND: case MN_BOR: case MN_BIOR: case MN_BEOR: case MN_BIEOR: case MN_LDBT: case MN_STBT:
                    do_bittr (x, row->mn);
                    break;
                case MN_OIM: case MN_AIM: case MN_EIM: case MN_TIM: {
                    uint8 imm = f8 (x), v;
                    uint32 ea = get_ea (x, row, 1);

                    if (x->stop) return x->stop;
                    v = rd8 (x, ea);
                    v = row->mn == MN_OIM ? (v | imm) : row->mn == MN_AIM ? (v & imm) : row->mn == MN_EIM ? (v ^ imm) : (v & imm);
                    set_nz (x, v, 1);
                    set_flag (x, CC_V, 0);
                    if (row->mn != MN_TIM)
                        wr8 (x, ea, v);
                    break;
                }
                default:
                    if (row->page == 0x18) {
                        int fr = do_fpu (x, row);

                        if (fr)
                            return fr;
                        break;
                    }
                    return UNIMPL ();
            }
            break;
        default:
            return UNIMPL ();
    }
    if (x->stop)
        return x->stop;
    c->pc = x->upper | (x->pc & x->amask);
    return 0;
}

/* Executes one instruction of a core (or takes an interrupt).  Returns 0 and the cycles used, or a STOP_* code. */
int m63f09_step (m63f09_core_t *c, int *cycles)
{
    int r, took, stop;

    took = service_interrupts (c, cycles, &stop);
    if (took == 1)
        return stop;
    if (took == 2)
        return 0;
    r = step_inner (c, cycles);
    if (r == STOP_TRAP_INT) {
        ctx_t xs, *x = &xs;

        init_ctx (x, c);
        x->pc = trap_pc;
        c->md = (uint8) ((c->md & ~(MD_IE | MD_DZ)) | (trap_cause == 1 ? MD_IE : MD_DZ));
        if (trap_cause == 2)                            /* ALU_DIV_BY_ZERO: flags of CLR (N V C cleared, Z set), stacked in the frame */
            c->cc = (uint8) ((c->cc & ~(CC_N | CC_V | CC_C)) | CC_Z);
        do_exception (x, V_TRAP, 1);
        *cycles = 21;
        if (x->stop)
            return x->stop;
        c->pc = x->upper | (x->pc & x->amask);
        return 0;
    }
    return r;
}
