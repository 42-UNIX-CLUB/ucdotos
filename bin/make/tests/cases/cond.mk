##### ifeq-forms
All the quoting forms of ifeq.
--- Makefile
A = x
ifeq ($(A),x)
$(info paren yes)
endif
ifeq "$(A)" "x"
$(info double yes)
endif
ifeq '$(A)' 'x'
$(info single yes)
endif
ifeq "$(A)" 'x'
$(info mixed yes)
endif
ifneq ($(A),y)
$(info ifneq yes)
endif
ifeq (,$(B))
$(info empty yes)
endif
ifeq ($(A), x)
$(info space after comma yes)
endif
all: ;
--- run
make -s

##### ifeq-whitespace
Whitespace inside ifeq.
--- Makefile
ifeq ( a , a )
$(info 1)
endif
ifeq (a ,a)
$(info 2)
endif
ifeq (a, a)
$(info 3)
endif
ifeq (a,a )
$(info 4)
endif
ifeq ($(strip a ),a)
$(info 5)
endif
all: ;
--- run
make -s

##### ifdef
ifdef and ifndef.
--- Makefile
A = a
B =
C = $(B)
ifdef A
$(info A defined)
endif
ifdef B
$(info B defined)
endif
ifdef C
$(info C defined)
endif
ifndef NOPE
$(info NOPE undefined)
endif
N = A
ifdef $(N)
$(info computed name)
endif
all: ;
--- run
make -s
make -s NOPE=1

##### else
else and else-if chains.
--- Makefile
X = 2
ifeq ($(X),1)
$(info one)
else ifeq ($(X),2)
$(info two)
else ifeq ($(X),2)
$(info two again)
else
$(info other)
endif
ifdef NOPE
$(info a)
else ifndef NOPE2
$(info b)
endif
all: ;
--- run
make -s
make -s X=1
make -s X=9

##### nested
Nested conditionals.
--- Makefile
A = 1
B = 2
ifeq ($(A),1)
  ifeq ($(B),2)
    $(info both)
  else
    $(info only A)
  endif
else
  ifeq ($(B),2)
    $(info only B)
  endif
endif
ifeq ($(A),9)
  ifeq ($(B),2)
    $(info never)
  else
    $(info never either)
  endif
endif
all: ;
--- run
make -s
make -s A=0

##### skipped-define
A define inside a false branch is skipped whole.
--- Makefile
ifeq (1,2)
define X
endif
endef
endif
$(info still fine)
all: ;
--- run
make -s

##### conditional-in-recipe
Conditionals between recipe lines.
--- Makefile
DEBUG = 1
all:
	@echo start
ifeq ($(DEBUG),1)
	@echo debug on
else
	@echo debug off
endif
	@echo end
--- run
make
make DEBUG=0

##### missing-endif
Errors in conditionals.
--- Makefile
ifeq (a,a)
all: ;
--- run
make; echo "status $?"

##### extra-endif
An endif with no if.
--- Makefile
all: ;
endif
--- run
make; echo "status $?"

##### extra-else
An else with no if.
--- Makefile
else
--- run
make; echo "status $?"

##### double-else
Two else in a row.
--- Makefile
ifeq (a,a)
else
else
endif
--- run
make; echo "status $?"

##### invalid-cond
Bad syntax.
--- Makefile
ifeq a b
endif
--- run
make; echo "status $?"

##### cond-with-comment
Comments on conditional lines.
--- Makefile
ifeq (a,a) # comment
$(info yes)
else # comment
$(info no)
endif # comment
all: ;
--- run
make -s

##### cond-tab-recipe
A tab before ifeq in a recipe makes it a shell line.
--- Makefile
all:
	@echo x; ifeq; true
--- run
make
