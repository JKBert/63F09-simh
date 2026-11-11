/* m63f09_cpu.c: 63F09 CPU simulator (state, physical memory, SCP glue)

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

   M0: the state, the memory map, the reset by the vector, the symbolic display and the loaders exist; the instruction set
   (M1 and after) does not: sim_instr() fetches and decodes, then stops with "not implemented" on the first instruction.

   The state of a core is the registers of m63f09_core_t.  Registers:
       PC S U X Y V          32 bits (16 in a 16-bit state)
       A B E F               8 bits  (D = A:B, W = E:F, Q = D:W, O = Q:V are pseudo-registers)
       DP                    direct page, 8 bits;  DS: 16 bits (63F09 only)
       CC MD                 condition codes, mode register (bit 0 native, bit 4 32 bits, bit 5 FPU)
*/

#include <stdlib.h>
#include <string.h>
#include "m63f09_defs.h"

m63f09_core_t m63f09_cores[M63_MAXCORES];

static uint8  rom_mem[PA_ROM_SIZE];
static int    rom_writable;                             /* SET CPU ROMWR: test aid, the 16-bit states only see the ROM */
static uint8  ram_mem[PA_RAM_SIZE];
static uint8 *ext_mem = NULL;                           /* external memory, at 0xF00000000 (what a core sees with the MMU off) */
static t_uint64 ext_size = 0;

/* Architecture used by the disassembler: 0 = the one of the current MD, else an M63_OPCODE_ARCH_* bit */
static uint16 dis_arch = 0;

uint16 m63f09_arch_of_md (uint8 md)
{
    if (md & MD_32)
        return (md & MD_FPU) ? M63_OPCODE_ARCH_63F09_32BITS_FPU : M63_OPCODE_ARCH_63F09_32BITS;
    if (md & MD_NATIVE)
        return M63_OPCODE_ARCH_6309;
    return M63_OPCODE_ARCH_6809;
}

uint16 m63f09_dis_arch (void)
{
    return dis_arch ? dis_arch : m63f09_arch_of_md (m63f09_cores[0].md);
}

/* Physical memory ----------------------------------------------------------------------------------------------- */

t_uint64 m63f09_cpu_hz = 100000000ULL;

#define M63_MAXIO   16
static struct { t_addr base; uint32 size; m63f09_io_rd_t rd; m63f09_io_wr_t wr; void *ctx; } io_tab[M63_MAXIO];
static int io_n = 0;

void m63f09_io_register (t_addr base, uint32 size, m63f09_io_rd_t rd, m63f09_io_wr_t wr, void *ctx)
{
    int i;

    for (i = 0; i < io_n; i++)
        if (io_tab[i].base == base) {
            io_tab[i].size = size; io_tab[i].rd = rd; io_tab[i].wr = wr; io_tab[i].ctx = ctx;
            return;
        }
    if (io_n < M63_MAXIO) {
        io_tab[io_n].base = base; io_tab[io_n].size = size; io_tab[io_n].rd = rd; io_tab[io_n].wr = wr; io_tab[io_n].ctx = ctx;
        io_n++;
    }
}

static int io_find (t_addr pa)
{
    int i;

    for (i = 0; i < io_n; i++)
        if (pa >= io_tab[i].base && pa < io_tab[i].base + io_tab[i].size)
            return i;
    return -1;
}

static uint8 *map_byte (t_addr pa, int for_write_dbg, int for_write)
{
    pa &= PA_MASK;
    if (pa >= PA_ROM_BASE)
        return (for_write && !for_write_dbg && !rom_writable) ? NULL : &rom_mem[pa - PA_ROM_BASE];
    if (pa >= PA_RAM_BASE && pa < PA_RAM_BASE + PA_RAM_SIZE)
        return &ram_mem[pa - PA_RAM_BASE];
    if (ext_mem && pa >= PA_PREFIX && pa - PA_PREFIX < ext_size)
        return &ext_mem[pa - PA_PREFIX];
    return NULL;                                        /* devices and MMU registers: M1 and after */
}

