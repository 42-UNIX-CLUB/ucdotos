# SPDX-License-Identifier: GPL-2.0-or-later
# memcpy / memmove / memset for the user C library (the kernel uses lib/mem.s).
# Above 16 bytes they are single WebAssembly bulk-memory instructions, which the
# engine turns into its fastest native copy loop. Up to 16 bytes they use plain
# loads and stores instead: the engine compiles a memory.copy or memory.fill of
# unknown length into a call out of the generated code, which costs more than
# the copy itself at these sizes (measured with bin/ubench, see PERFORMANCE.md).
# The kernel keeps the one-instruction versions: its calls are 64 bytes or more,
# and there the larger functions measured slower (they are no longer inlined).
# The short paths load everything before storing, so they are also correct for
# overlapping memmove. Lengths 8..16: two 8-byte words that may overlap;
# 4..7: two 4-byte words; 1..3: the first, the middle and the last byte.
	.text
	.globl	memcpy
	.type	memcpy,@function
memcpy:
	.functype	memcpy (i32, i32, i32) -> (i32)
	.local	i64, i64
	block
	local.get	2
	i32.const	16
	i32.gt_u
	br_if	0
	local.get	2
	i32.const	8
	i32.ge_u
	if
	local.get	1
	i64.load	0:p2align=0
	local.set	3
	local.get	1
	local.get	2
	i32.add
	i32.const	8
	i32.sub
	i64.load	0:p2align=0
	local.set	4
	local.get	0
	local.get	3
	i64.store	0:p2align=0
	local.get	0
	local.get	2
	i32.add
	i32.const	8
	i32.sub
	local.get	4
	i64.store	0:p2align=0
	local.get	0
	return
	end_if
	local.get	2
	i32.const	4
	i32.ge_u
	if
	local.get	1
	i64.load32_u	0:p2align=0
	local.set	3
	local.get	1
	local.get	2
	i32.add
	i32.const	4
	i32.sub
	i64.load32_u	0:p2align=0
	local.set	4
	local.get	0
	local.get	3
	i64.store32	0:p2align=0
	local.get	0
	local.get	2
	i32.add
	i32.const	4
	i32.sub
	local.get	4
	i64.store32	0:p2align=0
	local.get	0
	return
	end_if
	local.get	2
	i32.eqz
	if
	local.get	0
	return
	end_if
	local.get	1
	i64.load8_u	0
	local.set	3
	local.get	1
	local.get	2
	i32.const	1
	i32.shr_u
	i32.add
	i64.load8_u	0
	local.set	4
	local.get	1
	local.get	2
	i32.add
	i32.const	1
	i32.sub
	i32.load8_u	0
	local.set	1
	local.get	0
	local.get	3
	i64.store8	0
	local.get	0
	local.get	2
	i32.const	1
	i32.shr_u
	i32.add
	local.get	4
	i64.store8	0
	local.get	0
	local.get	2
	i32.add
	i32.const	1
	i32.sub
	local.get	1
	i32.store8	0
	local.get	0
	return
	end_block
	local.get	0
	local.get	1
	local.get	2
	memory.copy	0, 0
	local.get	0
	end_function

	.globl	memmove
	.type	memmove,@function
memmove:
	.functype	memmove (i32, i32, i32) -> (i32)
	local.get	0
	local.get	1
	local.get	2
	call	memcpy
	end_function

	.globl	memset
	.type	memset,@function
memset:
	.functype	memset (i32, i32, i32) -> (i32)
	.local	i64
	block
	local.get	2
	i32.const	16
	i32.gt_u
	br_if	0
	local.get	1
	i64.extend_i32_u
	i64.const	255
	i64.and
	i64.const	72340172838076673
	i64.mul
	local.set	3
	local.get	2
	i32.const	8
	i32.ge_u
	if
	local.get	0
	local.get	3
	i64.store	0:p2align=0
	local.get	0
	local.get	2
	i32.add
	i32.const	8
	i32.sub
	local.get	3
	i64.store	0:p2align=0
	local.get	0
	return
	end_if
	local.get	2
	i32.const	4
	i32.ge_u
	if
	local.get	0
	local.get	3
	i64.store32	0:p2align=0
	local.get	0
	local.get	2
	i32.add
	i32.const	4
	i32.sub
	local.get	3
	i64.store32	0:p2align=0
	local.get	0
	return
	end_if
	local.get	2
	i32.eqz
	if
	local.get	0
	return
	end_if
	local.get	0
	local.get	3
	i64.store8	0
	local.get	0
	local.get	2
	i32.const	1
	i32.shr_u
	i32.add
	local.get	3
	i64.store8	0
	local.get	0
	local.get	2
	i32.add
	i32.const	1
	i32.sub
	local.get	3
	i64.store8	0
	local.get	0
	return
	end_block
	local.get	0
	local.get	1
	local.get	2
	memory.fill	0
	local.get	0
	end_function
