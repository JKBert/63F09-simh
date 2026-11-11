/* m63f09_defs.h: 63F09 SoC simulator definitions

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

   The core state and the physical memory map are kept free of any SIMH call so that the functional core can be reused
   by another host later (see CLAUDE.md, "Architecture proposée").  Sources of truth: the VHDL of the SoC (SoC.vhd, 63F09.vhd)
   and the opcode table of binutils-gdb (m63f09_optab.h is generated from it by tools/gen_optab.py).
*/

#ifndef M63F09_DEFS_H_
#define M63F09_DEFS_H_

#include "sim_defs.h"
#include "m63f09_optab.h"

#define M63_MAXCORES    8

/* Physical addresses are 36 bits.  With the MMU off, the physical address of a core is 0xF followed by its 32-bit address
   (63F09.vhd: A <= x"F" & CPU_A), which is why the internal devices sit at 0xFFFFxxxxx. */
#define PA_BITS         36
#define PA_MASK         0xFFFFFFFFFULL
#define PA_PREFIX       0xF00000000ULL                  /* physical address of logical address 0 with the MMU off */

#define PA_ROM_BASE     0xFFFFF0000ULL                  /* ROM 27F512, 64 KB; vectors at 0xFFFFFFC0 - 0xFFFFFFFF */
#define PA_ROM_SIZE     0x10000
#define PA_DEV_BASE     0xFFFFE0000ULL                  /* peripherals, 4 KB pages: PIA F000, ACIA E000, PTM D000,
                                                           SPI C000, TWI B000, BAC A000, WDG 9000 */
#define PA_MMU_BASE     0xFFFFD0000ULL                  /* MMU0..MMU7 registers, 4 KB each */
#define PA_RAM_BASE     0xFFFFC0000ULL                  /* internal RAM 61F512, 64 KB */
#define PA_RAM_SIZE     0x10000
#define PA_EXT_END      0xFFFFBFFFFULL                  /* external memory below this address */
#define PA_VECTORS      0xFFFFFFC0ULL                   /* low 32 bits of the interrupt vector table */
#define PA_RESET_VEC    0xFFFFFFFEULL                   /* reset vector: 16 bits, the CPU adds 0xFFFF in front */

/* MD register (LDMD): bit 0 native mode (6309), bit 4 32-bit mode (63F09), bit 5 FPU.  Reset: all zero (6809). */
#define MD_NATIVE       0x01
#define MD_32           0x10
#define MD_FPU          0x20
#define MD_FM           0x02                            /* FIRQ saves the whole state (like IRQ) */
#define MD_ED           0x04                            /* EIRQ on edge (0: on level) */
#define MD_IS           0x08                            /* MMU/S reset semantics (not modelled) */
#define MD_IE           0x40                            /* last TRAP: illegal instruction (IE=1, DZ=0) */
#define MD_DZ           0x80                            /* last TRAP: division by 0 (DZ=1, IE=0), both: pagefault */

/* Condition codes */
#define CC_C            0x01
#define CC_V            0x02
#define CC_Z            0x04
#define CC_N            0x08
#define CC_I            0x10
#define CC_H            0x20
#define CC_F            0x40
#define CC_E            0x80

typedef struct {
    uint32 pc, s, u, x, y, v;                           /* 16 bits significant unless MD_32; V 16 bits on 6309 */
    uint8  a, b, e, f;                                  /* D = A:B, W = E:F, Q = D:W, O = Q:V (pseudo-registers) */
    uint8  dp;
    uint16 ds;
    uint8  cc, md;
    uint32 imask;                                       /* MD bits 31-8: interrupt mask word (ENI clears it, DII sets it) */
    uint8  s_loaded;                                    /* S has been written: LDMD is then refused (TRAP) and NMI is armed */
    uint8  wait;                                        /* 0 running, WAIT_CWAI or WAIT_SYNC */
    uint8  nmi_pend;                                    /* NMI edge latched */
    uint32 irq_lines;                                   /* asserted lines: bit 0 FIRQ, bit 1 IRQ, bit 2+i EIRQi */
    uint32 eirq_edge;                                   /* EIRQ falling edges latched (MD_ED) */
    t_uint64 fp[16];                                    /* FPU stack, fp[0] is the top (FP0); a single lives in the low 32 bits */
    uint8  fpcc;                                        /* bits 7-5 rounding mode (0 RNE 1 RTZ 2 RDN 3 RUP 4 RMM 7 DYN), bits 4-0 NV DZ OF UF NX */
} m63f09_core_t;

