/* m63f09_sys.c: 63F09 simulator, SCP data structures, loaders and symbolic display

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

   Loaders (LOAD file [address]):
     - an ELF32 big-endian file of this toolchain: the sections to allocate are loaded at their LMA (the program headers
       give it for .data placed after the ROM image), the NOBITS ones are cleared.  The first PT_LOAD of a file linked
       with the m6809soc emulation starts before .text (it maps the ELF headers), so the segments are never loaded as such.
     - anything else: raw binary at the physical address given.  Addresses of an ELF file are the addresses of a core with
       the MMU off: 0xF followed by the 32-bit address.
*/

#include <stdlib.h>
#include <string.h>
#include "m63f09_defs.h"

extern DEVICE cpu_dev;
extern DEVICE acia0_dev;
extern DEVICE acia1_dev;
extern REG cpu_reg[];
uint16 m63f09_dis_arch (void);

char sim_name[] = "63F09 SoC";

REG *sim_PC = &cpu_reg[0];

int32 sim_emax = M63_MAXINSN;

DEVICE *sim_devices[] = {
    &cpu_dev,
    &acia0_dev,
    &acia1_dev,
    NULL
};

const char *sim_stop_messages[SCPE_BASE] = {
    "Unknown error",
    "HALT",
    "Breakpoint",
    "Instruction not implemented (the instruction set starts with milestone M1)",
    "Nonexistent memory",
    "Branch to self (end of program)",
    "CWAI/SYNC with no interrupt source"
};

/* ELF ------------------------------------------------------------------------------------------------------------ */

static uint32 be32 (const uint8 *p) { return ((uint32) p[0] << 24) | ((uint32) p[1] << 16) | ((uint32) p[2] << 8) | p[3]; }
static uint32 be16 (const uint8 *p) { return ((uint32) p[0] << 8) | p[1]; }

static t_stat put_bytes (uint32 addr, const uint8 *src, uint32 size, int zero)
{
    uint32 i;

    for (i = 0; i < size; i++)
        if (m63f09_write8_dbg (PA_PREFIX | (uint32) (addr + i), zero ? 0 : src[i])) {
            sim_printf ("Nonexistent memory at 0x%09llX (use SET CPU EXTMEM=n for external memory)\n",
                        (unsigned long long) (PA_PREFIX | (uint32) (addr + i)));
            return SCPE_NXM;
        }
    return SCPE_OK;
}

static t_stat load_elf (const uint8 *f, size_t size)
{
    uint32 phoff = be32 (f + 28), shoff = be32 (f + 32), flags = be32 (f + 36);
    uint32 phentsize = be16 (f + 42), phnum = be16 (f + 44), shentsize = be16 (f + 46), shnum = be16 (f + 48);
    uint32 i, j, loaded = 0, bytes = 0;
    t_stat r;

    if (f[4] != 1 || f[5] != 2) {
        sim_printf ("ELF file is not 32-bit big-endian\n");
        return SCPE_FMT;
    }
    if (shnum == 0 && phnum > 0 && phentsize >= 32 && (size_t) phoff + (size_t) phnum * phentsize <= size) {
        /* no section headers (s09_to_elf.py, objcopy of a binary): the segments are the program */
        for (i = 0; i < phnum; i++) {
            const uint8 *ph = f + phoff + (size_t) i * phentsize;
            uint32 poff = be32 (ph + 4), ppaddr = be32 (ph + 12), pfilesz = be32 (ph + 16), pmemsz = be32 (ph + 20);

            if (be32 (ph) != 1 || pmemsz == 0)
                continue;
            if ((size_t) poff + pfilesz > size) {
                sim_printf ("ELF file: segment %u beyond the end of the file\n", i);
                return SCPE_FMT;
            }
            if ((r = put_bytes (ppaddr, f + poff, pfilesz, 0)) != SCPE_OK)
                return r;
            if (pmemsz > pfilesz && (r = put_bytes (ppaddr + pfilesz, NULL, pmemsz - pfilesz, 1)) != SCPE_OK)
                return r;
            loaded++;
            bytes += pmemsz;
        }
        sim_printf ("ELF (segments): %u segments, %u bytes, entry 0x%08X, flags 0x%X\n", loaded, bytes, be32 (f + 24), flags);
        return SCPE_OK;
    }
    if (shoff == 0 || shentsize < 40 || (size_t) shoff + (size_t) shnum * shentsize > size
            || (size_t) phoff + (size_t) phnum * phentsize > size) {
        sim_printf ("ELF file: bad header tables\n");
        return SCPE_FMT;
    }
    for (i = 0; i < shnum; i++) {
        const uint8 *sh = f + shoff + (size_t) i * shentsize;
        uint32 type = be32 (sh + 4), shflags = be32 (sh + 8), addr = be32 (sh + 12), off = be32 (sh + 16), sz = be32 (sh + 20);
        uint32 lma = addr;

        if (!(shflags & 2) || sz == 0 || type == 0)         /* not SHF_ALLOC, empty, or SHT_NULL */
            continue;
        if (type != 8) {                                    /* has contents: the LMA is given by the segment that holds it */
            if ((size_t) off + sz > size) {
                sim_printf ("ELF file: section %u beyond the end of the file\n", i);
                return SCPE_FMT;
            }
            for (j = 0; j < phnum; j++) {
                const uint8 *ph = f + phoff + (size_t) j * phentsize;
                uint32 poff = be32 (ph + 4), ppaddr = be32 (ph + 12), pfilesz = be32 (ph + 16);

                if (be32 (ph) == 1 && off >= poff && off - poff < pfilesz) {
                    lma = ppaddr + (off - poff);
                    break;
                }
            }
            if ((r = put_bytes (lma, f + off, sz, 0)) != SCPE_OK)
                return r;
        } else if ((r = put_bytes (lma, NULL, sz, 1)) != SCPE_OK)
            return r;
        loaded++;
        bytes += sz;
    }
    sim_printf ("ELF: %u sections, %u bytes, entry 0x%08X, flags 0x%X (%s%s)\n", loaded, bytes, be32 (f + 24), flags,
                (flags & 1) ? "32-bit (63F09)" : "16-bit", (flags & 2) ? ", FPU" : "");
    return SCPE_OK;
}

