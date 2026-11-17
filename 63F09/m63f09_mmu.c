/* m63f09_mmu.c: MMU 63F29 of the 63F09 SoC (one per core)

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

   Functional model of ~/vhdl/63F09/63F09.srcs/sources_1/imports/rtl/63F29.vhd (the comments of the architecture are the specification).
   Four rings of 64 entries (tag = logical page bits 31-12, payload: 27 always mapped, 26 execute, 25 write, 24 read, 23-0 physical page),
   registers at $FFFD0000 + $1000 x core: +0 RING (bit 7 enables the MMU, bits 1-0 the ring wanted), +1 FUSE, +2 STATUS, +3 MAXRING,
   +4..+7 TAG_STAGING, +8 number of entries, $100.. the tags as a flat 5-bytes-per-2-entries stream, $600.. the ages (LRU), $800.. the payloads
   (the last byte of an entry commits it, in the entry the hardware chooses: the one holding the same tag, else a free one, else the oldest
   that is not always mapped).

   FUSE.  It counts bus cycles of the core, not instructions: the ring that translates the accesses (CURRENT_RING) follows RING(1:0)
   only while FUSE is 0, and the interrupt lines are masked while it is not.  The write of FUSE (ring 0 only) happens at the end of its cycle
   and wins over the decrement of that same cycle: with FUSE = k written in cycle Tw, FUSE is k at the start of cycle Tw + 1 and 0 at the start
   of cycle Tw + k + 1, and the ring changes for the accesses of that cycle and after.  The simulator counts the cycles of each instruction with
   the cycles of its table; the first byte fetched is the cycle 1 of the instruction, a data access is placed in the last cycle.  Exact at the
   boundary of an instruction (JMP, as in regress/asm/mmuU1.asm: 2+7+6 cycles after the write of FUSE) and for RTI (opcode in the old ring,
   the frame in the new one), approximate for a switch inside a long instruction.
   A vector fetch (SWI, TRAP, interrupt, page fault) enters ring 0 (RING(1:0) and CURRENT_RING forced to 0) and is looked up in ring 0 whatever
   the ring was.

   Not modelled: the micro-TLB (only a timing aid), the registers of the other cores seen through a halted core (the bus of a single core
   is simulated), the wait states of the register reads.
*/

#include <string.h>
#include "m63f09_defs.h"

#define NENT        64
#define NRING       4
#define MMU_BASE    0xFFFFD0000ULL
#define MMU_STRIDE  0x1000ULL

typedef struct {
    uint8    ring, status, maxring;
    uint32   tag_stg, pay_stg;
    int      ring_stg;
    uint32   bank[NRING][NENT];                         /* 20-bit tags */
    uint32   pay[NRING][NENT];
    uint8    lru[NRING][NENT];
    int      cur;                                       /* CURRENT_RING */
    t_uint64 cyc;                                       /* cycles of the core before the current instruction */
    uint32   tot;                                       /* cycles of the current instruction (0 until decoded) */
    t_uint64 sw;                                        /* first cycle with FUSE = 0 */
    int      pf_count, pf_nest;                         /* faults being delivered, handlers still running */
} mmu_t;

static mmu_t mm[M63_MAXCORES];

extern DEVICE mmu_dev;
extern UNIT mmu_unit;

static mmu_t *M (const m63f09_core_t *c)
{
    return &mm[c - m63f09_cores];
}

static int fuse_val (const mmu_t *m, t_uint64 t)
{
    return m->sw > t ? (int) (m->sw - t > 255 ? 255 : m->sw - t) : 0;
}

/* CURRENT_RING follows RING(1:0) in every standard cycle with FUSE = 0 */
static void latch (mmu_t *m, t_uint64 t)
{
    if (m->sw <= t)
        m->cur = m->ring & 3;
}

static int is_free (uint32 p)
{
    return !(p & (1u << 27)) && !(p & (3u << 24));
}

int m63f09_mmu_active (const m63f09_core_t *c)
{
    const mmu_t *m = M (c);

    return m->ring != 0 || m->cur != 0 || m->sw > m->cyc;
}

