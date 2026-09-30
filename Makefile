# Copyright (c) Meta Platforms, Inc. and affiliates.

# first recipe is default recipe
.PHONY: default
default : zli

# Common repository-wide definitions
include build-scripts/make/zldefs.make

# Provides macros to generate targets
include build-scripts/make/multiconf.make

# =====================================
# ADAPTIVE mode: Per-Target Smart Defaults
# =====================================
# In ADAPTIVE mode, targets add their own appropriate flags
# In coercive modes (OPT, DEV, etc.), these complement are overridden by global settings
ifeq ($(BUILD_TYPE),ADAPTIVE)
    # Production binaries: optimize for performance
    zli: CFLAGS += -g0 -O3
    zli: CXXFLAGS += -g0 -O3
    zli: CPPFLAGS += -DNDEBUG

    libopenzl.a: CFLAGS += -g0 -O3
    libopenzl.so: CFLAGS += -g0 -O3
    libopenzl.a: CPPFLAGS += -DNDEBUG
    libopenzl.so: CPPFLAGS += -DNDEBUG

    # Benchmarks: optimize for representative performance
    unitBench: CFLAGS += -g0 -O3
    unitBench: CPPFLAGS += -DNDEBUG

    # Test programs: enable asserts for correctness checking
    gtests: CFLAGS += -g
    gtests: CXXFLAGS += -g
    gtests: CPPFLAGS += -DZL_ENABLE_ASSERT
endif

# dependencies
ifneq (,$(filter Windows%,$(OS)))
LIBZSTD_SO := deps/zstd/lib/dll/libzstd.dll
LIBLZ4_SO := deps/lz4/lib/liblz4.dll
LIBXGBOOST_SO := deps/xgboost/lib/libxgboost.dll
else ifeq ($(shell uname), Darwin)
LIBZSTD_SO := deps/zstd/lib/libzstd.dylib
LIBLZ4_SO := deps/lz4/lib/liblz4.dylib
LIBXGBOOST_SO := deps/xgboost/lib/libxgboost.dylib
else
LIBZSTD_SO := deps/zstd/lib/libzstd.so
LIBLZ4_SO := deps/lz4/lib/liblz4.so
LIBXGBOOST_SO := deps/xgboost/lib/libxgboost.so
endif

LIBZSTD_A := deps/zstd/lib/libzstd.a
LIBLZ4_A := deps/lz4/lib/liblz4.a
LIBGTEST_A := deps/googletest/lib/libgtest.a
LIBXGBOOST_A := deps/xgboost/lib/libxgboost.a
LIBDMLC_A := deps/xgboost/lib/libdmlc.a

ZSTD_HEADER := deps/zstd/lib/zstd.h
LZ4_HEADER := deps/lz4/lib/lz4.h
GTEST_HEADERS := deps/googletest/googletest/include/gtest/gtest.h
XGBOOST_HEADER := deps/xgboost/include/xgboost/c_api.h

# Set EXEC_PREFIX to prefix every build output that is run in tests.
# E.g.qemu
EXEC_PREFIX ?=

