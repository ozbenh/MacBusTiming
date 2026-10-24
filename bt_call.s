| Calls the generated timing code.
|
| void bt_call(struct bt_args *args)
|
| The generated code expects:
|   a0 = address accessed by the timed instruction
|   a2 = VIA1 T2 counter low, a3 = VIA1 T2 counter high
|   d1 = value written by the write instruction
|   d6 = loop count - 1, d7 = $FF (the value loaded into T2)
| and returns T2 high, low, high in d2, d3, d4.

	.text
	.globl	bt_call
bt_call:
	move.l	4(%sp),%a1
	movem.l	%d2-%d7/%a2-%a4,-(%sp)
	move.l	%a1,-(%sp)
	move.l	(%a1),%a4
	move.l	4(%a1),%a0
	move.l	8(%a1),%a2
	move.l	12(%a1),%a3
	moveq	#0,%d6
	move.w	16(%a1),%d6
	move.l	24(%a1),%d1
	moveq	#-1,%d7
	moveq	#0,%d2
	moveq	#0,%d3
	moveq	#0,%d4
	moveq	#0,%d5
	jsr	(%a4)
	move.l	(%sp)+,%a1
	move.b	%d2,19(%a1)
	move.b	%d3,20(%a1)
	move.b	%d4,21(%a1)
	movem.l	(%sp)+,%d2-%d7/%a2-%a4
	rts
