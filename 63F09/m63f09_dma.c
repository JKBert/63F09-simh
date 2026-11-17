/* m63f09_dma.c: DMA 63F44 of the 63F09 SoC

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

   Functional model of ~/vhdl/63F09/63F09.srcs/sources_1/imports/rtl/63F44.vhd (written 2026-11-17, simulated by its testbench
   regress/proto/dma/tb_dma.vhd) and of the chapter "Le DMA 63F44" of the manual.  The registers, the validation against the two
   windows, the causes of error, the modes of address (increment, decrement, fixed), the blocks of 512 bytes served round robin, the
   chaining by shadow registers, ABORT, RESET and the acknowledgement by reading STATE are those of the VHDL.

   Pages (provisional, not decoded by SoC.vhd yet): control $FFFE8000, channel c at $FFFE7000 - c x $1000, 4 I/O channels.
   Registers of the control page: +0 CTL (bit 0 ENABLE, bit 1 ABORT, bit 2 RESET), +1 STATUS, +2/+3 EPOCH, +4 INT_STATUS, +5 CHANNELS,
   +8 MEM_BASE, +$10 MEM_LIMIT, +$18 DEV_BASE, +$20 DEV_LIMIT, +$30 CTX_BASE, +$38 CTX_LIMIT (8 bytes each, big endian, 36 bits).
   Registers of a channel page: +0 SRC (8 bytes), +8 DST (8), +$10 COUNT (4), +$14 CONTROL (bit 0 GO, 1 MODE, 3-2 source mode,
   5-4 destination mode, 6 CHAIN, 7 IE), +$15 STATE (bit 0 BUSY, 1 DEND, 2 ERR, 6-4 cause), +$20 NEXT_SRC, +$28 NEXT_DST, +$30 NEXT_COUNT.
   Causes: 1 source outside the windows, 2 destination outside the windows, 3 count 0 / overflow / reserved mode, 4 chained transfer
   invalid, 5 bus error, 6 aborted, 7 not enabled.

   Differences with the RTL, on purpose: a transfer is done a byte at a time (the RTL moves 64-bit words with a byte mask: the result is
   the same except for an increment/increment copy whose two ranges overlap closer than 8 bytes, which the RTL leaves undefined) ; no
   cache: the range messages INV_* have no one to go to ; the time a block takes is a plain estimate, not measured.  The DMA reaches the
   external memory only (the 64-bit bus): the ROM, the internal RAM and the peripherals answer with a bus error (cause 5).
   The interrupt output is not wired in SoC.vhd yet: DEPOSIT DMA IRQLINE n connects it to the line EIRQn of core 0 (255 = none).
*/

#include <string.h>
#include "m63f09_defs.h"

extern DEVICE dma_dev;
extern UNIT dma_unit;

#define DMA_NCH         4
#define DMA_BLOCK       512
#define DMA_CTRL_BASE   0xFFFFE8000ULL
#define DMA_CH_BASE(c)  (0xFFFFE7000ULL - (t_uint64) (c) * 0x1000ULL)
#define DMA_MAXPA       (1ULL << 36)

#define CTL_MODE        0x02
#define CTL_CHAIN       0x40
#define CTL_IE          0x80

#define CAUSE_SRC       1
#define CAUSE_DST       2
#define CAUSE_COUNT     3
#define CAUSE_NEXT      4
#define CAUSE_BUS       5
#define CAUSE_ABORT     6
#define CAUSE_DISABLED  7

typedef struct {
    t_uint64 src, dst, nsrc, ndst;                      /* 36 bits */
    uint32 cnt, ncnt;
    uint8  ctl;                                         /* the GO bit is not stored */
    uint8  busy, run, dend, err, cause;
} dma_ch_t;

static struct {
    uint8    enable, abort_pend;
    uint16   epoch;
    t_uint64 mem_base, mem_limit, dev_base, dev_limit, ctx_base, ctx_limit;
    dma_ch_t ch[DMA_NCH];
    int      last;                                      /* channel served last (round robin) */
    uint8    ackread;                                   /* reading STATE clears DEND and ERR (ACK_ON_READ of the VHDL) */
    uint32   blocks;                                    /* statistics: blocks moved */
} dma;

static uint8 dma_irqline = 255;                         /* EIRQ line of the interrupt output, 255 = not connected (kept by a reset) */

/* Byte I (0 = most significant) of a 36-bit value held as 8 big endian bytes, and the same of a 32-bit value */
static uint8 get36 (t_uint64 v, int i)
{
    return (uint8) ((v >> (56 - 8 * i)) & 0xFF);
}

static t_uint64 put36 (t_uint64 v, int i, uint8 d)
{
    int b;

    for (b = 0; b < 8; b++) {
        int pos = 56 - 8 * i + b;

        if (pos >= 0 && pos <= 35) {
            v &= ~(1ULL << pos);
            v |= (t_uint64) ((d >> b) & 1) << pos;
        }
    }
    return v;
}