int m63f09_read8 (t_addr pa, uint8 *val)
{
    uint8 *p = map_byte (pa, 0, 0);
    int k;

    if (p) {
        *val = *p;
        return 0;
    }
    k = io_find (pa & PA_MASK);
    if (k >= 0 && io_tab[k].rd)
        return io_tab[k].rd (io_tab[k].ctx, (uint32) ((pa & PA_MASK) - io_tab[k].base), val);
    return 1;
}

int m63f09_write8 (t_addr pa, uint8 val)
{
    uint8 *p = map_byte (pa, 0, 1);
    int k;

    if (p) {
        *p = val;
        return 0;
    }
    k = io_find (pa & PA_MASK);
    if (k >= 0 && io_tab[k].wr)
        return io_tab[k].wr (io_tab[k].ctx, (uint32) ((pa & PA_MASK) - io_tab[k].base), val);
    /* a write to the ROM is dropped by the hardware: it is not an error (map_byte refuses it for a normal write) */
    if ((pa & PA_MASK) >= PA_ROM_BASE)
        return 0;
    return 1;
}

int m63f09_write8_dbg (t_addr pa, uint8 val)
{
    uint8 *p = map_byte (pa, 1, 1);

    if (!p)
        return 1;
    *p = val;
    return 0;
}

/* Logical address of a core seen as a physical one: with the MMU off the physical address is 0xF followed by the 32-bit
   address; in a 16-bit state the address is forced into the 0xFFFF segment (63F09.vhd). */
static t_addr fetch_pa (const m63f09_core_t *c, uint32 pc)
{
    return PA_PREFIX | ((c->md & MD_32) ? pc : (0xFFFF0000u | (pc & 0xFFFF)));
}

/* SCP interface -------------------------------------------------------------------------------------------------- */

static t_stat cpu_ex (t_value *vptr, t_addr addr, UNIT *uptr, int32 sw)
{
    uint8 v;

    if (addr > PA_MASK || m63f09_read8 (addr, &v))
        return SCPE_NXM;
    if (vptr)
        *vptr = v;
    return SCPE_OK;
}

static t_stat cpu_dep (t_value val, t_addr addr, UNIT *uptr, int32 sw)
{
    if (addr > PA_MASK || m63f09_write8_dbg (addr, (uint8) val))
        return SCPE_NXM;
    return SCPE_OK;
}

static t_stat cpu_reset (DEVICE *dptr)
{
    int k;
    uint32 vec;

    for (k = 0; k < M63_MAXCORES; k++) {
        memset (&m63f09_cores[k], 0, sizeof (m63f09_core_t));
        m63f09_cores[k].cc = CC_I | CC_F;               /* MD = 0: the 6809 state until LDMD */
    }
    /* Reset vector: 16 bits at 0xFFFFFFFE, the core puts 0xFFFF in front.  The loaders fill the ROM before this is called. */
    vec = ((uint32) rom_mem[PA_RESET_VEC - (PA_ROM_BASE & 0xFFFFFFFFULL)] << 8) | rom_mem[PA_RESET_VEC + 1 - (PA_ROM_BASE & 0xFFFFFFFFULL)];
    m63f09_cores[0].pc = 0xFFFF0000u | vec;
    sim_brk_types = sim_brk_dflt = SWMASK ('E');
    return SCPE_OK;
}

static t_stat cpu_boot (int32 unitno, DEVICE *dptr)
{
    return cpu_reset (dptr);                            /* "boot cpu" = reset, the PC comes from the reset vector */
}

static t_stat cpu_set_romwr (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    rom_writable = val;
    return SCPE_OK;
}

static t_stat cpu_show_romwr (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "ROM %s", rom_writable ? "writable (test aid)" : "read only");
    return SCPE_OK;
}

