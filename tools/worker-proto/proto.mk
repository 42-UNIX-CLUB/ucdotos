# SPDX-License-Identifier: GPL-2.0-or-later
# Worker-process prototype builds (docs/WORKER-PROCESSES.md). Separate from the default
# build on purpose: the default Makefile, libc and programs are not changed. Every variant
# re-runs the default Makefile into its own directory with different CC / ULDFLAGS.
#
#   make B=build-NAME dist                                   # the default build first
#   make -f tools/worker-proto/proto.mk B=build-NAME         # then this
#
# Outputs under $(B)/proto/:
#   shm/  every program built with a shared memory (approach A)
#   mrk/  every program built normally plus the ucos_worker mark (approach B)
#   yld/  every program with yield points at every loop (approach C, clang -S + awk)
#   cov/  every program with -fsanitize-coverage=trace-pc-guard hooks (approach C, variant)
#   pln/  the same programs through the same wrapper with no instrumentation (C's baseline)
#   run/    kernel.wasm + initrd.tar: the default image plus wproto, wproto-a, wproto-b,
#           wproto-y and bc-a, bc-b, bc-y
#   run-a/  image where EVERY program is the approach-A build (plus /bin/wproto, ordinary)
#   run-b/  image where EVERY program is the approach-B build (plus /bin/wproto, ordinary)
#   dist/   run/ as a web folder, with boot/worker-proto/ and the test pages
B       ?= build
P       := $(B)/proto
WASMCC  := clang --target=wasm32-unknown-unknown
UCFLAGS := -O2 -ffreestanding -nostdlib -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Ilibc/include
LDBASE  := -Wl,--entry=_start -Wl,-z,stack-size=1048576 -Wl,--stack-first -Wl,--fatal-warnings
SHMCC   := $(WASMCC) -matomics -mbulk-memory
SHMLD   := $(LDBASE) -Wl,--shared-memory -Wl,--max-memory=1073741824
YIELD_EVERY ?= 65536

all: $(P)/run/initrd.tar $(P)/run-a/initrd.tar $(P)/run-b/initrd.tar $(P)/dist/index.html

# ---- whole-system variants, by re-running the default Makefile ----
$(P)/shm/initrd.tar: FORCE
	@echo "  PROTO shared-memory build (A)"
	@$(MAKE) --no-print-directory B=$(P)/shm CC="$(SHMCC)" ULDFLAGS="$(SHMLD)" $@
$(P)/mark.o: tools/worker-proto/mark.c
	@mkdir -p $(P)
	@$(WASMCC) $(UCFLAGS) -c -o $@ $<
$(P)/mrk/initrd.tar: $(P)/mark.o FORCE
	@echo "  PROTO marked build (B)"
	@$(MAKE) --no-print-directory B=$(P)/mrk ULDFLAGS="$(LDBASE) $(abspath $(P)/mark.o)" $@
$(P)/yrt/yield-rt.o: tools/worker-proto/yield-rt.s
	@mkdir -p $(dir $@)
	@$(WASMCC) -c -o $@ $<
$(P)/yrt/yield-slow.o: tools/worker-proto/yield-slow.c
	@mkdir -p $(dir $@)
	@$(WASMCC) $(UCFLAGS) -DYIELD_EVERY=$(YIELD_EVERY) -c -o $@ $<
YRT = $(abspath $(P)/yrt/yield-rt.o) $(abspath $(P)/yrt/yield-slow.o)
$(P)/yld/initrd.tar $(P)/cov/initrd.tar $(P)/pln/initrd.tar: $(P)/yrt/yield-rt.o $(P)/yrt/yield-slow.o FORCE
	@echo "  PROTO $(notdir $(@D)) build (C)"
	@YIELD_MODE=$(if $(findstring /yld/,$@),loops,$(if $(findstring /cov/,$@),sancov,none)) YIELD_RT="$(YRT)" \
	  $(MAKE) --no-print-directory B=$(@D) CC="sh tools/worker-proto/yield-cc.sh" $@

# ---- the test program, built each way ----
$(P)/bin/wproto: tools/worker-proto/wproto.c $(B)/libc.a
	@mkdir -p $(dir $@)
	@$(WASMCC) $(UCFLAGS) $(LDBASE) -o $@ $(B)/obj/crt0_2.o $< $(B)/libc.a
$(P)/bin/wproto-b: tools/worker-proto/wproto.c $(P)/mark.o $(B)/libc.a
	@mkdir -p $(dir $@)
	@$(WASMCC) $(UCFLAGS) $(LDBASE) -o $@ $(B)/obj/crt0_2.o $< $(P)/mark.o $(B)/libc.a
