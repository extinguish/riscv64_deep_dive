	.option nopic
	.option norelax

	.section .data
	.balign 8

src:
	.byte 0x80, 0x00
	.half 0x8001
	.word 0x80000001
	.quad 0x1122334455667788

dst:
	.zero 16
	
	.section .text
	.global _start

_start:
	la s0, src # load the address of src to s0 register
	
	lb t0, (s0)
	lbu t1, (s0)
	lh t2, 2(s0)
	lhu t3, 2(s0)
	lw t4, 4(s0)
	lwu t5, 4(s0)
	ld t6, 8(s0)

after_loads:
	nop
	
	li s2, -128
	bne t0, s2, failed
	li s2, 0x80
	bne t1, s2, failed
	li s2, -32767
	bne t2, s2, failed
	li s2, 0x8001
	bne t3, s2, failed
	li s2, -2147483647
	bne t4, s2, failed
	li s2, 0x80000001
	bne t5, s2, failed
	li s2, 0x1122334455667788
	bne t6, s2, failed
	
	la s0, dst
	li s1, 0x1122334455667788
	
	sb s1, 0(s0)
	# dst[0] = 0x88
	sh s1, 2(s0)
	# dst[2..3] = 0x88 0x77
	sw s1, 4(s0)
	# dst[4..7] = 0x88 0x77 0x66 0x55
	sd s1, 8(s0)
	# dst[8..15] = 0x88 0x77 0x66 0x55 0x44 0x33 0x22 0x11

after_stores:
	nop
	ld t0, 0(s0)
	li t1, 0x5566778877880088
	bne t0, t1, failed
	
	ld t0, 8(s0)
	bne t0, s1, failed
	
	# the following are using to simulate the linux exit(0)
	li a0, 0
	li a7, 93
	ecall

failed:
	li a0, 1
	li a7, 93
	ecall

	
	