static t_stat cpu_set_extmem (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    t_stat r;
    t_value mb;
    uint8 *p;

    if (!cptr)
        return SCPE_ARG;
    mb = get_uint (cptr, 10, 4095, &r);
    if (r != SCPE_OK)
        return r;
    p = (uint8 *) calloc ((size_t) (mb ? mb : 1) << 20, 1);
    if (!p)
        return SCPE_MEM;
    free (ext_mem);
    ext_mem = mb ? p : (free (p), NULL);
    ext_size = mb << 20;
    return SCPE_OK;
}

static t_stat cpu_show_extmem (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "external memory %llu MB at 0x%09llX", (unsigned long long) (ext_size >> 20), (unsigned long long) PA_PREFIX);
    return SCPE_OK;
}

static t_stat cpu_set_freq (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    t_stat r;
    t_value mhz;

    if (!cptr)
        return SCPE_ARG;
    mhz = get_uint (cptr, 10, 1000, &r);
    if (r != SCPE_OK || mhz == 0)
        return SCPE_ARG;
    m63f09_cpu_hz = (t_uint64) mhz * 1000000ULL;
    return SCPE_OK;
}

static t_stat cpu_show_freq (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "core clock %llu MHz (SYSCLK of the peripherals 24 MHz)", (unsigned long long) (m63f09_cpu_hz / 1000000ULL));
    return SCPE_OK;
}

static const struct { const char *name; uint16 arch; } dis_choices[] = {
    { "AUTO", 0 }, { "6809", M63_OPCODE_ARCH_6809 }, { "6309", M63_OPCODE_ARCH_6309 },
    { "63F09", M63_OPCODE_ARCH_63F09_32BITS }, { "63F09HF", M63_OPCODE_ARCH_63F09_32BITS_FPU }
};

static t_stat cpu_set_disasm (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    size_t i;

    if (!cptr)
        return SCPE_ARG;
    for (i = 0; i < sizeof (dis_choices) / sizeof (dis_choices[0]); i++)
        if (!strcasecmp (cptr, dis_choices[i].name)) {
            dis_arch = dis_choices[i].arch;
            return SCPE_OK;
        }
    return SCPE_ARG;
}

static t_stat cpu_show_disasm (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    size_t i;

    for (i = 0; i < sizeof (dis_choices) / sizeof (dis_choices[0]); i++)
        if (dis_choices[i].arch == dis_arch)
            fprintf (st, "disassembly as %s", dis_choices[i].name);
    return SCPE_OK;
}

UNIT cpu_unit = { UDATA (NULL, 0, 0) };

REG cpu_reg[] = {
    { HRDATAD (PC, m63f09_cores[0].pc, 32, "program counter") },
    { HRDATAD (S,  m63f09_cores[0].s,  32, "system stack pointer") },
    { HRDATAD (U,  m63f09_cores[0].u,  32, "user stack pointer") },
    { HRDATAD (X,  m63f09_cores[0].x,  32, "index register X") },
    { HRDATAD (Y,  m63f09_cores[0].y,  32, "index register Y") },
    { HRDATAD (V,  m63f09_cores[0].v,  32, "register V (32 bits in the 63F09 state)") },
    { HRDATAD (A,  m63f09_cores[0].a,  8,  "accumulator A") },
    { HRDATAD (B,  m63f09_cores[0].b,  8,  "accumulator B") },
    { HRDATAD (E,  m63f09_cores[0].e,  8,  "accumulator E") },
    { HRDATAD (F,  m63f09_cores[0].f,  8,  "accumulator F") },
    { HRDATAD (DP, m63f09_cores[0].dp, 8,  "direct page") },
    { HRDATAD (DS, m63f09_cores[0].ds, 16, "direct segment (63F09)") },
    { HRDATAD (CC, m63f09_cores[0].cc, 8,  "condition codes EFHINZVC") },
    { HRDATAD (MD, m63f09_cores[0].md, 8,  "mode: 01 native, 10 32 bits, 20 FPU") },
    { HRDATAD (IMASK, m63f09_cores[0].imask, 24, "interrupt mask word (MD bits 31-8)") },
    { HRDATAD (IRQLINES, m63f09_cores[0].irq_lines, 27, "asserted lines: bit 0 FIRQ, 1 IRQ, 2+i EIRQi") },
    { HRDATAD (NMIPEND, m63f09_cores[0].nmi_pend, 1, "NMI latched") },
    { HRDATAD (WAIT, m63f09_cores[0].wait, 2, "1 CWAI, 2 SYNC") },
    { HRDATAD (SLOADED, m63f09_cores[0].s_loaded, 1, "S written (LDMD then traps)") },
    { NULL }
};