$(P)/bin/wproto-a: tools/worker-proto/wproto.c $(P)/shm/initrd.tar
	@mkdir -p $(dir $@)
	@$(SHMCC) $(UCFLAGS) $(SHMLD) -o $@ $(P)/shm/obj/crt0_2.o $< $(P)/shm/libc.a
$(P)/bin/wproto-y: tools/worker-proto/wproto.c $(P)/yld/initrd.tar
	@mkdir -p $(dir $@)
	@YIELD_RT="$(YRT)" sh tools/worker-proto/yield-cc.sh $(UCFLAGS) -c -o $(P)/bin/wproto-y.o $<
	@YIELD_RT="$(YRT)" sh tools/worker-proto/yield-cc.sh $(UCFLAGS) $(LDBASE) -o $@ $(P)/yld/obj/crt0_2.o $(P)/bin/wproto-y.o $(P)/yld/libc.a
$(P)/bin/wproto-p: tools/worker-proto/wproto.c $(P)/pln/initrd.tar
	@mkdir -p $(dir $@)
	@YIELD_MODE=none YIELD_RT="$(YRT)" sh tools/worker-proto/yield-cc.sh $(UCFLAGS) $(LDBASE) -o $@ $(P)/pln/obj/crt0_2.o $< $(P)/pln/libc.a

EXTRA = $(P)/bin/wproto $(P)/bin/wproto-b $(P)/bin/wproto-a $(P)/bin/wproto-y $(P)/bin/wproto-p

# ---- images ----
$(P)/run/initrd.tar: $(EXTRA) $(P)/cov/initrd.tar $(B)/initrd.tar tools/worker-proto/wp-tests.sh tools/worker-proto/wp-cpu.sh
	@mkdir -p $(P)/run/root/bin
	@cp -r $(B)/root/. $(P)/run/root/
	@cp $(EXTRA) $(P)/run/root/bin/
	@cp tools/worker-proto/wp-tests.sh tools/worker-proto/wp-cpu.sh $(P)/run/root/usr/share/tests/
	@for v in shm:a mrk:b yld:y cov:c pln:p; do d=$${v%%:*}; s=$${v##*:}; \
	  for prog in bc sort grep; do cp $(P)/$$d/bin/$$prog $(P)/run/root/bin/$$prog-$$s; done; done
	@cp $(B)/kernel.wasm $(P)/run/
	@echo "  TAR   $@"
	@tar --format=ustar --owner=0 --group=0 -C $(P)/run/root -cf $@ .
$(P)/run-a/initrd.tar: $(P)/shm/initrd.tar $(P)/bin/wproto $(P)/bin/wproto-a
	@mkdir -p $(P)/run-a/root/bin
	@cp -r $(P)/shm/root/. $(P)/run-a/root/
	@cp $(P)/bin/wproto $(P)/bin/wproto-a $(P)/run-a/root/bin/
	@cp $(B)/kernel.wasm $(P)/run-a/
	@echo "  TAR   $@"
	@tar --format=ustar --owner=0 --group=0 -C $(P)/run-a/root -cf $@ .
$(P)/run-b/initrd.tar: $(P)/mrk/initrd.tar $(P)/bin/wproto $(P)/bin/wproto-b
	@mkdir -p $(P)/run-b/root/bin
	@cp -r $(P)/mrk/root/. $(P)/run-b/root/
	@cp $(P)/bin/wproto $(P)/bin/wproto-b $(P)/run-b/root/bin/
	@cp $(B)/kernel.wasm $(P)/run-b/
	@echo "  TAR   $@"
	@tar --format=ustar --owner=0 --group=0 -C $(P)/run-b/root -cf $@ .

# ---- web folder: the default page files, the prototype modules, the test pages ----
$(P)/dist/index.html: $(P)/run/initrd.tar $(wildcard boot/*.mjs boot/*.html boot/worker-proto/*)
	@mkdir -p $(P)/dist/worker-proto
	@cp boot/index.html boot/page.mjs boot/host.mjs boot/audio.mjs boot/audio-worklet.js boot/keys.mjs $(P)/run/kernel.wasm $(P)/run/initrd.tar $(P)/dist/
	@cp boot/worker-proto/*.mjs boot/worker-proto/*.js boot/worker-proto/*.html $(P)/dist/worker-proto/
	@echo "  DIST  $(P)/dist"

FORCE:
.PHONY: all FORCE
