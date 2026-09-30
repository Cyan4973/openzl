#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.

# Checks that multiconf.make rebuilds exactly what changed, on a copy of ./project
# Usage: run_tests.sh [make-command]

set -eu

MAKE_CMD=${1:-make}
HERE=$(cd "$(dirname "$0")" && pwd)

WORK=
rc=
for sig in 1 2 3 13 15; do eval "trap 'exit $((sig + 128))' $sig"; done
trap 'rc=$?; set +e; rm -rf "$WORK"; exit $rc' EXIT
WORK=$(mktemp -d)
cp -R "$HERE/project/." "$WORK"
cd "$WORK"

m() {
    $MAKE_CMD MULTICONF="$HERE/../multiconf.make" "$@"
}

die() {
    printf 'FAIL: %s\n' "$*" 1>&2
    exit 1
}

check() {  # description, actual, expected
    [ "$2" = "$3" ] || die "$1: got '$2', expected '$3'"
    printf 'ok: %s\n' "$1"
}

# Number of compile (or link) commands that building the given targets would run
compiles() {
    m -n "$@" | grep -c -E '^echo (CC|CXX) ' || true
}
links() {
    m -n "$@" | grep -c -E '^echo (LD|AR) ' || true
}

# Object directory of a built target
objdir() {
    dirname "$(dirname "$(readlink "$1")")"
}

m all >/dev/null
check "second build does nothing" "$(compiles all) $(links all)" "0 0"

touch src/common.h
check "header change recompiles only its includers" \
    "$(m -n all | grep -E '^echo (CC|CXX) ' | sed 's|.*/||' | sort -u)" "a.o"
m all >/dev/null

check "targets with identical flags share objects" \
    "$(objdir prog_b) $(objdir prog_cxx) $(objdir libtest.a)" \
    "$(objdir prog_a) $(objdir prog_a) $(objdir prog_a)"
[ "$(objdir prog_fast)" != "$(objdir prog_a)" ] || die "target with its own flags shares objects"
printf 'ok: %s\n' "target with its own flags gets its own objects"
check "target flags are applied" "$(m -n -W src/b.c prog_fast | grep -c -e '-DFAST.*src/b\.c' || true)" 1
check "editing target flags recompiles that target" "$(compiles prog_fast FAST_FLAGS=-DMORE)" 3
check "editing target flags leaves other targets alone" "$(compiles prog_a FAST_FLAGS=-DMORE)" 0
check "CFLAGS change recompiles a C++ program" "$(compiles prog_cxx CFLAGS=-DOTHER)" 3

printf 'int added_symbol(void);\nint added_symbol(void) { return 0; }\n' > src/c.c
check "new source compiles only itself" "$(compiles prog_a)" 1
m prog_a libtest.a >/dev/null
check "new source is linked" "$(nm prog_a | grep -c added_symbol || true)" 1
rm src/c.c
check "removed source compiles nothing" "$(compiles prog_a)" 0
m prog_a libtest.a >/dev/null
check "removed source is no longer linked" "$(nm prog_a | grep -c added_symbol || true)" 0
check "removed source is no longer archived" "$(ar t libtest.a | grep -c '^c\.o$' || true)" 0

DEFAULT_CONFIG=$(objdir prog_a | cut -d/ -f1-2)
m all CFLAGS=-DOTHER >/dev/null
check "depfiles of other configurations are not read" \
    "$(m print-depfiles | tr ' ' '\n' | grep -c -v -e "^$DEFAULT_CONFIG/" -e '^$' || true)" 0

m clean >/dev/null
check "clean removes all build outputs" "$(find . -mindepth 1 -maxdepth 1 | sort | tr '\n' ' ')" "./Makefile ./src "
