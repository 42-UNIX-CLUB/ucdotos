X = 1
ifeq ($(X),1)
R = one
else ifeq ($(X),2)
R = two
else
R = other
endif
ifdef X
D = defined
endif
ifndef NOPE
N = undefined
endif
all: ; @echo $(R) $(D) $(N)
