/*
 * Sun-3 PROM boot block entry for the Linux/sun3 boot program.
 *
 * The PROM's device-independent boot (ROM 0x5E98 in 3.60 v3.0.1) reads
 * 8 KiB from block 1 of the boot partition to physical 0x4000 and jumps
 * there in supervisor mode with the monitor's maps live (low 8 MB
 * identity-mapped in context 0).  Like NetBSD's bootxx and SunOS's
 * bootsd we first move ourselves out of the way of what we are about to
 * load (they go to 0x240000; the Linux image is bigger, so RELOC is
 * 0x780000, just under the identity map) and run from the link address.
 */
	.section .note.GNU-stack,"",@progbits
	.text
	.globl	_start
_start:
	move.w	#0x2700,%sr
	lea	_start(%pc),%a0		| where the PROM put us
	lea	_start,%a1		| where we are linked (RELOC)
	cmp.l	%a0,%a1
	beq	1f
	move.w	#(8192 / 4) - 1,%d0
0:	move.l	(%a0)+,(%a1)+
	dbf	%d0,0b
	jmp	1f:l			| absolute: continue in the copy
1:	lea	__stack_top,%sp
	lea	__bss_start,%a0
	lea	__bss_end,%a1
2:	cmp.l	%a0,%a1
	beq	3f
	clr.l	(%a0)+
	bra	2b
3:	jsr	boot_main
	/* not reached unless the load failed: back to the monitor */
	move.l	0x0FEF00C4,%a0		| romvec->exitToMon
	jmp	(%a0)
