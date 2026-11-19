#include "idx_n.h"
#include "idx_exp.h"
#define ACIA_STATUS  (*(volatile unsigned char *) 0xFFFEE000UL)
#define ACIA_CONTROL (*(volatile unsigned char *) 0xFFFEE000UL)
#define ACIA_DATA    (*(volatile unsigned char *) 0xFFFEE001UL)
#define ACIA_DIV_HI  (*(volatile unsigned char *) 0xFFFEE002UL)
#define ACIA_DIV_LO  (*(volatile unsigned char *) 0xFFFEE003UL)
static void putc_ (char c) { while ((ACIA_STATUS & 2) == 0) ; ACIA_DATA = (unsigned char) c; }
static void puts_ (const char *s) { while (*s) putc_ (*s++); }
static void puthex (unsigned long v) { int i; for (i = 28; i >= 0; i -= 4) putc_ ("0123456789ABCDEF"[(v >> i) & 15]); }
extern unsigned char win[];
extern unsigned long res[];
extern void idx_run (void);
int main (void)
{
  int i, bad = 0;
  ACIA_CONTROL = 3; ACIA_DIV_HI = 0; ACIA_DIV_LO = 0; ACIA_CONTROL = 0x15;
  for (i = 0; i < WIN; i++) win[i] = (unsigned char) ((i * 7 + 3) & 255);
  idx_run ();
  for (i = 0; i < NRES; i++)
    if (res[i] != want[i])
      {
        if (bad < 12) { puts_ ("BAD "); puthex ((unsigned long) i); putc_ (' '); puthex (res[i]); putc_ (' '); puthex (want[i]); putc_ ('\n'); }
        bad++;
      }
  puts_ (bad ? "FAIL " : "OK "); puthex ((unsigned long) NRES); puts_ (" checks, "); puthex ((unsigned long) bad); putc_ ('\n');
  for (;;) ;
}
