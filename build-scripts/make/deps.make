# Copyright (c) Meta Platforms, Inc. and affiliates.

# Provides fetch_dependency, to declare a dependency fetched into deps/,
# check-dependency-pins, to check that git submodules are pinned to the declared tags,
# and cleandeps, to remove fetched dependencies.

GIT ?= git
CURL ?= curl
WGET ?= wget
TAR ?= tar

DEP_FETCH := $(dir $(lastword $(MAKEFILE_LIST)))fetch_dep.sh

# fetch_dependency - Rule creating deps/$(1)/$(2): from the git submodule deps/$(1) if possible,
# else by git clone of tag $(3), else by downloading tarball $(4) with SHA256 $(5).
# Optional $(6): --recursive, to also fetch nested submodules.
define fetch_dependency
DEP_PINS += $(1):$(3)
DEP_TARBALLS += deps/$(notdir $(4))
deps/$(1)/$(2):
	GIT="$$(GIT)" CURL="$$(CURL)" WGET="$$(WGET)" TAR="$$(TAR)" $$(DEP_FETCH) $(1) $(2) $(3) $(4) $(5) $(6)
endef

.PHONY: check-dependency-pins
check-dependency-pins:
	status=0; $(foreach p,$(DEP_PINS),GIT="$(GIT)" $(DEP_FETCH) --check-pin $(subst :, ,$(p)) || status=1;) exit $$status

.PHONY: cleandeps
cleandeps:
	-$(foreach p,$(DEP_PINS),$(GIT) submodule deinit -f deps/$(firstword $(subst :, ,$(p))) 2>/dev/null;)
	$(RM) -r $(foreach p,$(DEP_PINS),deps/$(firstword $(subst :, ,$(p)))) $(DEP_TARBALLS)