static uint8 get32 (uint32 v, int i)
{
    return (uint8) (v >> (24 - 8 * i));
}

static uint32 put32 (uint32 v, int i, uint8 d)
{
    v &= ~(0xFFu << (24 - 8 * i));
    return v | ((uint32) d << (24 - 8 * i));
}

static void dma_irq (void)
{
    int c, level = 0;

    for (c = 0; c < DMA_NCH; c++)
        if ((dma.ch[c].ctl & CTL_IE) && (dma.ch[c].dend || dma.ch[c].err))
            level = 1;
    if (dma_irqline < 24)
        m63f09_set_line (&m63f09_cores[0], M63_LINE_EIRQ + dma_irqline, level);
}

/* One byte on the 64-bit bus: only the external memory answers */
static int dma_read (t_uint64 pa, uint8 *v)
{
    if (pa >= PA_RAM_BASE)
        return 1;
    return m63f09_read8 ((t_addr) pa, v);
}

static int dma_write (t_uint64 pa, uint8 v)
{
    if (pa >= PA_RAM_BASE)
        return 1;
    return m63f09_write8 ((t_addr) pa, v);
}

/* Validation of the transfer of a channel: returns the cause (0 = valid) */
static int dma_validate (const dma_ch_t *c)
{
    int sm = (c->ctl >> 2) & 3, dm = (c->ctl >> 4) & 3;
    t_uint64 cm1, slo, shi, dlo, dhi;

    if (!dma.enable)
        return CAUSE_DISABLED;
    if (c->cnt == 0 || sm == 3 || dm == 3)
        return CAUSE_COUNT;
    cm1 = (t_uint64) c->cnt - 1;
    if (sm == 0) { slo = c->src; shi = c->src + cm1; if (shi >= DMA_MAXPA) return CAUSE_COUNT; }
    else if (sm == 1) { if (cm1 > c->src) return CAUSE_COUNT; slo = c->src - cm1; shi = c->src; }
    else slo = shi = c->src;
    if (dm == 0) { dlo = c->dst; dhi = c->dst + cm1; if (dhi >= DMA_MAXPA) return CAUSE_COUNT; }
    else if (dm == 1) { if (cm1 > c->dst) return CAUSE_COUNT; dlo = c->dst - cm1; dhi = c->dst; }
    else dlo = dhi = c->dst;
    if (!((slo >= dma.mem_base && shi < dma.mem_limit) || (slo >= dma.dev_base && shi < dma.dev_limit)))
        return CAUSE_SRC;
    if (!((dlo >= dma.mem_base && dhi < dma.mem_limit) || (dlo >= dma.dev_base && dhi < dma.dev_limit)))
        return CAUSE_DST;
    return 0;
}

static void dma_kick (void)
{
    int c, any = 0;

    for (c = 0; c < DMA_NCH; c++)
        if (dma.ch[c].run)
            any = 1;
    if (dma.abort_pend)
        any = 1;
    if (any && !sim_is_active (&dma_unit))
        sim_activate (&dma_unit, 16);
}

/* GO written: validate, then the channel runs or stops with the cause */
static void dma_go (dma_ch_t *c, int link)
{
    int cause = dma_validate (c);

    c->busy = 1;
    c->dend = 0;
    c->err = 0;
    c->cause = 0;
    if (cause) {
        c->busy = 0;
        c->run = 0;
        c->err = 1;
        c->cause = (uint8) ((link && cause != CAUSE_DISABLED) ? CAUSE_NEXT : cause);
    } else
        c->run = 1;
}

static void dma_finish (dma_ch_t *c, int cause)
{
    c->run = 0;
    c->busy = 0;
    if (cause == 0) {
        if ((c->ctl & CTL_CHAIN) && c->ncnt != 0) {
            c->src = c->nsrc;
            c->dst = c->ndst;
            c->cnt = c->ncnt;
            c->ncnt = 0;
            dma_go (c, 1);
        } else
            c->dend = 1;
    } else {
        c->err = 1;
        c->cause = (uint8) cause;
    }
}

