# SPDX-License-Identifier: GPL-2.0-or-later
# Build: kernel.wasm, the user C library, every program in bin/, and the initrd.
# Toolchain: clang + wasm-ld only. No Emscripten, no external libc.
CC      := clang --target=wasm32-unknown-unknown
AR      := llvm-ar
WARN    := -Wall -Wextra -Wno-unused-parameter
CFLAGS  := -O2 -ffreestanding -nostdlib -fvisibility=hidden $(WARN)
B       := build

KSRC    := $(wildcard kernel/*.c) lib/string.c lib/fmt.c lib/mem.s
KLDFLAGS := -Wl,--no-entry -Wl,--export-table -Wl,--growable-table -Wl,-z,stack-size=262144 -Wl,--stack-first

LIBC_SRC := $(wildcard libc/src/*.c) lib/string.c lib/fmt.c
LIBC_OBJ := $(patsubst %.c,$(B)/obj/%.o,$(LIBC_SRC)) $(B)/obj/lib/mem.o
LIBC_HDR := $(wildcard libc/include/*.h libc/include/sys/*.h) abi/abi.h
UCFLAGS := $(CFLAGS) -Ilibc/include -Ilibgfx -Ilibui
ULDFLAGS := -Wl,--entry=_start -Wl,-z,stack-size=1048576 -Wl,--stack-first -Wl,--fatal-warnings
CRT     := $(B)/obj/crt0_2.o $(B)/obj/crt0_3.o

# A program is bin/NAME.c (single file) or bin/NAME/*.c (a directory; an optional
# bin/NAME/CFLAGS file adds compiler flags).
PROG_SINGLE := $(patsubst bin/%.c,%,$(wildcard bin/*.c))
PROG_DIRS   := $(patsubst bin/%/,%,$(filter %/,$(wildcard bin/*/)))
PROGS       := $(PROG_SINGLE) $(PROG_DIRS)
# A program directory containing a file named USE_GFX links libui (windows, themes)
# and libgfx (font rasterizer + 2D drawing).
UI_SRC      := $(wildcard libui/*.c)
GFX_SRC     := $(wildcard libgfx/*.c)

all: $(B)/kernel.wasm $(B)/initrd.tar

$(B)/kernel.wasm: $(KSRC) kernel/kernel.h abi/abi.h
	@mkdir -p $(B)
	@echo "  KERNEL"
	@$(CC) $(CFLAGS) $(KLDFLAGS) -o $@ $(KSRC)

$(B)/obj/%.o: %.c $(LIBC_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -c -o $@ $<
$(B)/obj/lib/mem.o: lib/mem.s
	@mkdir -p $(dir $@)
	@$(CC) -c -o $@ $<
$(B)/obj/crt0_2.o: libc/crt/crt0.c $(LIBC_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -c -o $@ $<
$(B)/obj/crt0_3.o: libc/crt/crt0.c $(LIBC_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -DMAIN3 -c -o $@ $<
$(B)/libc.a: $(LIBC_OBJ)
	@echo "  AR    libc.a"
	@$(AR) rcs $@ $^
$(B)/libui.a: $(patsubst %.c,$(B)/obj/%.o,$(UI_SRC))
	@echo "  AR    libui.a"
	@$(AR) rcs $@ $^
$(B)/libgfx.a: $(patsubst %.c,$(B)/obj/%.o,$(GFX_SRC))
	@echo "  AR    libgfx.a"
	@$(AR) rcs $@ $^

# Link with the crt0 that matches main's signature: try (argc, argv) first, then (argc, argv, envp).
gfx_lib = $(if $(wildcard bin/$(1)/USE_GFX),$(B)/libui.a $(B)/libgfx.a)
define LINK
	@mkdir -p $(B)/bin
	@echo "  LINK  $(1)"
	@$(CC) $(UCFLAGS) $(3) $(ULDFLAGS) -o $(B)/bin/$(1) $(B)/obj/crt0_2.o $(2) $(call gfx_lib,$(1)) $(B)/libc.a 2>/dev/null || \
	 $(CC) $(UCFLAGS) $(3) $(ULDFLAGS) -o $(B)/bin/$(1) $(B)/obj/crt0_3.o $(2) $(call gfx_lib,$(1)) $(B)/libc.a
endef
define SINGLE_RULE
$(B)/bin/$(1): bin/$(1).c $(B)/libc.a $(CRT) $(call gfx_lib,$(1))
	$(call LINK,$(1),bin/$(1).c,)
endef
define DIR_RULE
$(B)/bin/$(1): $(wildcard bin/$(1)/*.c bin/$(1)/*.h bin/$(1)/CFLAGS) $(B)/libc.a $(CRT) $(call gfx_lib,$(1))
	$(call LINK,$(1),$(wildcard bin/$(1)/*.c),$(shell cat bin/$(1)/CFLAGS 2>/dev/null))
endef
$(foreach p,$(PROG_SINGLE),$(eval $(call SINGLE_RULE,$(p))))
$(foreach p,$(PROG_DIRS),$(eval $(call DIR_RULE,$(p))))

# initrd: rootfs/ plus the programs, as a plain ustar archive
$(B)/initrd.tar: $(addprefix $(B)/bin/,$(PROGS)) $(shell find rootfs -type f 2>/dev/null)
	@mkdir -p $(B)/root/bin
	@cp -r rootfs/. $(B)/root/
	@cp $(addprefix $(B)/bin/,$(PROGS)) $(B)/root/bin/
	@echo "  TAR   initrd.tar"
	@tar --format=ustar --owner=0 --group=0 -C $(B)/root -cf $@ .

# Run the kernel, C library and shell suites against this build. Fails if any check fails.
test: all
	@sh tools/test.sh $(B)

.PHONY: all test

# A directory ready to be served by any static web server.
dist: all
	@mkdir -p $(B)/dist
	@cp boot/index.html boot/page.mjs boot/host.mjs boot/keys.mjs $(B)/kernel.wasm $(B)/initrd.tar $(B)/dist/
	@echo "  DIST  $(B)/dist"
.PHONY: dist
