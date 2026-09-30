# Copyright (c) Meta Platforms, Inc. and affiliates.

# Provides fetch_dependency, to declare a dependency fetched into deps/,
# check-dependency-pins, to check that git submodules are pinned to the declared tags,
# and cleandeps or cleandep-NAME, to remove fetched dependencies.

GIT ?= git
CURL ?= curl
WGET ?= wget
TAR ?= tar

DEP_FETCH := $(dir $(lastword $(MAKEFILE_LIST)))fetch_dep.sh

# fetch_dependency - Rule creating deps/$(1)/$(2): from the git submodule deps/$(1) if possible,
# else by git clone of tag $(3), else by downloading tarball $(4) with SHA256 $(5).
# Optional $(6): --recursive, to also fetch nested submodules.
# The fetch records tag $(3) in the stamp deps/.$(1)-$(3). Without it, an existing deps/$(1)
# stops the build, until cleandep-$(1) removes it.
define fetch_dependency
DEP_PINS += $(1):$(3)
deps/$(1)/$(2):
	GIT="$$(GIT)" CURL="$$(CURL)" WGET="$$(WGET)" TAR="$$(TAR)" $$(DEP_FETCH) $(1) $(2) $(3) $(4) $(5) $(6)
ifneq (,$$(wildcard deps/$(1)/$(2)))
ifeq (,$$(wildcard deps/.$(1)-$(3)))
deps/$(1)/$(2): dep-outdated-$(1)
endif
endif
.PHONY: dep-outdated-$(1) cleandep-$(1)
dep-outdated-$(1):
	@echo "error: deps/$(1) is not at $(3): remove it with 'make cleandep-$(1)'" >&2; exit 1
cleandeps: cleandep-$(1)
cleandep-$(1):
	-$$(GIT) submodule deinit -f deps/$(1) 2>/dev/null
	$$(RM) -r deps/$(1) deps/.$(1)-* deps/$(notdir $(4))
endef

.PHONY: check-dependency-pins
check-dependency-pins:
	status=0; $(foreach p,$(DEP_PINS),GIT="$(GIT)" $(DEP_FETCH) --check-pin $(subst :, ,$(p)) || status=1;) exit $$status

# fetch_dependency makes each cleandep-NAME a prerequisite
.PHONY: cleandeps
cleandeps:
