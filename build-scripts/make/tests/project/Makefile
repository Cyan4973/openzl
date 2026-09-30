# Copyright (c) Meta Platforms, Inc. and affiliates.

# Minimal project built with multiconf.make, driven by ../run_tests.sh

.PHONY: default
default: all

CFLAGS ?= -O1
CXXFLAGS ?= -O1

MULTICONF ?= ../../multiconf.make
C_SRCDIRS := src
CXX_SRCDIRS := src
include $(MULTICONF)

LIB_OBJS := $(patsubst %.c,%.o,$(filter-out src/main.c,$(wildcard src/*.c)))

$(eval $(call c_program,prog_a,src/main.o $(LIB_OBJS)))
$(eval $(call c_program,prog_b,src/main.o $(LIB_OBJS)))
prog_fast: CFLAGS += -DFAST $(FAST_FLAGS)
$(eval $(call c_program,prog_fast,src/main.o $(LIB_OBJS)))
$(eval $(call cxx_program,prog_cxx,src/main_cpp.o $(LIB_OBJS)))
$(eval $(call static_library,libtest.a,$(LIB_OBJS)))

.PHONY: all
all: prog_a prog_b prog_fast prog_cxx libtest.a

.PHONY: print-depfiles
print-depfiles:
	@echo $(MCM_DEPFILES)