MTAB cpu_mod[] = {
    { MTAB_XTD | MTAB_VDV | MTAB_NMO | MTAB_SHP, 0, "EXTMEM", "EXTMEM=n", &cpu_set_extmem, &cpu_show_extmem, NULL,
      "External memory size in MB (0xF00000000)" },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO | MTAB_SHP, 1, "ROMWR", "ROMWR", &cpu_set_romwr, &cpu_show_romwr, NULL,
      "Make the ROM writable (to test the 16-bit states, which only see the ROM)" },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO, 0, NULL, "NOROMWR", &cpu_set_romwr, NULL, NULL, "ROM read only (default)" },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO | MTAB_SHP, 0, "FREQ", "FREQ=n", &cpu_set_freq, &cpu_show_freq, NULL,
      "Core clock in MHz (default 100): the time base of the peripherals" },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO | MTAB_SHP, 0, "DISASM", "DISASM={AUTO|6809|6309|63F09|63F09HF}", &cpu_set_disasm,
      &cpu_show_disasm, NULL, "CPU state used by examine -m (AUTO: the one of MD)" },
    { 0 }
};

DEVICE cpu_dev = {
    "CPU", &cpu_unit, cpu_reg, cpu_mod,
    1, 16, PA_BITS, 1, 16, 8,
    &cpu_ex, &cpu_dep, &cpu_reset, &cpu_boot, NULL, NULL,
    NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* Instruction loop ----------------------------------------------------------------------------------------------- */

t_stat sim_instr (void)
{
    m63f09_core_t *c = &m63f09_cores[0];
    t_stat reason = SCPE_OK;

    while (reason == SCPE_OK) {
        int cycles = 1, r;

        if (sim_interval <= 0 && (reason = sim_process_event ()) != SCPE_OK)
            break;
        if (sim_brk_summ && sim_brk_test (c->pc, SWMASK ('E'))) {
            reason = STOP_IBKPT;
            break;
        }
        r = m63f09_step (c, &cycles);
        if (r) {
            if (r == STOP_UNIMPL) {
                uint8 buf[M63_MAXINSN];
                int n;

                for (n = 0; n < M63_MAXINSN; n++)
                    if (m63f09_read8 (fetch_pa (c, c->pc + n), &buf[n]))
                        break;
                { extern int m63f09_why; sim_printf ("Not implemented or illegal (m63f09_exec.c:%d) at PC=%08X: ", m63f09_why, c->pc); }
                m63f09_disasm (stdout, c->pc, buf, n, m63f09_arch_of_md (c->md), (c->md & MD_32) != 0);
                sim_printf ("\n");
            }
            reason = r;
            break;
        }
        if (c->wait && !m63f09_wake_pending (c)) {           /* CWAI/SYNC: time passes until a device raises a line */
            if (sim_clock_queue == QUEUE_LIST_END) {
                reason = STOP_WAIT;
                break;
            }
            cycles = sim_interval > 0 ? sim_interval : 1;
        }
        sim_interval -= cycles;
    }
    return reason;
}