/* Moves one block (512 bytes, or all the rest in burst mode) of the next running channel, round robin */
static t_stat dma_svc (UNIT *uptr)
{
    int k, c = -1, cause = 0;
    uint32 left;
    int sm, dm;
    dma_ch_t *ch;
    int32 cyc;

    if (dma.abort_pend) {
        for (k = 0; k < DMA_NCH; k++)
            if (dma.ch[k].busy) {
                dma.ch[k].busy = 0;
                dma.ch[k].run = 0;
                dma.ch[k].dend = 0;
                dma.ch[k].err = 1;
                dma.ch[k].cause = CAUSE_ABORT;
            }
        dma.abort_pend = 0;
        dma_irq ();
    }
    for (k = 1; k <= DMA_NCH; k++) {
        int i = (dma.last + k) % DMA_NCH;

        if (dma.ch[i].run) {
            c = i;
            break;
        }
    }
    if (c < 0)
        return SCPE_OK;
    dma.last = c;
    ch = &dma.ch[c];
    sm = (ch->ctl >> 2) & 3;
    dm = (ch->ctl >> 4) & 3;
    left = (ch->ctl & CTL_MODE) ? 0xFFFFFFFFu : DMA_BLOCK;
    cyc = 20;
    while (ch->cnt > 0 && left > 0 && !cause) {
        uint8 v;

        if (dma_read (ch->src, &v) || dma_write (ch->dst, v)) {
            cause = CAUSE_BUS;
            break;
        }
        if (sm == 0) ch->src++; else if (sm == 1) ch->src--;
        if (dm == 0) ch->dst++; else if (dm == 1) ch->dst--;
        ch->cnt--;
        left--;
        if ((++cyc & 7) == 0)
            cyc += 1;
    }
    dma.blocks++;
    if (cause)
        dma_finish (ch, cause);
    else if (ch->cnt == 0)
        dma_finish (ch, 0);
    dma_irq ();
    dma_kick ();
    sim_activate (uptr, cyc);
    return SCPE_OK;
}

static uint8 ch_status (const dma_ch_t *c)
{
    return (uint8) ((c->cause << 4) | (c->err << 2) | (c->dend << 1) | c->busy);
}

/* Control page */
static int dma_ctrl_rd (void *ctx, uint32 off, uint8 *val)
{
    int c;
    uint8 any = 0, ints = 0;

    (void) ctx;
    for (c = 0; c < DMA_NCH; c++) {
        if (dma.ch[c].busy) any = 1;
        if (dma.ch[c].dend || dma.ch[c].err) ints |= (uint8) (1 << c);
    }
    *val = 0;
    if (off == 0) *val = dma.enable;
    else if (off == 1) *val = any;
    else if (off == 2) *val = (uint8) (dma.epoch >> 8);
    else if (off == 3) *val = (uint8) dma.epoch;
    else if (off == 4) *val = ints;
    else if (off == 5) *val = DMA_NCH;
    else if (off >= 0x08 && off < 0x10) *val = get36 (dma.mem_base, (int) (off - 0x08));
    else if (off >= 0x10 && off < 0x18) *val = get36 (dma.mem_limit, (int) (off - 0x10));
    else if (off >= 0x18 && off < 0x20) *val = get36 (dma.dev_base, (int) (off - 0x18));
    else if (off >= 0x20 && off < 0x28) *val = get36 (dma.dev_limit, (int) (off - 0x20));
    else if (off >= 0x30 && off < 0x38) *val = get36 (dma.ctx_base, (int) (off - 0x30));
    else if (off >= 0x38 && off < 0x40) *val = get36 (dma.ctx_limit, (int) (off - 0x38));
    return 0;
}

static int dma_ctrl_wr (void *ctx, uint32 off, uint8 val)
{
    int c;

    (void) ctx;
    if (off == 0) {
        dma.enable = val & 1;
        if (val & 2) {
            dma.abort_pend = 1;
            dma_kick ();
        }
        if (val & 4) {
            for (c = 0; c < DMA_NCH; c++)
                memset (&dma.ch[c], 0, sizeof (dma.ch[c]));
            dma.abort_pend = 0;
            dma_irq ();
        }
    }
    else if (off == 2) dma.epoch = (uint16) ((dma.epoch & 0x00FF) | (val << 8));
    else if (off == 3) dma.epoch = (uint16) ((dma.epoch & 0xFF00) | val);
    else if (off >= 0x08 && off < 0x10) dma.mem_base = put36 (dma.mem_base, (int) (off - 0x08), val);
    else if (off >= 0x10 && off < 0x18) dma.mem_limit = put36 (dma.mem_limit, (int) (off - 0x10), val);
    else if (off >= 0x18 && off < 0x20) dma.dev_base = put36 (dma.dev_base, (int) (off - 0x18), val);
    else if (off >= 0x20 && off < 0x28) dma.dev_limit = put36 (dma.dev_limit, (int) (off - 0x20), val);
    else if (off >= 0x30 && off < 0x38) dma.ctx_base = put36 (dma.ctx_base, (int) (off - 0x30), val);
    else if (off >= 0x38 && off < 0x40) dma.ctx_limit = put36 (dma.ctx_limit, (int) (off - 0x38), val);
    return 0;
}

