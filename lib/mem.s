# SPDX-License-Identifier: GPL-2.0-or-later
# memcpy / memmove / memset as single WebAssembly bulk-memory instructions.
# Shared by the kernel and the user libc. The engine turns these into its
# fastest native copy loop, so there is nothing to gain from a C loop here.
	.text
	.globl	memcpy
	.type	memcpy,@function
memcpy:
	.functype	memcpy (i32, i32, i32) -> (i32)
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
	memory.copy	0, 0
	local.get	0
	end_function

	.globl	memset
	.type	memset,@function
memset:
	.functype	memset (i32, i32, i32) -> (i32)
	local.get	0
	local.get	1
	local.get	2
	memory.fill	0
	local.get	0
	end_function