# Source directories of each component
LIBROOT := src
LIBDIRS := $(shell find $(LIBROOT) -type d)
TESTROOT:= tests
TESTSDIRS:= $(shell find $(TESTROOT) -type d)
CLIDIR  := cli
CLIDIRS := $(CLIDIR) $(CLIDIR)/args $(CLIDIR)/commands $(CLIDIR)/utils
CLI_TEST_DIRS := $(CLIDIR)/tests $(CLIDIR)/tests/unittest $(CLIDIR)/training/tests
ARGDIR  := tools/arg
IODIR := tools/io
LOGGERDIR := tools/logger
STREAMDUMPDIR := tools/streamdump
UNITBENCH_ROOT:= benchmark/unitBench
UNITBENCH_DIRS:= $(shell find $(UNITBENCH_ROOT) -type d)
CUSTOMPARSERSDIR := custom_parsers
CSVDIR := $(CUSTOMPARSERSDIR)/csv
PROFILES_SDDL_DIR := $(CUSTOMPARSERSDIR)/sddl
PARQUETDIR := $(CUSTOMPARSERSDIR)/parquet
SHARED_COMPONENTSDIR := $(CUSTOMPARSERSDIR)/shared_components
VISUALIZER_CPPDIR := tools/zl_visualizer/compression_introspection
ZLCPP_ROOT := cpp/src
ZLCPP_DIRS := $(shell find $(ZLCPP_ROOT) -type d)
ZLCPP_TEST_DIR := cpp/tests
TRAINING_ROOTS := tools/training/clustering tools/training/ace tools/training/graph_mutation tools/training/utils tools/training/sample_collection tools/training/dict tools/training/lz
TRAINING_DIRS := tools/training $(shell find $(TRAINING_ROOTS) -type d)
TRAINING_TEST_DIRS := $(shell find tools/training/tests -type d)
SDDL_COMPILER_DIR := tools/sddl/compiler
SDDL2_COMPILER_DIR := tools/sddl2/compiler
SDDL2_COMPILER_DIRS := $(filter-out %/tests,$(shell find $(SDDL2_COMPILER_DIR) -type d))
SDDL2_ASSEMBLER_DIR := tools/sddl2/assembler
ML_SELECTOR_DIR := tools/ml_selector
ML_SELECTOR_TESTS_DIR := $(ML_SELECTOR_DIR)/tests
TEST_REGISTRY_DIRS := tests/registry tests/registry/components


# =====================================
# library
# =====================================

