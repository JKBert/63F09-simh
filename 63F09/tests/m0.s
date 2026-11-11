	.text
	.globl	_start
_start:
	ldmd	#$31
	lds	#$FFFD0000
	ldx	#msg
	ldb	#3
	lslnb	#2
	lsrnx	#1
	pshs	u,x,y
	tfm	x+,y+
	leax	[,u++]
	puls	u,x,y
	bra	_start
	.data
msg:	.asciz	"hello"
	.section .vectors,"a"
	.rept	31
	.fdb	0
	.endr
	.fdb	%lo16(_start)
