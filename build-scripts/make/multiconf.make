# Copyright (c) Meta Platforms, Inc. and affiliates.


# Provides c_program(_shared_o) and cxx_program(_shared_o) target generation macros
# Provides static_library and c_dynamic_library target generation macros
# Support recompilation of only impacted units when an associated *.h is updated.
# Provides V=1 / VERBOSE=1 support. V=2 is used for debugging purposes.
# Complement target clean: delete objects and binaries created by this script

# Source files are discovered in the current directory and below.
# Requires:
# - directory `.cache/multiconf/` available to cache object files.
#   alternatively: set MCM_CACHE_ROOT to some different value.
# Optional:
# - MCM_EXCLUDE_DIRS: directories to skip, besides hidden ones and MCM_CACHE_ROOT,
#   as paths from the current directory or `find -path` patterns (e.g. docs */experimental)
# - C_SRCDIRS, CXX_SRCDIRS, ASM_SRCDIRS: directories to add, e.g. excluded ones
# - C_SRCS, CPP_SRCS, CC_SRCS and ASM_SRCS (or C_OBJS, CPP_OBJS, CC_OBJS and ASM_OBJS)
#   replace the discovered files
# - HASH can be set to a different custom hash program.

# *_program*: generates a recipe for a target that will be built in a cache directory.
# The cache directory is automatically derived from MCM_CACHE_ROOT and the compilers and flags
# in effect for the target, including target-level modifications (like: target: CFLAGS += someFlag).
# Targets with identical flags share their object files.
# *_shared_o* variants are kept for compatibility: they are identical to the standard variants.
#
# Note: enables .SECONDEXPANSION, so a `$$` in a prerequisite list is expanded twice.

# All *_program* macro functions take up to 4 argument:
# - The name of the target
# - The list of object files to build in the cache directory
# - An optional list of dependencies for linking, that will not be built
# - An optional complementary recipe code, that will run after compilation and link


# Silent mode is default; use V = 1 or VERBOSE = 1 to see compilation lines
VERBOSE ?= $(V)
$(VERBOSE).SILENT:

# Directory where object files will be built
MCM_CACHE_ROOT ?= .cache/multiconf
# CACHE_ROOT, the former name, is ignored: an environment variable of that name would make `clean` delete its directory
ifneq (,$(filter file command line override,$(origin CACHE_ROOT)))
  $(warning CACHE_ROOT is ignored, set MCM_CACHE_ROOT instead)
endif

# --------------------------------------------------------------------------------------------

# Dependency management
DEPFLAGS = -MT $@ -MMD -MP -MF

# --------------------------------------------------------------------------------------------

# Automatic determination of build artifacts cache directory, keyed on build
# flags, so that we can do incremental, parallel builds of different binaries
# with different build flags without collisions.

UNAME ?= $(shell uname)
ifeq ($(UNAME), Darwin)
  HASH ?= md5
else ifeq ($(UNAME), FreeBSD)
  HASH ?= gmd5sum
else ifeq ($(UNAME), OpenBSD)
  HASH ?= md5
endif
HASH ?= md5sum

# Layout: $(MCM_CACHE_ROOT)/<global flags>/<target flags>/ holds object files, and each binary
# is linked in a subdirectory keyed on its object list and link flags, so that changing
# them relinks without recompiling.

# Flags that affect object files. Expanded in the context of each target,
# so that target-level modifications are included.
MCM_COMPILE_KEY = $(CC) $(CXX) $(CPPFLAGS) $(CFLAGS) $(CXXFLAGS)

HAVE_HASH := $(shell echo 1 | $(HASH) > /dev/null && echo 1 || echo 0)
ifeq ($(HAVE_HASH),0)
  $(info warning : could not find HASH ($(HASH)), required to differentiate builds using different flags)
  MCM_GLOBAL_DIR := generic
  mcm_subdirs = $(1)/bin
else
  MCM_GLOBAL_DIR := $(firstword $(shell echo $(MCM_COMPILE_KEY) | $(HASH)))
  # mcm_subdirs - <hash of $(2)>/<hash of $(3)>, in a single shell call.
  # md5sum follows each hash with `-`, or `*-` in binary mode (Windows).
  mcm_subdirs = $(call mcm_join_dirs,$(filter-out - *-,$(shell echo $(2) | $(HASH); echo $(3) | $(HASH))))
  mcm_join_dirs = $(word 1,$(1))/$(word 2,$(1))
endif

# The target-level flags of a target are only visible when make processes it,
# hence its cache path is computed during secondary expansion of its prerequisites.
.SECONDEXPANSION:

# mcm_cache_path - Cache path of binary $(1), from the flags in effect for $(1)
mcm_cache_path = $(MCM_CACHE_ROOT)/$(MCM_GLOBAL_DIR)/$(call mcm_subdirs,$(1),$(MCM_COMPILE_KEY) $(MCM_XHASH_$(1)),$(MCM_OBJS_$(1)) $(MCM_LINK_KEY_$(1)))/$(1)

# mcm_link_objs - Object files $(2) of a binary whose cache path stem is $(1):
# they are in the parent directory
mcm_link_objs = $(addprefix $(MCM_CACHE_ROOT)/$(dir $(1)),$(2))

STRIP ?= strip
MKDIR ?= mkdir
LN ?= ln

# --------------------------------------------------------------------------------------------
# The following macros are used to create object files in the cache directory.
# The object files are named after the source file, but with a different path.

# Create build directories on-demand.
#
# For some reason, make treats the directory as an intermediate file and tries
# to delete it. So we work around that by marking it "precious". Solution found
# here:
# http://ismail.badawi.io/blog/2017/03/28/automatic-directory-creation-in-make/
.PRECIOUS: $(MCM_CACHE_ROOT)/%/.
$(MCM_CACHE_ROOT)/%/. :
	$(MKDIR) -p $@


define addTargetAsmObject  # targetName, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2))))

.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(1)
$$(MCM_CACHE_ROOT)/%/$(1) : $(1:.o=.S) $(2) | $$(MCM_CACHE_ROOT)/%/$(dir $(1))/. $$$$(MCM_ODEPS_$(1))
	@echo AS $$@
	$$(CC) $$(CPPFLAGS) $$(CXXFLAGS) $$(DEPFLAGS) $$(MCM_CACHE_ROOT)/$$*/$(1:.o=.d) -c $$< -o $$@

endef # addTargetAsmObject

define addTargetCObject  # targetName, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2)))) #debug print

.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(1)
$$(MCM_CACHE_ROOT)/%/$(1) : $(1:.o=.c) $(2) | $$(MCM_CACHE_ROOT)/%/$(dir $(1))/. $$$$(MCM_ODEPS_$(1))
	@echo CC $$@
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) $$(DEPFLAGS) $$(MCM_CACHE_ROOT)/$$*/$(1:.o=.d) -c $$< -o $$@

endef # addTargetCObject

define addTargetCxxObject  # targetName, suffix, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3))))

