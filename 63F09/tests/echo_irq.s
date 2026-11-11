/* echo_irq.s: the same echo, driven by the receive interrupt of ACIA0 (EIRQ23, vector $FFFFFFEE).  The main program only waits for the 'q'
   that the handler stores in RAM.  Build:  as-new -m63f09 echo_irq.s -o echo_irq.o && ld-new -m m6809soc echo_irq.o -o echo_irq.elf
   Run, from the root of the tree:  BIN/m63f09 63F09/tests/echo_irq.ini */
	.text
	.globl	_start
_start:
	ldmd	#$11
	clra
	sta	>$FFFC0300		/* last byte received */
	lds	#$FFFD0000
	ldx	#$FFFEE000
	clr	2,x			/* divisor 12 */
	lda	#12
	sta	3,x
	lda	#$95			/* /16, 8N1, receive interrupt enabled */
	sta	,x
loop:	lda	>$FFFC0300
	cmpa	#'q
	bne	loop
done:	bra	done
handler:
	ldx	#$FFFEE000
	ldb	1,x			/* reading the byte clears the request */
	stb	>$FFFC0300
tx:	lda	,x
	bita	#$02			/* TDRE */
	beq	tx
	stb	1,x
	rti
	.section .vectors,"a"
	.rept	23
	.fdb	0
	.endr
	.fdb	%lo16(handler)		/* EIRQ23 : $FFFFFFC0 + 2 * 23 */
	.rept	7
	.fdb	0
	.endr
	.fdb	%lo16(_start)