int m63f09_mmu_fuse_active (const m63f09_core_t *c)
{
    const mmu_t *m = M (c);

    return fuse_val (m, m->cyc + 1) != 0;
}

void m63f09_mmu_set_total (const m63f09_core_t *c, uint32 total)
{
    M (c)->tot = total;
}

void m63f09_mmu_advance (const m63f09_core_t *c, int cycles)
{
    mmu_t *m = M (c);

    m->cyc += (t_uint64) (cycles > 0 ? cycles : 0);
    m->tot = 0;
}

/* Page faults being delivered / handlers running (the triple-fault detector of 63F09.vhd) */
int m63f09_mmu_pf_level (const m63f09_core_t *c)
{
    const mmu_t *m = M (c);

    return m->pf_count + m->pf_nest;
}

void m63f09_mmu_pf_begin (const m63f09_core_t *c)
{
    M (c)->pf_count++;
}

void m63f09_mmu_pf_done (const m63f09_core_t *c)
{
    mmu_t *m = M (c);

    m->pf_count = 0;
    if (m->pf_nest < 3)
        m->pf_nest++;
}

void m63f09_mmu_pf_rti (const m63f09_core_t *c)
{
    mmu_t *m = M (c);

    if (m->pf_nest > 0)
        m->pf_nest--;
}

/* Translation: returns 0 and the physical address, or 1 for a page fault (STATUS updated for a standard access).  kind: MMU_*.
   pos: the cycle of the instruction in which the access happens (1 = the first). */
int m63f09_mmu_xlate (const m63f09_core_t *c, uint32 la, int kind, uint32 pos, t_uint64 *pa)
{
    mmu_t *m = M (c);
    t_uint64 t = m->cyc + pos;
    int id = (int) (c - m63f09_cores), i, hits = 0, idx = -1, code;
    uint32 tag = la >> 12, p;

    if (kind == MMU_VECTOR) {
        if (!(m->ring & 0x80)) {
            *pa = PA_PREFIX | la;
            return 0;
        }
        m->ring &= (uint8) ~3;                          /* VEC_ENTRY: ring 0 */
        m->cur = 0;
        for (i = 0; i < NENT; i++)
            if (m->bank[0][i] == tag) {
                hits++;
                if (idx < 0)
                    idx = i;
            }
        if (hits != 1)
            return 1;                                   /* not mapped (or several entries): PAGEFAULT_n, STATUS untouched */
        p = m->pay[0][idx];
        *pa = (p & (1u << 24)) ? (((t_uint64) (p & 0xFFFFFF) << 12) | (la & 0xFFF)) : 0xFFFFFFFFFULL;   /* not readable: poison address */
        return 0;
    }
    latch (m, t);
    if (!(m->ring & 0x80) || (la & 0xFFFFF000u) == (uint32) (0xFFFD0000u + (uint32) id * 0x1000u)) {
        *pa = PA_PREFIX | la;                           /* MMU off, or the registers of its own MMU: not translated */
        return 0;
    }
    for (i = 0; i < NENT; i++)
        if (m->bank[m->cur][i] == tag && (m->pay[m->cur][i] & (3u << 24))) {
            hits++;
            if (idx < 0)
                idx = i;
        }
    if (hits == 0)
        code = 1;
    else if (hits > 1) {
        m->status |= 0x80;
        code = 0;
    } else {
        p = m->pay[m->cur][idx];
        if (kind == MMU_FETCH_OP)
            code = ((p & (1u << 26)) && (p & (1u << 24))) ? -1 : 2;
        else if (kind == MMU_WRITE)
            code = (p & (1u << 25)) ? -1 : 4;
        else
            code = (p & (1u << 24)) ? -1 : 8;
        if (code < 0) {
            if (!(p & (1u << 27))) {                    /* ageing: the entry used goes to 0, the others of the ring age */
                int k;

                for (k = 0; k < NENT; k++)
                    if (k == idx)
                        m->lru[m->cur][k] = 0;
                    else if (m->lru[m->cur][k] != 0xFF)
                        m->lru[m->cur][k]++;
            }
            *pa = ((t_uint64) (p & 0xFFFFFF) << 12) | (la & 0xFFF);
            return 0;
        }
    }
    m->status = (uint8) ((m->status & 0xC0) | (m->cur << 4) | code);
    return 1;
}

