/* echo.s: echo of the keyboard on ACIA0 (polling), for the interactive simulator.  A 'q' stops the program (branch to self).
   Build:  as-new -m63f09 echo.s -o echo.o && ld-new -m m6809soc echo.o -o echo.elf
   Run, from the root of the tree:  BIN/m63f09 63F09/tests/echo.ini   (Ctrl-E returns to the SCP prompt) */
	.text
	.globl	_start
_start:
	ldmd	#$11
	lds	#$FFFD0000
	ldx	#$FFFEE000		/* ACIA0 */
	clr	2,x			/* divisor 12 : 115200 baud at 24 MHz (it is $FFFF after a reset) */
	lda	#12
	sta	3,x
	lda	#$15			/* /16, 8 bits no parity 1 stop, no interrupt */
	sta	,x
wait:	lda	,x			/* status */
	bita	#$01			/* RDRF */
	beq	wait
	ldb	1,x			/* the byte */
tx:	lda	,x
	bita	#$02			/* TDRE */
	beq	tx
	stb	1,x			/* sends it back */
	cmpb	#'q
	bne	wait
done:	bra	done
	.section .vectors,"a"
	.rept	31
	.fdb	0
	.endr
	.fdb	%lo16(_start)