.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(1)
$$(MCM_CACHE_ROOT)/%/$(1) : $(1:.o=.$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/$(dir $(1))/. $$$$(MCM_ODEPS_$(1))
	@echo CXX $$@
	$$(CXX) $$(CPPFLAGS) $$(CXXFLAGS) $$(DEPFLAGS) $$(MCM_CACHE_ROOT)/$$*/$(1:.o=.d) -c $$< -o $$@

endef # addTargetCxxObject

# mcm_order_deps - Make objects $(1) wait for files $(2) before compiling, without
# recompiling when they change: e.g. headers of dependencies fetched on demand.
mcm_order_deps = $(foreach o,$(1),$(eval MCM_ODEPS_$(o) += $(2)))

# Discover source files: in the current directory and below, except in hidden directories,
# $(MCM_CACHE_ROOT) and MCM_EXCLUDE_DIRS. Files are listed rather than directories, so that
# large trees without sources (e.g. node_modules) only cost their traversal.
MCM_SRCS := $(patsubst ./%,%,$(shell find . -name '.?*' -prune \
	$(foreach d,$(patsubst %/,%,$(MCM_CACHE_ROOT) $(MCM_EXCLUDE_DIRS)),-o -path './$(d)' -prune) \
	-o ! -type d \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.S' \) -print))

# Directories added by C_SRCDIRS, CXX_SRCDIRS and ASM_SRCDIRS
vpath %.c $(C_SRCDIRS)
vpath %.cpp $(CXX_SRCDIRS)
vpath %.cc $(CXX_SRCDIRS)
vpath %.S $(ASM_SRCDIRS)

# If C_SRCS, CPP_SRCS, CC_SRCS and ASM_SRCS are not defined, use the discovered and added files
C_SRCS   ?= $(sort $(filter %.c,$(MCM_SRCS)) $(foreach dir,$(C_SRCDIRS),$(wildcard $(dir)/*.c)))
CPP_SRCS ?= $(sort $(filter %.cpp,$(MCM_SRCS)) $(foreach dir,$(CXX_SRCDIRS),$(wildcard $(dir)/*.cpp)))
CC_SRCS  ?= $(sort $(filter %.cc,$(MCM_SRCS)) $(foreach dir,$(CXX_SRCDIRS),$(wildcard $(dir)/*.cc)))
CXX_SRCS ?= $(CPP_SRCS) $(CC_SRCS)
ASM_SRCS ?= $(sort $(filter %.S,$(MCM_SRCS)) $(foreach dir,$(ASM_SRCDIRS),$(wildcard $(dir)/*.S)))

# If C_SRCS, CXX_SRCS and ASM_SRCS are not defined, use C_OBJS, CXX_OBJS and ASM_OBJS
C_OBJS   ?= $(patsubst %.c,%.o,$(C_SRCS))
CPP_OBJS ?= $(patsubst %.cpp,%.o,$(CPP_SRCS))
CC_OBJS  ?= $(patsubst %.cc,%.o,$(CC_SRCS))
CXX_OBJS ?= $(CPP_OBJS) $(CC_OBJS)
ASM_OBJS ?= $(patsubst %.S,%.o,$(ASM_SRCS))

# Create targets for individual object files
$(foreach OBJ,$(C_OBJS),$(eval $(call addTargetCObject,$(OBJ))))
$(foreach OBJ,$(CPP_OBJS),$(eval $(call addTargetCxxObject,$(OBJ),cpp)))
$(foreach OBJ,$(CC_OBJS),$(eval $(call addTargetCxxObject,$(OBJ),cc)))
$(foreach OBJ,$(ASM_OBJS),$(eval $(call addTargetAsmObject,$(OBJ))))

# Include the depfiles of objects built with the current global flags, so that header
# changes trigger recompilation. Cache directories of other global flags are not read.
MCM_DEPFILES := $(shell find $(MCM_CACHE_ROOT)/$(MCM_GLOBAL_DIR) -name '*.d' 2>/dev/null)
# Empty rule: stops make searching implicit rules to remake each depfile (~100 failed stat() each).
$(MCM_DEPFILES): ;
include $(MCM_DEPFILES)

# --------------------------------------------------------------------------------------------
# The following macros are used to create targets in the user Makefile.
# Binaries are built in the cache directory, and then symlinked to the current directory.
# The cache directory is automatically derived from MCM_CACHE_ROOT and list of flags and compilers.


# static_library - Create build rules for a static library with caching
# Parameters:
#   1. libName       - Library name (becomes output file and phony target)
#   2. objectDeps    - Object file dependencies (will be built in cache path)
# The following parameters are all optional:
#   3. extraDeps     - Additional dependencies (no cache path prefix)
#   4. postBuildCmds - Extra commands to run after AR
#   5. extraHash     - Additional key to compute the unique cache path
# Example:
#   $(call static_library,libmath.a,vector.o matrix.o,$(CONFIG_H),strip $@,$(VERSION))
define static_library  # libName, objectDeps, extraDeps, postBuildCmds, extraHash

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5))))
MCM_ALL_BINS += $(1)
MCM_OBJS_$(1) := $(2)
MCM_XHASH_$(1) := $(5)
MCM_LINK_KEY_$(1) = $$(AR) $$(ARFLAGS) $(MCM_STRIP)

$$(MCM_CACHE_ROOT)/%/$(1) : $$$$(call mcm_link_objs,$$$$*,$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/.
	@echo AR $$@
ifeq ($(MCM_LD_RESPONSE_FILE),1)
	$$(file >$(1)_objects.rsp,$$(filter-out %.a,$$^))
	$$(AR) $$(ARFLAGS) $$@ @$(1)_objects.rsp
	$(RM) $(1)_objects.rsp
else
	$$(AR) $$(ARFLAGS) $$@ $$(filter-out %.a,$$^)
endif
	$(4)

.PHONY: $(1)
$(1) : ARFLAGS = rcs
$(1) : $$$$(call mcm_cache_path,$(1))
	$$(LN) -sf $$< $$@

endef # static_library


# c_dynamic_library - Create build rules for a C dynamic/shared library with caching
# Parameters:
#   1. libName      - Library name (becomes output file and phony target)
#   2. objectDeps   - Object file dependencies (will be built in cache path)
# The following parameters are all optional:
#   3. extraDeps    - Additional dependencies (no cache path prefix)
#   4. postLinkCmds - Extra commands to run after linking
#   5. extraHash    - Additional key to compute the unique cache path
# Example:
#   $(call c_dynamic_library,libmath.so,vector.o matrix.o,$(CONFIG_H),strip $@,$(VERSION))
define c_dynamic_library  # libName, objectDeps, extraDeps, postLinkCmds, extraHash

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5))))
MCM_ALL_BINS += $(1)
MCM_OBJS_$(1) := $(2)
MCM_XHASH_$(1) := $(5)
MCM_LINK_KEY_$(1) = $$(LDFLAGS) $$(LDLIBS) $(MCM_STRIP)

$$(MCM_CACHE_ROOT)/%/$(1) : $$$$(call mcm_link_objs,$$$$*,$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/.
	@echo LD $$@
ifeq ($(MCM_LD_RESPONSE_FILE),1)
	$$(file >$(1)_objects.rsp,$$^)
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) $$(LDFLAGS) -shared -o $$@ @$(1)_objects.rsp $$(LDLIBS)
	$(RM) $(1)_objects.rsp
else
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) $$(LDFLAGS) -shared -o $$@ $$^ $$(LDLIBS)
endif
ifeq ($(MCM_STRIP),1)
	-$(STRIP) -S $$@
endif
	$(4)

.PHONY: $(1)
$(1) : CFLAGS += -fPIC
$(1) : $$$$(call mcm_cache_path,$(1))
	$$(LN) -sf $$< $$@

endef # c_dynamic_library


# program_base - Create build rules for an executable program with caching
# Parameters:
#   1. progName      - Executable name (becomes output file and phony target)
#   2. objectDeps    - Object file dependencies (will be prefixed with cache path)
# Parameters 3 to 5 are optional:
#   3. extraDeps     - Additional dependencies (without cache path prefix)
#   4. postLinkCmds  - Extra commands to run after linking
#   5. extraHash     - Additional data to include in cache path hash
# Parameters 6 & 7 are compulsory:
#   6. compiler      - Variable name of compiler to use (CC or CXX)
#   7. compilerFlags - Variable name of compiler flags to use (CFLAGS or CXXFLAGS)
# Example:
#   $(call program_base,myapp,main.o utils.o,$(CONFIG_H),strip $@,$(VERSION),CC,CFLAGS)
#   $(call program_base,mycppapp,main.o utils.o,$(CONFIG_H),strip $@,$(VERSION),CXX,CXXFLAGS)
define program_base  # progName, objectDeps, extraDeps, postLinkCmds, extraHash, compiler, compilerFlags

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5),$(6),$(7))))
MCM_ALL_BINS += $(1)
MCM_OBJS_$(1) := $(2)
MCM_XHASH_$(1) := $(5)
MCM_LINK_KEY_$(1) = $$(LDFLAGS) $$(LDLIBS) $$(MCM_STRIP)

ifeq ($(MCM_LD_RESPONSE_FILE),1)
# Use response files when command line length limit is too small to fit the list of object files
# Note: requires GNU make 4.0 or later

$$(MCM_CACHE_ROOT)/%/$(1) : $$$$(call mcm_link_objs,$$$$*,$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/.
	@echo LD $$@
	$$(file >$(1)_objects.rsp,$$^)
	$$($(6)) $$(CPPFLAGS) $$($(7)) @$(1)_objects.rsp -o $$@ $$(LDFLAGS) $$(LDLIBS)
	$(RM) $(1)_objects.rsp
ifeq ($(MCM_STRIP),1)
	-$(STRIP) $$@$(EXE)
endif
	$(4)

else

# for normal cases: use direct listing of object files
$$(MCM_CACHE_ROOT)/%/$(1) : $$$$(call mcm_link_objs,$$$$*,$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/.
	@echo LD $$@
	$$($(6)) $$(CPPFLAGS) $$($(7)) $$^ -o $$@ $$(LDFLAGS) $$(LDLIBS)
ifeq ($(MCM_STRIP),1)
	-$(STRIP) $$@$(EXE)
endif
	$(4)

endif

.PHONY: $(1)
$(1) : $$$$(call mcm_cache_path,$(1))
	$$(LN) -sf $$< $$@$(EXE)

endef # program_base
# Note: $(EXE) must be set to .exe for Windows

define c_program  # progName, objectDeps, extraDeps, postLinkCmds
$$(eval $$(call program_base,$(1),$(2),$(3),$(4),,CC,CFLAGS))
endef # c_program

define c_program_shared_o  # progName, objectDeps, extraDeps, postLinkCmds
$$(eval $$(call program_base,$(1),$(2),$(3),$(4),,CC,CFLAGS))
endef # c_program_shared_o

define cxx_program  # progName, objectDeps, extraDeps, postLinkCmds
$$(eval $$(call program_base,$(1),$(2),$(3),$(4),,CXX,CXXFLAGS))
endef # cxx_program

define cxx_program_shared_o  # progName, objectDeps, extraDeps, postLinkCmds
$$(eval $$(call program_base,$(1),$(2),$(3),$(4),,CXX,CXXFLAGS))
endef # cxx_program_shared_o

# --------------------------------------------------------------------------------------------

# Cleaning: delete all objects and binaries created by this script, the parent directory
# of MCM_CACHE_ROOT if left empty, and cachedObjs/, the default cache directory of earlier versions
.PHONY: clean_cache
clean_cache:
	$(RM) -rf $(MCM_CACHE_ROOT) cachedObjs
	rmdir $(dir $(MCM_CACHE_ROOT)) 2>/dev/null || true
	$(RM) $(MCM_ALL_BINS)
	$(RM) *.rsp

# automatically attach to standard clean target
.PHONY: clean
clean: clean_cache
