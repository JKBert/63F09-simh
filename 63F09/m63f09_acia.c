/* m63f09_acia.c: ACIA 63F50 (two serial lines)

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

   Model of ACIA_63F50 (63F50.vhd): a 6850-compatible serial line with a 16-bit divisor of SYSCLK (24 MHz) instead of the external baud
   generator.  Four bytes each: ACIA0 at $FFFEE000, ACIA1 at $FFFEE004.
     +0 read  status   bit 0 RDRF (receive data ready), bit 1 TDRE (transmit data register empty, the CTS input is assumed asserted),
                       bit 5 overrun, bit 7 IRQ flag ; bits 2, 3 and 4 read 0
     +0 write control  bits 1-0 baud divide (00 /1, 01 /16, 10 /64, 11 master reset), bits 4-2 word format, bits 6-5 transmit control,
                       bit 7 receive interrupt enable
     +1 read  received byte (clears RDRF) ; +1 write byte to send
     +2 / +3  divisor high / low byte (reads $FFFF at the start, kept by a master reset)
   Bit time = (D + 1) x N / SYSCLK, N = 1, 16 or 64 ; one character = 10 bits.  The time is counted in cycles of the core clock (SET CPU FREQ).
   The transmitter is double buffered as on the RTL (measured with a polling program on soc63f09): a byte written while the shifter is free
   enters it half a bit time later (TDRE low until then) ; a byte written while a character is being sent enters it when that character
   ends.  ACIA0 is the console: the bytes sent go to the SCP console, the keyboard is polled to fill the receiver.  ACIA1 is not
   connected.  The interrupt output is wired to the core: ACIA0 to EIRQ23, ACIA1 to EIRQ22 (SoC.vhd), a level.
   Not reproduced: the exact phase of the baud clock (the wait for TDRE after a write is 0 to 1 bit time on the RTL, half a bit here), and the
   absolute time scale, which was not calibrated against the RTL.
*/

#include <string.h>
#include <unistd.h>
#include <poll.h>
#include "m63f09_defs.h"

extern DEVICE acia0_dev, acia1_dev;
extern UNIT acia0_units[2], acia1_unit;

#define ACIA_RDRF   0x01
#define ACIA_TDRE   0x02
#define ACIA_OVRN   0x20
#define ACIA_IRQ    0x80

typedef struct {
    uint8  ctrl;
    uint8  rx;
    uint8  rx_full;
    uint8  tx_busy;                                     /* the data register is full (TDRE low) */
    uint8  txd;                                         /* the byte waiting in the data register */
    uint8  shifting;                                    /* the shift register is sending a character */
    uint16 div;
    uint8  overrun;
} acia_t;

static acia_t acia[2];

/* One bit, and one character (10 bits), in cycles of the core clock */
static int32 bit_cycles (const acia_t *a)
{
    t_uint64 n = ((a->ctrl & 3) == 0) ? 1 : ((a->ctrl & 3) == 1) ? 16 : 64;
    t_uint64 c = ((t_uint64) a->div + 1) * n * m63f09_cpu_hz / M63_SYSCLK_HZ;

    return (int32) (c < 1 ? 1 : c > 0x03FFFFFF ? 0x03FFFFFF : c);
}

static uint8 acia_status (const acia_t *a)
{
    uint8 st = 0;

    if (a->rx_full) st |= ACIA_RDRF;
    if (!a->tx_busy) st |= ACIA_TDRE;
    if (a->overrun) st |= ACIA_OVRN;
    if (((a->ctrl & 0x80) && a->rx_full) || (((a->ctrl & 0x60) == 0x20) && !a->tx_busy))
        st |= ACIA_IRQ;
    return st;
}

/* The interrupt output of ACIA0 is EIRQ23 and the one of ACIA1 EIRQ22 (SoC.vhd), active while the IRQ flag of the status register is set
   ((RxIE and RxRdy) or (TxIE and TxRdy)): a level, updated at each change of the state of the line. */
static void acia_irq (const acia_t *a)
{
    int line = (a == &acia[1]);

    m63f09_set_line (&m63f09_cores[0], M63_LINE_EIRQ + 23 - line, (acia_status (a) & ACIA_IRQ) != 0);
}

static int acia_rd (void *ctx, uint32 off, uint8 *val)
{
    acia_t *a = (acia_t *) ctx;

    switch (off & 3) {
        case 0:  *val = acia_status (a); break;
        case 1:  *val = a->rx; a->rx_full = 0; a->overrun = 0; break;
        case 2:  *val = (uint8) (a->div >> 8); break;
        default: *val = (uint8) a->div; break;
    }
    acia_irq (a);
    return 0;
}

static int acia_wr (void *ctx, uint32 off, uint8 val)
{
    acia_t *a = (acia_t *) ctx;
    DEVICE *d = (a == &acia[1]) ? &acia1_dev : &acia0_dev;

    switch (off & 3) {
        case 0:
            if ((val & 3) == 3) {                       /* master reset: the divisor is not touched */
                a->ctrl = 0;
                a->rx_full = 0;
                a->tx_busy = 0;
                a->shifting = 0;
                a->overrun = 0;
                sim_cancel (&d->units[0]);
            } else
                a->ctrl = val;
            break;
        case 1:
            a->txd = val;
            a->tx_busy = 1;
            if (!a->shifting)
                sim_activate_abs (&d->units[0], bit_cycles (a) / 2 + 1);
            break;
        case 2:
            a->div = (uint16) ((a->div & 0x00FF) | ((uint16) val << 8));
            break;
        default:
            a->div = (uint16) ((a->div & 0xFF00) | val);
            break;
    }
    acia_irq (a);
    return 0;
}

