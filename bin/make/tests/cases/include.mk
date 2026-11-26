##### include
include reads another makefile.
--- Makefile
include a.mk b.mk
all: ; @echo $(A) $(B)
--- file a.mk
A = from-a
--- file b.mk
B = from-b
--- run
make

##### include-glob
include with a wildcard.
--- Makefile
include parts/*.mk
all: ; @echo $(sort $(PARTS))
--- file parts/one.mk
PARTS += one
--- file parts/two.mk
PARTS += two
--- run
make

##### include-missing
A missing include is an error.
--- Makefile
include nope.mk
all: ; @echo all
--- run
make; echo "status $?"

##### dash-include
-include and sinclude ignore missing files.
--- Makefile
-include nope.mk
sinclude nope2.mk
all: ; @echo all
--- run
make

##### include-dir
-I finds included makefiles.
--- Makefile
include found.mk
all: ; @echo $(F)
--- file incdir/found.mk
F = found
--- run
make -I incdir
make --include-dir=incdir
make; echo "status $?"

##### include-remade
An included makefile that can be made is made, then make starts again.
--- Makefile
include gen.mk
all: ; @echo "V=$(V)"
gen.mk: ; @echo making gen.mk; echo 'V = generated' > gen.mk
--- run
make
make

##### include-remade-dash
-include of a file with a rule.
--- Makefile
-include gen.mk
all: ; @echo "V=$(V) restarts=$(MAKE_RESTARTS)"
gen.mk: ; @echo 'V = generated' > gen.mk
--- run
make

##### include-out-of-date
An included makefile older than its prerequisite is remade.
--- Makefile
include dep.mk
all: ; @echo "V=$(V)"
dep.mk: dep.src ; @echo regen; sed 's/^/V = /' dep.src > dep.mk
--- file dep.mk
V = old
--- run
touch -d '2001-01-01' dep.mk
echo new > dep.src
make
make

##### makefile-remade-itself
The makefile itself has a rule.
--- Makefile
all: ; @echo all
Makefile: ; @echo checking makefile
--- run
make

##### include-dependency-files
The -MD style: dependency files included with -include.
--- Makefile
OBJS = a.o
all: $(OBJS) ; @echo all done
%.o: %.c ; @echo compile $<; echo '$@: $< $(basename $<).h' > $(basename $@).d; touch $@
-include $(OBJS:.o=.d)
--- file a.c
--- file a.h
--- run
make
make
touch a.h
make

##### include-no-rule
A missing include with no rule.
--- Makefile
all: ; @echo all
include missing.mk
--- run
make; echo "status $?"

##### include-rule-fails
An include whose recipe fails.
--- Makefile
include bad.mk
all: ; @echo all
bad.mk: ; @echo trying; exit 1
--- run
make; echo "status $?"

##### include-n
Makefiles are remade even with -n.
--- Makefile
include gen.mk
all: ; @echo "V=$(V)"
gen.mk: ; echo 'V = gen' > gen.mk
--- run
make -n

##### makefiles-env
MAKEFILES from the environment.
--- Makefile
all: ; @echo "[$(M)]"
--- file extra.mk
M = from-makefiles
first-in-extra: ; @echo not default
--- run
MAKEFILES=extra.mk make

##### eval-include
include inside $(eval).
--- Makefile
$(eval include a.mk)
all: ; @echo $(A)
--- file a.mk
A = a
--- run
make

##### include-twice
The same file included twice.
--- Makefile
include a.mk
include a.mk
all: ; @echo $(N)
--- file a.mk
N += x
--- run
make

##### included-default-goal
The first rule in an included file can be the default goal.
--- Makefile
include rules.mk
all: ; @echo all
--- file rules.mk
inc: ; @echo inc
--- run
make
