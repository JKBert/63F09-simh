/* newlib_hello.c -- first real link against newlib's libc.a for this
 * port (newlib port plan, etape 1: minimal glue, zero real I/O).
 *
 * Exercises the three things etape 1 set out to prove: printf/malloc
 * link and produce real code (not stubs), setjmp/longjmp actually
 * unwind (this port's ABI has no callee-saved registers to restore,
 * see libc/machine/m6809/setjmp.S -- verified structurally via
 * disassembly, never executed, no simulator/board exists). _write
 * itself is still a stub (libc/sys/m6809/syscalls.c, real console I/O
 * is etape 2) -- printf here still exercises the real formatting/
 * buffering code, only the final byte-sink call fails.
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

static jmp_buf jb;

static void unwind_to_main(int n)
{
  longjmp(jb, n);
}

int main(void)
{
  int *p = malloc(sizeof(int));
  if (!p)
    return 1;
  *p = 42;

  if (setjmp(jb) == 0)
    unwind_to_main(7);

  iprintf("p=%d\n", *p);
  free(p);
  return 0;
}