/* Unit 0 is the transmitter: it moves the data register to the shift register and ends the character.  Unit 1 (ACIA0 only) polls
   the console keyboard. */
#define KBD_POLL_CYCLES 200000

static void tx_start (acia_t *a, int line, UNIT *uptr)
{
    a->tx_busy = 0;                                     /* the data register is free again */
    a->shifting = 1;
    acia_irq (a);
    if (line == 0)
        sim_putchar (a->txd);
    sim_activate_abs (uptr, 10 * bit_cycles (a));
}

static t_stat acia_tx_svc (UNIT *uptr)
{
    int line = (uptr == &acia1_unit);
    acia_t *a = &acia[line];

    if (a->shifting)                                    /* the character has been sent */
        a->shifting = 0;
    if (a->tx_busy)                                     /* a byte is waiting: it enters the shift register */
        tx_start (a, line, uptr);
    return SCPE_OK;
}

/* SCP polls the console only when stdin is a terminal (sim_poll_kbd), so a pipe or a file on stdin gets nothing.  The characters of
   such a stdin go to the receiver here (the SEND command of SCP keeps working, it goes through sim_poll_kbd).  poll() first, so that
   an empty pipe never blocks the simulation ; the end of the input is remembered. */
static int stdin_char (void)
{
    static int eof = 0;
    struct pollfd p;
    unsigned char ch;

    if (eof || sim_ttisatty ())
        return -1;
    p.fd = 0;
    p.events = POLLIN;
    p.revents = 0;
    if (poll (&p, 1, 0) <= 0)
        return -1;
    if (read (0, &ch, 1) != 1) {
        eof = 1;
        return -1;
    }
    return ch;
}

static t_stat acia_kbd_svc (UNIT *uptr)
{
    acia_t *a = &acia[0];
    t_stat c = sim_poll_kbd ();
    int32 next = KBD_POLL_CYCLES;

    if (c < SCPE_KFLAG && c != SCPE_STOP && !a->rx_full) {      /* not before the receiver has been read: the pipe waits */
        int ch = stdin_char ();

        if (ch >= 0)
            c = (t_stat) ch | SCPE_KFLAG;
    }
    if (c >= SCPE_KFLAG) {
        if (a->rx_full)
            a->overrun = 1;
        a->rx = (uint8) (c & 0xFF);
        a->rx_full = 1;
        acia_irq (a);
        next = 10 * bit_cycles (a);                     /* the next character cannot come before the end of this one */
        if (next < KBD_POLL_CYCLES)
            next = KBD_POLL_CYCLES;
    } else if (c == SCPE_STOP)
        return c;
    sim_activate (uptr, next);
    return SCPE_OK;
}

static t_stat acia_reset (DEVICE *dptr)
{
    int line = (dptr == &acia1_dev);
    acia_t *a = &acia[line];

    memset (a, 0, sizeof (*a));
    a->div = 0xFFFF;
    m63f09_set_line (&m63f09_cores[0], M63_LINE_EIRQ + 23 - line, 0);
    m63f09_io_register (line ? 0xFFFFEE004ULL : 0xFFFFEE000ULL, 4, acia_rd, acia_wr, a);
    sim_cancel (&dptr->units[0]);
    if (line == 0) {
        sim_cancel (&dptr->units[1]);
        sim_activate_abs (&dptr->units[1], KBD_POLL_CYCLES);
    }
    return SCPE_OK;
}

UNIT acia0_units[2] = { { UDATA (&acia_tx_svc, 0, 0) }, { UDATA (&acia_kbd_svc, 0, 0) } };
UNIT acia1_unit = { UDATA (&acia_tx_svc, 0, 0) };

REG acia0_reg[] = {
    { HRDATAD (CTRL, acia[0].ctrl, 8, "control register") },
    { HRDATAD (DIV, acia[0].div, 16, "divisor of SYSCLK") },
    { HRDATAD (RX, acia[0].rx, 8, "received byte") },
    { FLDATAD (RXFULL, acia[0].rx_full, 0, "receive data ready") },
    { FLDATAD (TXBUSY, acia[0].tx_busy, 0, "data register full (TDRE low)") },
    { FLDATAD (SHIFTING, acia[0].shifting, 0, "shift register busy") },
    { NULL }
};
REG acia1_reg[] = {
    { HRDATAD (CTRL, acia[1].ctrl, 8, "control register") },
    { HRDATAD (DIV, acia[1].div, 16, "divisor of SYSCLK") },
    { HRDATAD (RX, acia[1].rx, 8, "received byte") },
    { FLDATAD (RXFULL, acia[1].rx_full, 0, "receive data ready") },
    { FLDATAD (TXBUSY, acia[1].tx_busy, 0, "data register full (TDRE low)") },
    { FLDATAD (SHIFTING, acia[1].shifting, 0, "shift register busy") },
    { NULL }
};

DEVICE acia0_dev = {
    "ACIA0", acia0_units, acia0_reg, NULL, 2, 16, 4, 1, 16, 8,
    NULL, NULL, &acia_reset, NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
DEVICE acia1_dev = {
    "ACIA1", &acia1_unit, acia1_reg, NULL, 1, 16, 4, 1, 16, 8,
    NULL, NULL, &acia_reset, NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