/* Channel page */
static int dma_ch_rd (void *ctx, uint32 off, uint8 *val)
{
    dma_ch_t *c = (dma_ch_t *) ctx;

    *val = 0;
    if (off < 8) *val = get36 (c->src, (int) off);
    else if (off >= 0x08 && off < 0x10) *val = get36 (c->dst, (int) (off - 0x08));
    else if (off >= 0x10 && off < 0x14) *val = get32 (c->cnt, (int) (off - 0x10));
    else if (off == 0x14) *val = (uint8) ((c->ctl & 0xFE) | c->busy);
    else if (off == 0x15) {
        *val = ch_status (c);
        if (dma.ackread && (c->dend || c->err)) {
            c->dend = 0;
            c->err = 0;
            dma_irq ();
        }
    }
    else if (off >= 0x20 && off < 0x28) *val = get36 (c->nsrc, (int) (off - 0x20));
    else if (off >= 0x28 && off < 0x30) *val = get36 (c->ndst, (int) (off - 0x28));
    else if (off >= 0x30 && off < 0x34) *val = get32 (c->ncnt, (int) (off - 0x30));
    return 0;
}

static int dma_ch_wr (void *ctx, uint32 off, uint8 val)
{
    dma_ch_t *c = (dma_ch_t *) ctx;

    if (off < 8) { if (!c->busy) c->src = put36 (c->src, (int) off, val); }
    else if (off >= 0x08 && off < 0x10) { if (!c->busy) c->dst = put36 (c->dst, (int) (off - 0x08), val); }
    else if (off >= 0x10 && off < 0x14) { if (!c->busy) c->cnt = put32 (c->cnt, (int) (off - 0x10), val); }
    else if (off == 0x14) {
        if (!c->busy) {
            c->ctl = val & 0xFE;
            if (val & 1) {
                dma_go (c, 0);
                dma_kick ();
            }
        } else
            c->ctl = (uint8) ((c->ctl & ~CTL_IE) | (val & CTL_IE));
        dma_irq ();
    }
    else if (off == 0x15) {
        c->dend = 0;
        c->err = 0;
        dma_irq ();
    }
    else if (off >= 0x20 && off < 0x28) c->nsrc = put36 (c->nsrc, (int) (off - 0x20), val);
    else if (off >= 0x28 && off < 0x30) c->ndst = put36 (c->ndst, (int) (off - 0x28), val);
    else if (off >= 0x30 && off < 0x34) c->ncnt = put32 (c->ncnt, (int) (off - 0x30), val);
    return 0;
}

static t_stat dma_reset (DEVICE *dptr)
{
    int c;

    memset (&dma, 0, sizeof (dma));
    dma.ackread = 1;
    dma.last = DMA_NCH - 1;
    sim_cancel (&dptr->units[0]);
    m63f09_io_register (DMA_CTRL_BASE, 0x1000, dma_ctrl_rd, dma_ctrl_wr, NULL);
    for (c = 0; c < DMA_NCH; c++)
        m63f09_io_register (DMA_CH_BASE (c), 0x1000, dma_ch_rd, dma_ch_wr, &dma.ch[c]);
    return SCPE_OK;
}

UNIT dma_unit = { UDATA (&dma_svc, 0, 0) };

REG dma_reg[] = {
    { HRDATAD (ENABLE, dma.enable, 8, "DMA enabled") },
    { HRDATAD (EPOCH, dma.epoch, 16, "epoch word kept by the server") },
    { HRDATAD (IRQLINE, dma_irqline, 8, "EIRQ line of the interrupt output (255: not connected)") },
    { FLDATAD (ACKREAD, dma.ackread, 0, "reading STATE acknowledges") },
    { DRDATAD (BLOCKS, dma.blocks, 32, "blocks moved") },
    { HRDATAD (MEMBASE, dma.mem_base, 36, "memory window, first address") },
    { HRDATAD (MEMLIMIT, dma.mem_limit, 36, "memory window, first address after") },
    { HRDATAD (DEVBASE, dma.dev_base, 36, "device window, first address") },
    { HRDATAD (DEVLIMIT, dma.dev_limit, 36, "device window, first address after") },
    { HRDATAD (CH0CTL, dma.ch[0].ctl, 8, "channel 0 control") },
    { HRDATAD (CH0SRC, dma.ch[0].src, 36, "channel 0 source") },
    { HRDATAD (CH0DST, dma.ch[0].dst, 36, "channel 0 destination") },
    { HRDATAD (CH0CNT, dma.ch[0].cnt, 32, "channel 0 bytes left") },
    { FLDATAD (CH0BUSY, dma.ch[0].busy, 0, "channel 0 busy") },
    { NULL }
};

DEVICE dma_dev = {
    "DMA", &dma_unit, dma_reg, NULL, 1, 16, 4, 1, 16, 8,
    NULL, NULL, &dma_reset, NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
