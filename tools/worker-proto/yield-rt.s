# SPDX-License-Identifier: GPL-2.0-or-later
# Worker-process prototype, approach C (yield points): the counter that the inserted
# check decrements at every loop header. A WebAssembly global, not memory, so the check
# is one global.get, an add, a global.set and a branch. See yield-cc.sh.
	.globaltype	__ucos_yield_ctr, i32
	.globl	__ucos_yield_ctr
__ucos_yield_ctr:

	.globl	__ucos_yield_set
	.type	__ucos_yield_set,@function
__ucos_yield_set:
	.functype	__ucos_yield_set (i32) -> ()
	local.get	0
	global.set	__ucos_yield_ctr
	end_function
