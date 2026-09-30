# Copyright (c) Meta Platforms, Inc. and affiliates.

# Provides fetch_dependency, to declare a dependency fetched into deps/,
# check-dependency-pins, to check that git submodules are pinned to the declared tags,
# and cleandeps, to remove fetched dependencies.

GIT ?= git
CURL ?= curl
WGET ?= wget
TAR ?= tar

DEP_FETCH := $(dir $(lastword $(MAKEFILE_LIST)))fetch_dep.sh

# Non-empty in a dry run (-n or -q)
DEP_DRY_RUN := $(findstring n,$(firstword -$(MAKEFLAGS)))$(findstring q,$(firstword -$(MAKEFLAGS)))

# fetch_dependency - Rule creating deps/$(1)/$(2): from the git submodule deps/$(1) if possible,
# else by git clone of tag $(3), else by downloading tarball $(4) with SHA256 $(5).
# Optional $(6): --recursive, to also fetch nested submodules.
# Once fetched, the dependency is fetched again when tag $(3) changes: its stamp deps/.$(1)-$(3)
# is an included makefile, so make fetches it first, then restarts with a fresh view of the files.
# In a dry run (-n, -q), where GNU make would still remake included makefiles,
# the stamp is an order-only prerequisite instead, so the fetch is only listed.
define fetch_dependency
DEP_NAMES += $(1)
DEP_PINS += $(1):$(3)
DEP_TARBALLS += deps/$(notdir $(4))
DEP_FETCH_$(1) = GIT="$$(GIT)" CURL="$$(CURL)" WGET="$$(WGET)" TAR="$$(TAR)" $$(DEP_FETCH) $(1) $(2) $(3) $(4) $(5) $(6)
deps/$(1)/$(2):
	$$(DEP_FETCH_$(1))
ifneq (,$$(wildcard deps/$(1)/$(2)))
ifeq (,$$(DEP_DRY_RUN))
include deps/.$(1)-$(3)
else
deps/$(1)/$(2): | deps/.$(1)-$(3)
endif
deps/.$(1)-$(3):
	$$(DEP_FETCH_$(1))
endif
endef

.PHONY: check-dependency-pins
check-dependency-pins:
	status=0; $(foreach p,$(DEP_PINS),GIT="$(GIT)" $(DEP_FETCH) --check-pin $(subst :, ,$(p)) || status=1;) exit $$status

.PHONY: cleandeps
cleandeps:
	-$(foreach d,$(DEP_NAMES),$(GIT) submodule deinit -f deps/$(d) 2>/dev/null;)
	$(RM) -r $(foreach d,$(DEP_NAMES),deps/$(d) deps/$(d).previous $(wildcard deps/.$(d)-*)) $(DEP_TARBALLS)