t_stat sim_load (FILE *fileref, CONST char *cptr, CONST char *fnam, int flag)
{
    uint8 *buf;
    long size;
    t_stat r;

    if (flag) {                                             /* DUMP file start-end : raw bytes of the physical memory */
        t_stat st;
        t_value lo, hi, a;
        char *dash;
        char tmp[64];

        if (!cptr || !*cptr || strlen (cptr) >= sizeof (tmp))
            return SCPE_ARG;
        strcpy (tmp, cptr);
        dash = strchr (tmp, '-');
        if (!dash)
            return SCPE_ARG;
        *dash = 0;
        lo = get_uint (tmp, 16, PA_MASK, &st);
        if (st != SCPE_OK) return st;
        hi = get_uint (dash + 1, 16, PA_MASK, &st);
        if (st != SCPE_OK || hi < lo) return SCPE_ARG;
        for (a = lo; a <= hi; a++) {
            uint8 v;

            if (m63f09_read8 (a, &v))
                return SCPE_NXM;
            fputc (v, fileref);
        }
        return SCPE_OK;
    }
    fseek (fileref, 0, SEEK_END);
    size = ftell (fileref);
    fseek (fileref, 0, SEEK_SET);
    if (size <= 0)
        return SCPE_FMT;
    buf = (uint8 *) malloc ((size_t) size);
    if (!buf)
        return SCPE_MEM;
    if (fread (buf, 1, (size_t) size, fileref) != (size_t) size) {
        free (buf);
        return SCPE_IOERR;
    }
    if (size >= 52 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')
        r = load_elf (buf, (size_t) size);
    else {
        t_stat st;
        t_value pa;
        long k;

        if (!cptr || !*cptr) {
            sim_printf ("Raw binary: LOAD file physical-address\n");
            free (buf);
            return SCPE_ARG;
        }
        pa = get_uint (cptr, 16, PA_MASK, &st);
        if (st != SCPE_OK) {
            free (buf);
            return st;
        }
        r = SCPE_OK;
        for (k = 0; k < size && r == SCPE_OK; k++)
            if (m63f09_write8_dbg (pa + k, buf[k])) {
                sim_printf ("Nonexistent memory at 0x%09llX\n", (unsigned long long) (pa + k));
                r = SCPE_NXM;
            }
        if (r == SCPE_OK)
            sim_printf ("Raw binary: %ld bytes at 0x%09llX\n", size, (unsigned long long) pa);
    }
    free (buf);
    return r;
}

/* Symbolic display ------------------------------------------------------------------------------------------------ */

t_stat fprint_sym (FILE *of, t_addr addr, t_value *val, UNIT *uptr, int32 sw)
{
    if (sw & SWMASK ('M')) {
        uint8 b[M63_MAXINSN];
        uint16 arch = m63f09_dis_arch ();
        int i, len;

        for (i = 0; i < M63_MAXINSN; i++)
            b[i] = (uint8) val[i];
        len = m63f09_disasm (of, addr, b, M63_MAXINSN, arch, (arch & (M63_OPCODE_ARCH_63F09_32BITS | M63_OPCODE_ARCH_63F09_32BITS_FPU)) != 0);
        return (t_stat) (-(len - 1));
    }
    if (sw & SWMASK ('C')) {
        fprintf (of, (val[0] >= 0x20 && val[0] < 0x7f) ? "'%c'" : "'.'", (int) val[0]);
        return SCPE_OK;
    }
    return SCPE_ARG;
}

t_stat parse_sym (CONST char *cptr, t_addr addr, UNIT *uptr, t_value *val, int32 sw)
{
    return SCPE_ARG;                                        /* no assembler: use the numeric forms */
}