/* Registers -------------------------------------------------------------------------------------------------------------------------- */

static int tag_byte (const mmu_t *m, int ring, int ent, int ph, uint8 *v)
{
    /* two entries A and B = A + 1 in 5 bytes: A(19:12) A(11:4) A(3:0)B(19:16) B(15:8) B(7:0) -- see the flat stream of 63F29.vhd */
    uint32 a = m->bank[ring][ent], b = m->bank[ring][ent + 1];

    switch (ph) {
        case 0: *v = (uint8) (a >> 12); break;
        case 1: *v = (uint8) (a >> 4); break;
        case 2: *v = (uint8) (((a & 0xF) << 4) | ((b >> 16) & 0xF)); break;
        case 3: *v = (uint8) (b >> 8); break;
        default: *v = (uint8) b; break;
    }
    return 0;
}

static int mmu_rd (void *ctx, uint32 off, uint8 *val)
{
    mmu_t *m = (mmu_t *) ctx;
    t_uint64 t = m->cyc + (m->tot ? m->tot : 1);

    *val = 0;
    latch (m, t);
    if (off == 0) *val = m->ring;
    else if (off == 1) *val = (uint8) fuse_val (m, t);
    else if (off == 2) *val = m->status;
    else if (off == 3) *val = m->maxring;
    else if (off >= 4 && off < 8) *val = (uint8) (m->tag_stg >> (24 - 8 * (off - 4)));
    else if (off == 8) *val = NENT;
    else if ((off >> 9) == 3) {                         /* $600-$7FF: ages, A(8:7) ring, A(6:0) entry */
        if ((off & 0x7F) < NENT)
            *val = m->lru[(off >> 7) & 3][off & 0x7F];
    } else if (off >= 0x800) {                          /* payloads */
        int e = (off >> 2) & 0x7F;

        if (e < NENT)
            *val = (uint8) (m->pay[(off >> 9) & 3][e] >> (24 - 8 * (off & 3)));
    } else if (off >= 256 && off < 256 + 10 * NENT) {   /* flat stream of tags */
        uint32 f = off - 256, pair = f / 5;

        tag_byte (m, (int) (pair / (NENT / 2)), (int) ((pair % (NENT / 2)) * 2), (int) (f % 5), val);
    }
    return 0;
}

static int victim (const mmu_t *m, int ring, uint32 tag)
{
    int i, best = -1, free_i = -1, dup = -1;

    for (i = 0; i < NENT; i++) {
        uint32 p = m->pay[ring][i];

        if (m->bank[ring][i] == tag && !is_free (p) && dup < 0)
            dup = i;
        if (is_free (p) && free_i < 0)
            free_i = i;
        if (!(p & (1u << 27)) && (best < 0 || m->lru[ring][i] > m->lru[ring][best]))
            best = i;                                   /* the oldest that is not always mapped, the lowest index on a tie */
    }
    return dup >= 0 ? dup : free_i >= 0 ? free_i : best;
}

