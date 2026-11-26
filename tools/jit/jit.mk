# SPDX-License-Identifier: GPL-2.0-or-later
# The binary translator's own builds (docs/BINARY-TRANSLATION.md). Does not touch the main
# Makefile's outputs.
#   make -f tools/jit/jit.mk B=build-NAME            everything below
#   make -f tools/jit/jit.mk B=build-NAME guests     only the test programs
# Outputs in $(B)/jit/:
#   engine.wasm          the engine as a standalone wasm32 module (tools/jit/run.mjs, bench.html)
#   rvjit-native         the engine built for the host (interpreter only; for reference runs)
#   xltest               native unit test: translates every function of a guest, writes modules
#   guest/NAME.elf       test program for riscv64 (rv64im; NAME-gc.elf: rv64gc)
#   guest/NAME.wasm      the same C compiled straight to wasm32 (the ceiling)
#   guest/NAME.native    the same C compiled for the host
B       ?= build
O       := $(B)/jit
LIBJIT  := libjit/wasm.c libjit/rvdec.c libjit/xlat.c libjit/rvsem.c libjit/rvfp.c libjit/rvinterp.c libjit/engine.c libjit/fmt.c
HDR     := $(wildcard libjit/*.h)
GUESTS  := sieve sort sha256 fib matmul strloop indirect switch hello
FPGUESTS := nbody fpmix
WCC     := clang --target=wasm32-unknown-unknown
WFLAGS  := -O2 -ffreestanding -nostdlib -fvisibility=hidden -mbulk-memory -Wall -Wextra -Wno-unused-parameter -Ilibjit
RVCC    := clang --target=riscv64-unknown-elf -O2 -fno-math-errno -nostdlib -static -fno-builtin -ffreestanding -fuse-ld=lld -Wl,-e,_start
NCC     := cc -O2 -Wall -Wextra -Wno-unused-parameter -Ilibjit

all: $(O)/engine.wasm $(O)/rvjit-native $(O)/xltest guests
guests: $(foreach g,$(GUESTS),$(O)/guest/$(g).elf $(O)/guest/$(g)-gc.elf $(O)/guest/$(g).wasm $(O)/guest/$(g).native) \
        $(foreach g,$(FPGUESTS),$(O)/guest/$(g)-gc.elf $(O)/guest/$(g).wasm $(O)/guest/$(g).native)

$(O)/engine.wasm: $(LIBJIT) tools/jit/sa/port_sa.c lib/mem.s $(HDR)
	@mkdir -p $(O)
	@echo "  JIT   engine.wasm"
	@$(WCC) $(WFLAGS) -Wl,--no-entry -Wl,--export-table -Wl,--growable-table -Wl,-z,stack-size=1048576 -Wl,--stack-first \
	  -Wl,--export=__heap_base -o $@ $(LIBJIT) tools/jit/sa/port_sa.c lib/mem.s

$(O)/rvjit-native: $(LIBJIT) libjit/port_native.c libjit/main.c $(HDR)
	@mkdir -p $(O)
	@echo "  JIT   rvjit-native"
	@$(NCC) -o $@ $(LIBJIT) libjit/port_native.c libjit/main.c

$(O)/xltest: tools/jit/xltest.c libjit/wasm.c libjit/rvdec.c libjit/xlat.c libjit/rvsem.c libjit/rvfp.c libjit/fmt.c libjit/port_native.c $(HDR)
	@mkdir -p $(O)
	@echo "  JIT   xltest"
	@$(NCC) -DXLTEST -o $@ tools/jit/xltest.c libjit/wasm.c libjit/rvdec.c libjit/xlat.c libjit/rvsem.c libjit/rvfp.c libjit/fmt.c libjit/port_native.c

$(O)/guest/%.elf: tools/jit/guest/%.c tools/jit/guest/rt.h
	@mkdir -p $(O)/guest
	@$(RVCC) -march=rv64im -mabi=lp64 -o $@ $<
$(O)/guest/%-gc.elf: tools/jit/guest/%.c tools/jit/guest/rt.h
	@mkdir -p $(O)/guest
	@$(RVCC) -march=rv64gc -mabi=lp64d -o $@ $<
$(O)/guest/%.wasm: tools/jit/guest/%.c tools/jit/guest/rt.h
	@mkdir -p $(O)/guest
	@$(WCC) -O2 -fno-math-errno -nostdlib -fno-builtin -ffreestanding -Wl,--no-entry -Wl,--export-dynamic -o $@ $<
$(O)/guest/%.native: tools/jit/guest/%.c tools/jit/guest/rt.h
	@mkdir -p $(O)/guest
	@cc -O2 -fno-math-errno -o $@ $< -lm

.PHONY: all guests

# UC.OS integration: the normal image with rvjit, plus the test programs in /usr/share/jit,
# and a web folder whose jit-proto/jit.html boots it with the translator extension.
UCOS_PROGS ?= init sh ls cat echo true rvjit
ucos: guests
	@$(MAKE) --no-print-directory B=$(B) PROGS="$(UCOS_PROGS)" all
	@mkdir -p $(O)/root/usr/share/jit $(O)/dist/jit-proto $(O)/dist/worker-proto $(O)/dist/jit/guest
	@cp -r $(B)/root/. $(O)/root/
	@cp $(O)/guest/*.elf $(O)/root/usr/share/jit/
	@if [ -f $(O)/lx.wasm ]; then cp $(O)/lx.wasm $(O)/root/bin/lx; fi
	@tar --format=ustar --owner=0 --group=0 -C $(O)/root -cf $(O)/initrd.tar .
	@cp boot/index.html boot/page.mjs boot/host.mjs boot/keys.mjs $(B)/kernel.wasm $(O)/dist/
	@cp $(O)/initrd.tar $(O)/dist/initrd.tar
	@cp boot/jit-proto/*.mjs boot/jit-proto/*.html $(O)/dist/jit-proto/
	@cp boot/worker-proto/wasm.mjs $(O)/dist/worker-proto/
	@echo "  JIT   $(O)/initrd.tar, $(O)/dist"
.PHONY: ucos

# Case (2) of the comparison: the core engine's interpreter (branch feat/linux-binaries,
# worktree .worktrees/lx). Its sources are copied (read-only use) into $(O)/lx-src at a known
# moment, then built natively and as a UC.OS program against this tree's libc.
LXSRC ?= ../lx/bin/lx
lx-snapshot:
	@mkdir -p $(O)/lx-src
	@cp $(LXSRC)/*.c $(LXSRC)/*.h $(LXSRC)/*.inc $(O)/lx-src/
	@echo "  JIT   copied $(LXSRC) to $(O)/lx-src"
$(O)/lx-native: $(wildcard $(O)/lx-src/*.c $(O)/lx-src/*.h)
	@cc -O2 -ffp-contract=off -fno-math-errno -w -o $@ $(O)/lx-src/*.c
$(O)/lx.wasm: $(wildcard $(O)/lx-src/*.c $(O)/lx-src/*.h) $(B)/libc.a
	@$(WCC) -O2 -ffreestanding -nostdlib -fvisibility=hidden -ffp-contract=off -fno-math-errno -w -Ilibc/include -Wl,--entry=_start \
	  -Wl,-z,stack-size=1048576 -Wl,--stack-first -o $@ $(B)/obj/crt0_3.o $(filter-out %/host_linux.c,$(wildcard $(O)/lx-src/*.c)) $(B)/libc.a
.PHONY: lx-snapshot
