# SPDX-License-Identifier: GPL-2.0-or-later
# M1: the smallest static riscv64 Linux program. write(1, msg, 14); exit(0).
	.globl _start
_start:
	li a0, 1
	la a1, msg
	li a2, 14
	li a7, 64
	ecall
	li a0, 0
	li a7, 93
	ecall
	.section .rodata
msg:	.ascii "Hello, world!\n"
