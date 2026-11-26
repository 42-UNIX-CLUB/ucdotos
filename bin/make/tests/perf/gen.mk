# SPDX-License-Identifier: GPL-2.0-or-later
# Writes big.mk: N object files built from N sources through a pattern rule,
# a dependency chain of N/10 steps, heavy use of +=, substitution references,
# foreach and target-specific variables. Used to time parsing and no-op builds:
#   make -f gen.mk N=10000          writes big.mk
#   make -f big.mk -t > /dev/null   creates every file, all up to date
#   make -f big.mk parse-only       time to read and evaluate big.mk
#   make -f big.mk                  no-op build: everything is up to date
N ?= 10000
NUMS := $(shell seq 1 $(N))
CHAIN := $(shell seq 1 $$(($(N) / 10)))
# DEEP=1 spreads the sources over a tree of small directories (src/1/2/s123.c),
# which keeps directory sizes small; make mkdirs creates the tree.
DEEP ?= 0
digits = $(subst 9,9/,$(subst 8,8/,$(subst 7,7/,$(subst 6,6/,$(subst 5,5/,$(subst 4,4/,$(subst 3,3/,$(subst 2,2/,$(subst 1,1/,$(subst 0,0/,$(1)))))))))))
srcdir = src/$(if $(filter 1,$(DEEP)),$(subst ./,,$(dir $(patsubst %/,%,$(call digits,$(1))))))
define nl


endef
define HEAD
CFLAGS = -O2 -Wall $$(EXTRA)
EXTRA = -DCOUNT=$$(words $$(OBJS))
SRCS :=
all: prog chain$(lastword $(CHAIN))
parse-only: ; @:
%.o: %.c inc/common.h ; $$(CC) $$(CFLAGS) -c -o $$@ $$<
src/%.c: ; @touch $$@
inc/common.h: ; @touch $$@
chain0: ; @touch $$@
endef
$(file >big.mk,$(HEAD))
$(file >>big.mk,$(foreach i,$(NUMS),SRCS += $(call srcdir,$(i))s$(i).c$(nl)$(if $(filter %0,$(i)),$(call srcdir,$(i))s$(i).o: CFLAGS += -DI=$(i)$(nl))))
$(file >>big.mk,mkdirs: ; @mkdir -p inc $(sort $(foreach i,$(NUMS),$(call srcdir,$(i)))))
$(file >>big.mk,OBJS := $$(SRCS:.c=.o)$(nl)prog: $$(OBJS) ; @echo link $$(words $$^) > $$@)
PREV := 0 $(CHAIN)
$(file >>big.mk,$(foreach i,$(CHAIN),chain$(i): chain$(word $(i),$(PREV)) ; @touch $$@$(nl)))
all: ; @echo wrote big.mk with $(N) objects and a chain of $(words $(CHAIN))