static int mmu_wr (void *ctx, uint32 off, uint8 val)
{
    mmu_t *m = (mmu_t *) ctx;
    t_uint64 t = m->cyc + (m->tot ? m->tot : 1);
    int rc;

    latch (m, t);
    rc = m->cur;
    if (off == 0) { if (rc == 0) m->ring = val; }
    else if (off == 1) { if (rc == 0) m->sw = t + val + 1; }
    else if (off == 2) { if (rc == 0) m->status = val; }
    else if (off == 3) { if (rc == 0) m->maxring = val; }
    else if (off >= 4 && off < 8) {
        if (rc <= (m->maxring & 3)) {
            int sh = 24 - 8 * (int) (off - 4);

            m->tag_stg = (m->tag_stg & ~(0xFFu << sh)) | ((uint32) val << sh);
        }
    } else if (off >= 0x800) {                          /* INDIRECTIONS: bytes 0-2 stage, byte 3 commits */
        int ring = (off >> 9) & 3, sh = 24 - 8 * (int) (off & 3);

        if (rc <= (m->maxring & 3)) {
            int ok = 0, v = -1;

            if ((off & 3) == 3) {
                v = victim (m, m->ring_stg, m->tag_stg >> 12);
                ok = v >= 0 && m->ring_stg == ring;
                if (ok) {
                    int k;

                    for (k = 0; k < NENT; k++)
                        if (k != v && m->lru[ring][k] != 0xFF)
                            m->lru[ring][k]++;
                    m->bank[ring][v] = m->tag_stg >> 12;
                    m->pay[ring][v] = (m->pay_stg & 0xFFFFFF00u) | val;
                    m->lru[ring][v] = 0;
                    m->status &= (uint8) ~0x40;
                } else
                    m->status = (uint8) ((m->status | 0x40 ) & ~0x30) | (uint8) (ring << 4);
            }
            m->ring_stg = ring;
            if ((off & 3) != 3)
                m->pay_stg = (m->pay_stg & ~(0xFFu << sh)) | ((uint32) val << sh);
            else
                m->pay_stg = (m->pay_stg & 0xFFFFFF00u) | val;
        }
    } else if ((off >> 9) == 3) {
        if (rc <= (m->maxring & 3) && (off & 0x7F) < NENT)
            m->lru[(off >> 7) & 3][off & 0x7F] = val;
    } else if (off >= 256 && off < 256 + 10 * NENT) {
        if (rc <= (m->maxring & 3)) {
            uint32 f = off - 256, pair = f / 5;
            int ring = (int) (pair / (NENT / 2)), a = (int) ((pair % (NENT / 2)) * 2), ph = (int) (f % 5);
            uint32 *ta = &m->bank[ring][a], *tb = &m->bank[ring][a + 1];

            switch (ph) {
                case 0: *ta = (*ta & 0x00FFFu) | ((uint32) val << 12); break;
                case 1: *ta = (*ta & 0xFF00Fu) | ((uint32) val << 4); break;
                case 2: *ta = (*ta & 0xFFFF0u) | (val >> 4); *tb = (*tb & 0x0FFFFu) | ((uint32) (val & 0xF) << 16); break;
                case 3: *tb = (*tb & 0xF00FFu) | ((uint32) val << 8); break;
                default: *tb = (*tb & 0xFFF00u) | val; break;
            }
        }
    }
    return 0;
}

static t_stat mmu_reset (DEVICE *dptr)
{
    int k;

    memset (mm, 0, sizeof (mm));
    sim_cancel (&mmu_unit);
    for (k = 0; k < M63_MAXCORES; k++)
        m63f09_io_register (MMU_BASE + (t_uint64) k * MMU_STRIDE, 0x1000, mmu_rd, mmu_wr, &mm[k]);
    return SCPE_OK;
}

UNIT mmu_unit = { UDATA (NULL, 0, 0) };

REG mmu_reg[] = {
    { HRDATAD (RING0, mm[0].ring, 8, "core 0: RING register (bit 7 MMU enabled, bits 1-0 ring asked)") },
    { HRDATAD (STATUS0, mm[0].status, 8, "core 0: STATUS") },
    { HRDATAD (MAXRING0, mm[0].maxring, 8, "core 0: MAXRING") },
    { DRDATAD (CURRENT0, mm[0].cur, 2, "core 0: ring that translates now (CURRENT_RING)") },
    { DRDATAD (FUSEEND0, mm[0].sw, 64, "core 0: first cycle with FUSE = 0") },
    { DRDATAD (CYCLES0, mm[0].cyc, 64, "core 0: cycles executed") },
    { DRDATAD (PFNEST0, mm[0].pf_nest, 3, "core 0: page fault handlers running") },
    { NULL }
};

DEVICE mmu_dev = {
    "MMU", &mmu_unit, mmu_reg, NULL, 1, 16, 4, 1, 16, 8,
    NULL, NULL, &mmu_reset, NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