LIBCSRCS := $(wildcard $(addsuffix /*.c, $(LIBDIRS)))
LIBASMSRCS := $(wildcard $(addsuffix /*.S, $(LIBDIRS)))
LIBOBJS := $(patsubst %.c,%.o,$(LIBCSRCS)) $(patsubst %.S,%.o,$(LIBASMSRCS))

libopenzl.a:
$(eval $(call static_library,libopenzl.a,$(LIBOBJS),$(LIBZSTD_A) $(LIBLZ4_A)))

libopenzl.so: CFLAGS += -fPIC
$(eval $(call c_dynamic_library,libopenzl.so,$(LIBOBJS),$(LIBZSTD_SO) $(LIBLZ4_SO)))

.PHONY:lib
lib: libopenzl.a libopenzl.so

# =====================================
# Targets
# =====================================

.PHONY: all
all : lib gtests unitBench zli sddl_compiler stream_dump2 examples

# Define a function to generate a list of C++ object files from directory
cxx_objs = $(patsubst %.cpp,%.o,$(wildcard $(addsuffix /*.cpp, $(1))))
c_objs = $(patsubst %.c,%.o,$(wildcard $(addsuffix /*.c, $(1))))

ZLCPP_OBJS := $(call cxx_objs,$(ZLCPP_DIRS))
CLI_CXXOBJS := $(filter-out %zli.o,$(foreach DIR,$(CLIDIRS),$(call cxx_objs,$(DIR))))
ARG_CXXOBJS := $(call cxx_objs,$(ARGDIR))
LOGGER_CXXOBJS := $(call cxx_objs,$(LOGGERDIR))
STREAMDUMP_COBJS := $(call c_objs,$(STREAMDUMPDIR))
CUSTOM_PARSERS_COBJS := $(call c_objs,$(CUSTOMPARSERSDIR))
CUSTOM_PARSERS_CXXOBJS := $(call cxx_objs,$(CUSTOMPARSERSDIR))
# Exclude pytorch_model_compressor.cpp because it depends on Folly.
CUSTOM_PARSERS_CXXOBJS := $(filter-out %/pytorch_model_compressor.o,$(CUSTOM_PARSERS_CXXOBJS))
SHARED_COMPONENTS_CXXOBJS := $(call cxx_objs,$(SHARED_COMPONENTSDIR))
CSV_CXXOBJS := $(call cxx_objs,$(CSVDIR))
CSV_COBJS := $(call c_objs,$(CSVDIR))
PARQUET_CXXOBJS := $(call cxx_objs,$(PARQUETDIR))
PARQUET_COBJS := $(call c_objs,$(PARQUETDIR))
PROFILES_SDDL_COBJS := $(call c_objs,$(PROFILES_SDDL_DIR))
IO_CXXOBJS := $(call cxx_objs,$(IODIR))
VISUALIZER_CXXOBJS := $(call cxx_objs,$(VISUALIZER_CPPDIR))
TRAINING_CXXOBJS := $(call cxx_objs,$(TRAINING_DIRS))
TRAINING_TEST_CXXOBJS := $(call cxx_objs,$(TRAINING_TEST_DIRS))
SDDL_COMPILER_CXXOBJS := $(filter-out %main.o, $(call cxx_objs,$(SDDL_COMPILER_DIR)))
SDDL2_COMPILER_CXXOBJS := $(filter-out %main.o, $(call cxx_objs,$(SDDL2_COMPILER_DIRS)))
SDDL2_ASSEMBLER_CXXOBJS :=  $(filter-out %main.o, $(call cxx_objs,$(SDDL2_ASSEMBLER_DIR)))
ML_SELECTOR_COBJS := $(call c_objs,$(ML_SELECTOR_DIR))
ML_SELECTOR_CXXOBJS := $(call cxx_objs,$(ML_SELECTOR_DIR))

ML_SELECTOR_CPPFLAGS := -Ideps/xgboost/include -Ideps/xgboost/dmlc-core/include -DDMLC_LOG_STACK_TRACE=0 -DOPENZL_HAS_ML_SELECTOR_TRAINER=1

# Add flags for cross platform compatibility for Windows
zli: LDFLAGS += $(XGBOOST_LDFLAGS)
zli: CPPFLAGS += $(ML_SELECTOR_CPPFLAGS) -DZDICT_STATIC_LINKING_ONLY
zli: LDLIBS += $(XGBOOST_LDLIBS)

gtests: LDFLAGS += $(XGBOOST_LDFLAGS)
gtests: CPPFLAGS += $(ML_SELECTOR_CPPFLAGS) -DZDICT_STATIC_LINKING_ONLY
gtests: LDLIBS += $(XGBOOST_LDLIBS)

$(eval $(call cxx_program,zli, \
	cli/zli.o \
	$(CLI_CXXOBJS) \
	$(ARG_CXXOBJS) \
	$(LOGGER_CXXOBJS) \
	$(CUSTOM_PARSERS_COBJS) \
	$(CUSTOM_PARSERS_CXXOBJS) \
	$(SHARED_COMPONENTS_CXXOBJS) \
	$(CSV_COBJS) \
	$(CSV_CXXOBJS) \
	$(PROFILES_SDDL_COBJS) \
	$(PARQUET_COBJS) \
	$(PARQUET_CXXOBJS) \
	$(VISUALIZER_CXXOBJS) \
	$(IO_CXXOBJS) \
	$(TRAINING_CXXOBJS) \
	$(SDDL_COMPILER_CXXOBJS) \
	$(SDDL2_COMPILER_CXXOBJS) \
 	$(SDDL2_ASSEMBLER_CXXOBJS) \
	$(ML_SELECTOR_COBJS) \
	$(ML_SELECTOR_CXXOBJS) \
	$(ZLCPP_OBJS) \
	$(LIBOBJS), \
	$(LIBZSTD_A) $(LIBLZ4_A) $(LIBXGBOOST_A) $(LIBDMLC_A)))

.PHONY: examples
examples: zs2_pipeline zs2_trygraph zs2_selector zs2_struct zs2_round_trip

.PHONY: test
test : gtests test-zs2 test-cli
	$(EXEC_PREFIX) ./gtests

# Python bindings for openzl.ext module (required for ML tests)
.PHONY: python-bindings
python-bindings:
	@echo "Building and installing openzl Python bindings..."
	pip install --quiet py/

.PHONY: test-cli
test-cli: zli
	cd cli/tests && python3 cli_integration_tests.py ../../zli

.PHONY: test-train
test-train: zli
	cd cli/tests && python3 cli_train_tests.py ../../zli

.PHONY: test-formats
test-formats: zli
	cd cli/tests && python3 cli_formats_tests.py ../../zli

.PHONY: check-python-format
check-python-format:
	@./scripts/check_python_format.sh

.PHONY: fix-python-format
fix-python-format:
	@./scripts/check_python_format.sh --fix

.PHONY: test-zs2
test-zs2 : examples
	$(EXEC_PREFIX) ./zs2_pipeline
	$(EXEC_PREFIX) ./zs2_trygraph

# ********     Tools     ********

UNITBENCH_COBJS := $(foreach DIR,$(UNITBENCH_DIRS),$(call c_objs,$(DIR)))
$(eval $(call c_program_shared_o,unitBench,tools/time/timefn.o tools/fileio/fileio.o $(UNITBENCH_COBJS) $(LIBOBJS),$(LIBZSTD_A) $(LIBLZ4_A)))

stream_dump2:
$(eval $(call c_program_shared_o,stream_dump2, \
    $(STREAMDUMP_COBJS) tools/fileio/fileio.o $(LIBOBJS),$(LIBZSTD_A) $(LIBLZ4_A)))

$(eval $(call cxx_program,sddl_compiler, \
	$(SDDL_COMPILER_DIR)/main.o \
	$(SDDL_COMPILER_CXXOBJS) \
	$(ZLCPP_OBJS) \
	$(LIBOBJS), \
	$(LIBZSTD_A) $(LIBLZ4_A)))

# Selection of gtest units (by file name convention)
CXX_FILE_OBJS := $(notdir $(CXX_OBJS))
GTEST_FILEO := $(filter test_%.o,$(CXX_FILE_OBJS))
GTEST_FILEO += $(filter %Test.o,$(CXX_FILE_OBJS))
GTEST_FILTER_LIST := VersionTest.o NoIntrospectionTest.o
GTEST_FILEO := $(filter-out $(GTEST_FILTER_LIST),$(GTEST_FILEO))

ALL_TEST_OBJS := $(patsubst %.cpp,%.o,$(foreach dir,$(TESTSDIRS) $(CLI_TEST_DIRS) $(ML_SELECTOR_TESTS_DIR),$(wildcard $(dir)/*.cpp)))
GTEST_OBJS := $(foreach name,$(GTEST_FILEO),$(filter %/$(name),$(ALL_TEST_OBJS)))

# Other module objects used in gtests
DATAGEN_OBJS := \
	tests/datagen/structures/CompressorProducer.o \
	tests/datagen/structures/LocalParamsProducer.o \
	tests/datagen/structures/openzl/StringInputProducer.o \
	tests/datagen/InputExpander.o
SERIALIZATION_TEST_OBJS := \
	tests/serialization/GraphBuilder.o \
	tests/serialization/GraphBuilderUtils.o
TEST_REGISTRY_SRCS = $(wildcard $(addsuffix /*.cpp, $(TEST_REGISTRY_DIRS)))
TEST_REGISTRY_OBJS = $(patsubst %.cpp,%.o,$(TEST_REGISTRY_SRCS))
ZLCPP_TEST_OBJS := $(call cxx_objs,$(ZLCPP_TEST_DIR))

ALL_GTESTS_OBJS := \
	tests/gtest_main.o \
	tools/time/timefn.o \
	tests/utils.o \
	tests/local_params_utils.o \
	tests/ml_selector_utils.o \
	tests/unittest/common/test_errors_in_c.o \
	tests/unittest/common/sha_vec.o \
	tests/compress/ml_selectors/test_zstrong_ml_core_models.o \
	$(GTEST_OBJS) \
	$(ZLCPP_TEST_OBJS) \
	$(CLI_CXXOBJS) \
	$(ARG_CXXOBJS) \
	$(LOGGER_CXXOBJS) \
	$(CUSTOM_PARSERS_COBJS) \
	$(CUSTOM_PARSERS_CXXOBJS) \
	$(SHARED_COMPONENTS_CXXOBJS) \
	$(CSV_CXXOBJS) \
	$(CSV_COBJS) \
	$(PROFILES_SDDL_COBJS) \
	$(PARQUET_CXXOBJS) \
	$(PARQUET_COBJS) \
	$(VISUALIZER_CXXOBJS) \
	$(IO_CXXOBJS) \
	$(TRAINING_CXXOBJS) \
	$(SDDL_COMPILER_CXXOBJS) \
	$(SDDL2_COMPILER_CXXOBJS) \
	$(SDDL2_ASSEMBLER_CXXOBJS) \
	$(ML_SELECTOR_COBJS) \
	$(ML_SELECTOR_CXXOBJS) \
	$(DATAGEN_OBJS) \
	$(SERIALIZATION_TEST_OBJS) \
	$(TEST_REGISTRY_OBJS) \
	$(ZLCPP_OBJS) \
	$(LIBOBJS)

# Objects wait for the headers of the dependencies they include, which are fetched on demand
$(call mcm_order_deps,$(C_OBJS) $(CPP_OBJS),$(ZSTD_HEADER) $(LZ4_HEADER))
$(call mcm_order_deps,$(ML_SELECTOR_COBJS) $(ML_SELECTOR_CXXOBJS),$(XGBOOST_HEADER))
$(call mcm_order_deps,$(filter tests/% %/tests/%,$(ALL_GTESTS_OBJS)),$(GTEST_HEADERS))

gtests: $(LIBGTEST_A) $(LIBZSTD_A) $(LIBLZ4_A) $(LIBXGBOOST_A)
gtests: CPPFLAGS += -Ideps/googletest/googletest/include
gtests: CXXFLAGS += -Wno-undef -Wno-sign-compare
gtests: LDLIBS   += -lpthread
$(eval $(call cxx_program,gtests, \
	$(ALL_GTESTS_OBJS), \
	$(LIBGTEST_A) $(LIBZSTD_A) $(LIBLZ4_A) $(LIBXGBOOST_A) $(LIBDMLC_A)))

# ********     Examples     ********

zs2_pipeline:
$(eval $(call c_program_shared_o,zs2_pipeline,examples/zs2_pipeline.o tools/fileio/fileio.o $(LIBOBJS),$(LIBZSTD_A) $(LIBLZ4_A)))

zs2_struct:
$(eval $(call c_program_shared_o,zs2_struct,examples/zs2_struct.o tools/fileio/fileio.o $(LIBOBJS),$(LIBZSTD_A) $(LIBLZ4_A)))

zs2_trygraph:
$(eval $(call c_program_shared_o,zs2_trygraph,examples/zs2_trygraph.o $(LIBOBJS),$(LIBZSTD_A) $(LIBLZ4_A)))

zs2_selector:
$(eval $(call c_program_shared_o,zs2_selector,examples/zs2_selector.o tools/fileio/fileio.o $(LIBOBJS),$(LIBZSTD_A) $(LIBLZ4_A)))

zs2_round_trip:
$(eval $(call cxx_program_shared_o,zs2_round_trip,tests/round_trip.o tools/fileio/fileio.o $(SHARED_COMPONENTS_CXXOBJS) $(LIBOBJS),$(LIBZSTD_A) $(LIBLZ4_A)))

# ********     Compatibility tests     ********

# Compile-only check that public headers remain usable from a strict C99 TU.
.PHONY: c99_compat
c99_compat:
	$(CC) -std=c99 -Werror -Wall -Wextra -Iinclude \
	    -c tests/compat/c99_compat.c -o /dev/null

# ********     Cleaning     ********

.PHONY: clean
clean:
	# note: a lot is done within multiconf.make
	@echo Cleaning completed

#special cases : these targets require additional flags to compile without warnings
$(MCM_CACHE_ROOT)/%/src/openzl/common/errors.o : CFLAGS += -Wno-format-nonliteral
$(MCM_CACHE_ROOT)/%/src/openzl/common/logging.o : CFLAGS += -Wno-format-nonliteral
$(MCM_CACHE_ROOT)/%/src/openzl/codecs/rolz/encode_experimental_enc.o : CFLAGS += -Wno-uninitialized
$(MCM_CACHE_ROOT)/%/tests/unittest/common/test_debug.o: CXXFLAGS += -Wno-ignored-attributes

# ********     Dependencies     ********

# Provides fetch_dependency, check-dependency-pins and cleandeps
include build-scripts/make/deps.make

# Source files of a dependency, in directories $(1) with extensions $(2).
# Its library is rebuilt when any of them changes; since the sub-build may then
# find nothing to do, the library is touched afterwards.
dep_srcs = $(wildcard $(foreach d,$(1),$(addprefix $(d)/*.,$(2))))

# Builds all dependencies ahead of time
.PHONY : builddeps
builddeps : $(LIBGTEST_A) $(LIBZSTD_A) $(LIBZSTD_SO) $(LIBLZ4_A) $(LIBLZ4_SO)

# Variables are by default not exported, but when they're passed on the CLI,
# they are exported. We do not want to pass super restrictive flags to our
# dependencies, like zstd, which will fail to build with them.
unexport CFLAGS
unexport CXXFLAGS

# Zstandard
ZSTD_VERSION ?= 1.5.7
ZSTD_SHA256 ?= eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
$(eval $(call fetch_dependency,zstd,lib/zstd.h,v$(ZSTD_VERSION),https://github.com/facebook/zstd/releases/download/v$(ZSTD_VERSION)/zstd-$(ZSTD_VERSION).tar.gz,$(ZSTD_SHA256)))

ZSTD_LIBDIR := deps/zstd/lib
ZSTD_SRCS := $(call dep_srcs,$(ZSTD_LIBDIR) $(ZSTD_LIBDIR)/*,c h S)

$(LIBZSTD_SO) : MAKEOVERRIDES=
$(LIBZSTD_SO) : $(ZSTD_HEADER) $(ZSTD_SRCS)
	$(MAKE) -C $(ZSTD_LIBDIR) libzstd
	touch $@

$(LIBZSTD_A) : MAKEOVERRIDES=
$(LIBZSTD_A) : $(ZSTD_HEADER) $(ZSTD_SRCS)
	$(MAKE) -C $(ZSTD_LIBDIR) libzstd.a
	touch $@

# LZ4
LZ4_VERSION ?= 1.10.0
LZ4_SHA256 ?= 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
$(eval $(call fetch_dependency,lz4,lib/lz4.h,v$(LZ4_VERSION),https://github.com/lz4/lz4/releases/download/v$(LZ4_VERSION)/lz4-$(LZ4_VERSION).tar.gz,$(LZ4_SHA256)))

LZ4_LIBDIR := deps/lz4/lib
LZ4_SRCS := $(call dep_srcs,$(LZ4_LIBDIR),c h)

$(LIBLZ4_SO) : MAKEOVERRIDES=
$(LIBLZ4_SO) : $(LZ4_HEADER) $(LZ4_SRCS)
	$(MAKE) -C $(LZ4_LIBDIR) liblz4
	touch $@

$(LIBLZ4_A) : MAKEOVERRIDES=
$(LIBLZ4_A) : $(LZ4_HEADER) $(LZ4_SRCS)
	$(MAKE) -C $(LZ4_LIBDIR) liblz4.a
	touch $@

# Google Test
GTEST_VERSION ?= 1.17.0
GTEST_SHA256 ?= 65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
$(eval $(call fetch_dependency,googletest,googletest/include/gtest/gtest.h,v$(GTEST_VERSION),https://github.com/google/googletest/releases/download/v$(GTEST_VERSION)/googletest-$(GTEST_VERSION).tar.gz,$(GTEST_SHA256)))

GTEST_SRCS := $(call dep_srcs,$(addprefix deps/googletest/googletest/,src include/gtest include/gtest/internal include/gtest/internal/custom),cc h)

$(LIBGTEST_A) : MAKEOVERRIDES=
$(LIBGTEST_A) : $(GTEST_HEADERS) $(GTEST_SRCS)
	cd deps/googletest && cmake .
	$(MAKE) -C deps/googletest
	touch $@

# XGBoost
XGBOOST_VERSION ?= 3.1.0
XGBOOST_SHA256 ?= 4c42d35976067270a9255bf9ee290a706917bb3929a60cdd74d4dd3f1a9c86cc
$(eval $(call fetch_dependency,xgboost,include/xgboost/c_api.h,v$(XGBOOST_VERSION),https://github.com/dmlc/xgboost/releases/download/v$(XGBOOST_VERSION)/xgboost-src-$(XGBOOST_VERSION).tar.gz,$(XGBOOST_SHA256),--recursive))

XGBOOST_LIBDIR := deps/xgboost/lib

# Common CMake flags for xgboost shared library build
XGBOOST_CMAKE_COMMON := -DBUILD_STATIC_LIB=OFF -DUSE_OPENMP=OFF \
	-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$(abspath $(XGBOOST_LIBDIR))

# Platform-specific CMake flags for xgboost
XGBOOST_CMAKE_PLATFORM :=
XGBOOST_LDFLAGS :=
XGBOOST_LDLIBS :=
ifneq (,$(filter $(SMALL_CMD_LINE),$(UNAME)))
    # MinGW/MSYS: Use Unix Makefiles generator and link ws2_32 for Windows socket functions
    XGBOOST_CMAKE_PLATFORM := -G "Unix Makefiles" \
        -DCMAKE_CXX_STANDARD_LIBRARIES="-lws2_32" \
        -DCMAKE_SHARED_LINKER_FLAGS="-lws2_32"
    XGBOOST_LDFLAGS += -L$(abspath $(XGBOOST_LIBDIR))
    XGBOOST_LDLIBS += -lws2_32
else ifeq ($(shell uname),Darwin)
    # macOS: Set install_name to absolute path so dyld can find the library
    XGBOOST_CMAKE_PLATFORM := -DCMAKE_INSTALL_NAME_DIR=$(abspath $(XGBOOST_LIBDIR)) \
        -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_MACOSX_RPATH=ON
endif

# Build shared library only after static library is done (to avoid parallel cmake conflicts)
$(LIBXGBOOST_SO) : MAKEOVERRIDES=
$(LIBXGBOOST_SO) : $(LIBXGBOOST_A)
	$(MKDIR) -p $(XGBOOST_LIBDIR)
	cd deps/xgboost && mkdir -p build-shared && cd build-shared && \
		cmake .. $(XGBOOST_CMAKE_COMMON) $(XGBOOST_CMAKE_PLATFORM) && $(MAKE)
ifeq ($(shell uname),Darwin)
	install_name_tool -id "$(abspath $(XGBOOST_LIBDIR))/libxgboost.dylib" \
		"$(abspath $(XGBOOST_LIBDIR))/libxgboost.dylib" || true
endif
	touch $@

XGBOOST_SRCS := $(call dep_srcs,$(addprefix deps/xgboost/,src src/* src/*/* include/xgboost include/xgboost/* dmlc-core/src dmlc-core/src/* dmlc-core/include/dmlc),cc cu cuh h)

$(LIBXGBOOST_A) : MAKEOVERRIDES=
$(LIBXGBOOST_A) : $(XGBOOST_HEADER) $(XGBOOST_SRCS)
	$(MKDIR) -p $(XGBOOST_LIBDIR)
	cd deps/xgboost && mkdir -p build && cd build && cmake .. -DBUILD_STATIC_LIB=ON -DUSE_OPENMP=OFF -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=$(abspath $(XGBOOST_LIBDIR)) $(XGBOOST_CMAKE_PLATFORM) && $(MAKE)
	# xgboost's CMakeLists.txt strips the 'lib' prefix on Windows; rename to match expected name
	@if [ -f $(XGBOOST_LIBDIR)/xgboost.a ] && [ ! -f $(XGBOOST_LIBDIR)/libxgboost.a ]; then \
		mv $(XGBOOST_LIBDIR)/xgboost.a $(XGBOOST_LIBDIR)/libxgboost.a; \
	fi
	touch $@

# libdmlc.a is built as part of xgboost static build
$(LIBDMLC_A): $(LIBXGBOOST_A)

# Empty rule: stops make searching implicit rules for each dependency source (~100 failed stat() each)
$(filter-out $(ZSTD_HEADER) $(LZ4_HEADER) $(GTEST_HEADERS) $(XGBOOST_HEADER),$(ZSTD_SRCS) $(LZ4_SRCS) $(GTEST_SRCS) $(XGBOOST_SRCS)): ;
