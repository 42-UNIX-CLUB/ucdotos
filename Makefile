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
UCFLAGS := $(CFLAGS) -Ilibc/include -Ilibgfx -Ilibui -Ilibarc
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
# A program directory containing a file named USE_ARC links libarc (gzip, xz,
# zstd, tar, ar, checksums). A file named ALIASES lists more names (one per
# line) under which the same program is installed in /bin, as gunzip for gzip.
ARC_SRC     := $(wildcard libarc/*.c)
ARC_HDR     := $(wildcard libarc/*.h)
# A program directory containing a file named USE_PKG links libpkg (the package
# manager's logic) and libarc (archives and compression).
PKG_SRC     := $(wildcard libpkg/*.c)
# libsig verifies OpenPGP signatures (apt's InRelease); USE_PKG links it too.
SIG_SRC     := $(wildcard libsig/*.c)
SIG_HDR     := $(wildcard libsig/*.h)
# Archive keyrings for apt: the *.gpg, *.asc and *.pgp files in this directory
# are copied to /debian/etc/apt/trusted.gpg.d/ in the image (the default
# keyrings of apt's root). keyrings/ holds Debian's trixie archive keys, each
# fingerprint compared with Debian's published list (libpkg/tests/keyrings-check.sh).
# KEYRINGS=/some/dir picks another directory for a local build.
KEYRINGS    ?= keyrings
KEYRING_FILES := $(wildcard $(KEYRINGS)/*.gpg $(KEYRINGS)/*.asc $(KEYRINGS)/*.pgp)

all: $(B)/kernel.wasm $(B)/initrd.tar

$(B)/kernel.wasm: $(KSRC) kernel/kernel.h abi/abi.h
	@mkdir -p $(B)
	@echo "  KERNEL"
	@$(CC) $(CFLAGS) $(KLDFLAGS) -o $@ $(KSRC)

$(B)/obj/%.o: %.c $(LIBC_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -c -o $@ $<
# The libc's memcpy/memmove/memset come from lib/umem.s (the kernel links lib/mem.s). The
# object keeps the name mem.o so that it replaces the old member in an existing libc.a.
$(B)/obj/lib/mem.o: lib/umem.s
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
$(B)/obj/libarc/%.o: libarc/%.c $(LIBC_HDR) $(ARC_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -c -o $@ $<
$(B)/libarc.a: $(patsubst %.c,$(B)/obj/%.o,$(ARC_SRC))
	@echo "  AR    libarc.a"
	@$(AR) rcs $@ $^
$(patsubst %.c,$(B)/obj/%.o,$(PKG_SRC)): libpkg/pkg.h $(ARC_HDR) $(SIG_HDR)
$(B)/libpkg.a: $(patsubst %.c,$(B)/obj/%.o,$(PKG_SRC))
	@echo "  AR    libpkg.a"
	@$(AR) rcs $@ $^
$(patsubst %.c,$(B)/obj/%.o,$(SIG_SRC)): $(SIG_HDR) $(ARC_HDR)
$(B)/libsig.a: $(patsubst %.c,$(B)/obj/%.o,$(SIG_SRC))
	@echo "  AR    libsig.a"
	@$(AR) rcs $@ $^

# Link with the crt0 that matches main's signature: try (argc, argv) first, then (argc, argv, envp).
gfx_lib = $(if $(wildcard bin/$(1)/USE_GFX),$(B)/libui.a $(B)/libgfx.a)
pkg_lib = $(if $(wildcard bin/$(1)/USE_PKG),$(B)/libpkg.a $(B)/libsig.a)
arc_lib = $(if $(wildcard bin/$(1)/USE_ARC bin/$(1)/USE_PKG),$(B)/libarc.a)
arc_hdr = $(if $(wildcard bin/$(1)/USE_ARC bin/$(1)/USE_PKG),$(ARC_HDR))
define LINK
	@mkdir -p $(B)/bin
	@echo "  LINK  $(1)"
	@$(CC) $(UCFLAGS) $(3) $(ULDFLAGS) -o $(B)/bin/$(1) $(B)/obj/crt0_2.o $(2) $(call pkg_lib,$(1)) $(call arc_lib,$(1)) $(call gfx_lib,$(1)) $(B)/libc.a 2>/dev/null || \
	 $(CC) $(UCFLAGS) $(3) $(ULDFLAGS) -o $(B)/bin/$(1) $(B)/obj/crt0_3.o $(2) $(call pkg_lib,$(1)) $(call arc_lib,$(1)) $(call gfx_lib,$(1)) $(B)/libc.a
endef
define SINGLE_RULE
$(B)/bin/$(1): bin/$(1).c $(B)/libc.a $(CRT) $(call gfx_lib,$(1))
	$(call LINK,$(1),bin/$(1).c,)
endef
define DIR_RULE
$(B)/bin/$(1): $(wildcard bin/$(1)/*.c bin/$(1)/*.h bin/$(1)/CFLAGS) $(B)/libc.a $(CRT) $(call gfx_lib,$(1)) $(call pkg_lib,$(1)) $(call arc_lib,$(1)) $(call arc_hdr,$(1))
	$(call LINK,$(1),$(wildcard bin/$(1)/*.c),$(shell cat bin/$(1)/CFLAGS 2>/dev/null))
endef
$(foreach p,$(PROG_SINGLE),$(eval $(call SINGLE_RULE,$(p))))
$(foreach p,$(PROG_DIRS),$(eval $(call DIR_RULE,$(p))))

# What the C compiler needs inside the image, when cc is one of the programs: the libc
# headers in /usr/include (sys/syscall.h finds abi.h as <ucos/abi.h> there), libui and
# libgfx headers next to them, cc's own freestanding headers in /usr/lib/cc/include,
# and the start files and libraries in /usr/lib.
CC_SYSROOT := $(if $(filter cc,$(PROGS)),$(B)/cc-sysroot.stamp)
$(B)/cc-sysroot.stamp: $(LIBC_HDR) $(wildcard bin/cc/include/*.h libui/*.h libgfx/*.h) $(B)/libc.a $(B)/libui.a $(B)/libgfx.a $(CRT)
	@mkdir -p $(B)/root/usr/include/sys $(B)/root/usr/include/ucos $(B)/root/usr/lib/cc/include
	@cp libc/include/*.h libui/*.h libgfx/*.h $(B)/root/usr/include/
	@cp libc/include/sys/*.h $(B)/root/usr/include/sys/
	@sed 's|"../../../abi/abi.h"|<ucos/abi.h>|' libc/include/sys/syscall.h > $(B)/root/usr/include/sys/syscall.h
	@cp abi/abi.h $(B)/root/usr/include/ucos/abi.h
	@cp bin/cc/include/*.h $(B)/root/usr/lib/cc/include/
	@cp $(B)/libc.a $(B)/libui.a $(B)/libgfx.a $(CRT) $(B)/root/usr/lib/
	@echo "  SYSROOT /usr/include /usr/lib"
	@touch $@

# Remote files (kernel/remote.c): $(B)/files/ beside the image, and the manifest
# /etc/remote-files inside it. Doom's game data comes from $(WADS) when present.
# The script runs on every build (it is quick when nothing changed) and rewrites the
# manifest only when its contents change, so a different WADS= is picked up.
WADS    := bin/doom/wads
$(B)/root/etc/remote-files: FORCE
	@sh tools/remote-files.sh $(B) $(WADS)
FORCE:

# initrd: rootfs/ plus the programs, as a plain ustar archive
# Alias names (bin/NAME/ALIASES, one per line): each is a small program,
# tools/alias/stub.c built with TARGET=/bin/NAME, that starts NAME with the same
# argv[0] and arguments (the kernel has no links yet; with links they can become links).
ALIASED := $(foreach p,$(PROGS),$(if $(wildcard bin/$(p)/ALIASES),$(p)))
$(B)/alias/%: tools/alias/stub.c $(B)/libc.a $(CRT)
	@mkdir -p $(dir $@)
	@echo "  ALIAS $*"
	@$(CC) $(UCFLAGS) $(ULDFLAGS) -DTARGET='"/bin/$*"' -o $@ $(B)/obj/crt0_3.o $< $(B)/libc.a

$(B)/initrd.tar: $(addprefix $(B)/bin/,$(PROGS)) $(shell find rootfs -type f 2>/dev/null) $(CC_SYSROOT) $(B)/root/etc/remote-files $(wildcard bin/*/ALIASES) $(addprefix $(B)/alias/,$(ALIASED)) $(KEYRING_FILES)
	@mkdir -p $(B)/root/bin
	@cp -r rootfs/. $(B)/root/
	@if [ -n "$(KEYRING_FILES)" ]; then mkdir -p $(B)/root/debian/etc/apt/trusted.gpg.d && cp $(KEYRING_FILES) $(B)/root/debian/etc/apt/trusted.gpg.d/ && echo "  KEYS  $(notdir $(KEYRING_FILES))"; fi
	@cp $(addprefix $(B)/bin/,$(PROGS)) $(B)/root/bin/
	@for p in $(ALIASED); do for a in `cat bin/$$p/ALIASES`; do cp $(B)/alias/$$p $(B)/root/bin/$$a; done; done
	@echo "  TAR   initrd.tar"
	@tar --format=ustar --owner=0 --group=0 -C $(B)/root -cf $@ .

# Run the kernel, C library, shell, sound, network and router suites against this build. Fails if any check fails.
test: all
	@sh tools/test.sh $(B)

.PHONY: all test FORCE

# The image compressed for the page (it unpacks it with the browser's DecompressionStream).
# The Node tools keep reading the plain initrd.tar.
$(B)/initrd.tar.gz: $(B)/initrd.tar
	@echo "  GZIP  initrd.tar.gz"
	@gzip -9 -n -c $< > $@

# A directory ready to be served by any static web server.
dist: all $(B)/initrd.tar.gz
	@mkdir -p $(B)/dist
	@cp boot/index.html boot/page.mjs boot/host.mjs boot/audio.mjs boot/audio-worklet.js boot/hostnet.mjs boot/keys.mjs $(B)/kernel.wasm $(B)/initrd.tar $(B)/initrd.tar.gz $(B)/dist/
	@cp -r $(B)/files $(B)/dist/
	@echo "  DIST  $(B)/dist"
.PHONY: dist