extern m63f09_core_t m63f09_cores[M63_MAXCORES];

/* SCP stop codes (indexes of sim_stop_messages) */
#define STOP_HALT       1
#define STOP_IBKPT      2
#define STOP_UNIMPL     3
#define STOP_NXM        4
#define STOP_LOOP       5                               /* branch to self: the end of a test program */
#define STOP_WAIT       6                               /* CWAI/SYNC with no interrupt source able to wake the core */
#define WAIT_CWAI       1
#define WAIT_SYNC       2

/* Interrupt lines (m63f09_exec.c): 0 FIRQ, 1 IRQ, 2+i EIRQi (i = 0..23), 26 NMI.  level = 1 asserts the line. */
#define M63_LINE_FIRQ   0
#define M63_LINE_IRQ    1
#define M63_LINE_EIRQ   2
#define M63_LINE_NMI    26
void m63f09_set_line (m63f09_core_t *c, int line, int level);
/* 1 when the core is waiting (CWAI/SYNC) and an interrupt would wake it */
int m63f09_wake_pending (m63f09_core_t *c);

/* Physical memory (m63f09_cpu.c): return 0 when the address is mapped.  The _dbg write also reaches the ROM (loader, deposit). */
int m63f09_read8 (t_addr pa, uint8 *val);
int m63f09_write8 (t_addr pa, uint8 val);
int m63f09_write8_dbg (t_addr pa, uint8 val);

/* Executes one instruction of a core (m63f09_exec.c): returns 0 and the cycles used, or a STOP_* code (the PC then stays on
   the instruction that stopped). */
int m63f09_step (m63f09_core_t *c, int *cycles);

/* Peripherals on the physical address space (m63f09_cpu.c).  rd/wr return 0 when the register exists.  A device registers its region
   once (a second registration of the same base replaces the first). */
typedef int (*m63f09_io_rd_t) (void *ctx, uint32 off, uint8 *val);
typedef int (*m63f09_io_wr_t) (void *ctx, uint32 off, uint8 val);
void m63f09_io_register (t_addr base, uint32 size, m63f09_io_rd_t rd, m63f09_io_wr_t wr, void *ctx);

/* Core clock in Hz (SET CPU FREQ=n, default the 100 MHz target of the hardware): the cycles of sim_interval are cycles of this clock. */
extern t_uint64 m63f09_cpu_hz;
#define M63_SYSCLK_HZ   24000000ULL                     /* SYSCLK of the peripherals */

/* FPU arithmetic (m63f09_fpu.c) */
void m63f09_fpu_exec (int op, int s32, int rm, t_uint64 d1, t_uint64 d2, t_uint64 d3, t_uint64 *res, uint8 *flags);
void m63f09_fpu_cvt (int kind, int cop, int s32, int rm, t_uint64 d1, t_uint64 *res, uint8 *flags);

/* CPU state selected by MD: 6809, 6309, 63F09, 63F09 with FPU (an M63_OPCODE_ARCH_* bit of the generated table) */
uint16 m63f09_arch_of_md (uint8 md);

/* Row of the opcode table for (page, opcode) in the CPU state arch, or NULL (the core takes a TRAP).  The table says which
   assembler target accepts an instruction; the core decodes by its MD register, and the VHDL has one exception: LDMD
   immediate (page 0x11, opcode 0x3D) is decoded in every state, because it is what leaves the 6809 state. */
const m63f09_op_t *m63f09_find_op (uint8 page, uint8 opcode, uint16 arch);

/* Disassembler (m63f09_dis.c): prints one instruction from the bytes mem[0..avail-1] found at logical address addr and
   returns its length in bytes (1 for an unknown opcode, shown as .byte).  wide = 1 in 32-bit mode (wider fields). */
int m63f09_disasm (FILE *of, t_addr addr, const uint8 *mem, int avail, uint16 arch, int wide);
#define M63_MAXINSN     12                              /* longest instruction (page + opcode + 64-bit immediate = 10) */

#endif
