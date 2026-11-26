define RULE
$(1): ; @echo rule for $(1)
endef
$(foreach t,p q,$(eval $(call RULE,$(t))))
define TWO
@echo first
@echo second
endef
all: p q ; $(TWO)
