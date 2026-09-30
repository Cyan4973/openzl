#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.

# Checks each way fetch_dep.sh gets a dependency, offline, from a local repository and tarball

set -eu

FETCH=$(cd "$(dirname "$0")/.." && pwd)/fetch_dep.sh

WORK=
rc=
for sig in 1 2 3 13 15; do eval "trap 'exit $((sig + 128))' $sig"; done
trap 'rc=$?; set +e; rm -rf "$WORK"; exit $rc' EXIT
WORK=$(mktemp -d)
cd "$WORK"

die() {
    printf 'FAIL: %s\n' "$*" 1>&2
    exit 1
}

check() {  # description, actual, expected
    [ "$2" = "$3" ] || die "$1: got '$2', expected '$3'"
    printf 'ok: %s\n' "$1"
}

# A private git configuration, which allows local repositories as submodules
export HOME="$WORK" GIT_CONFIG_NOSYSTEM=1
git config --global user.name test
git config --global user.email test@example.com
git config --global protocol.file.allow always

# The dependency: a repository with annotated tag v1, and its release tarball
git init -q foo
mkdir foo/lib
echo v1 > foo/lib/foo.h
git -C foo add lib
git -C foo commit -q -m v1
git -C foo tag -a -m v1 v1
mkdir release
git -C foo archive --prefix=foo-1/ HEAD | tar -x -C release
tar -czf foo-1.tar.gz -C release foo-1
SHA=$(sha256sum foo-1.tar.gz 2>/dev/null || shasum -a 256 foo-1.tar.gz)
SHA=${SHA%% *}
URL=file://$WORK/foo-1.tar.gz
REPO=file://$WORK/foo

# A project which is not a git checkout
mkdir project
cd project
printf '[submodule "deps/foo"]\n\tpath = deps/foo\n\turl = %s\n' "$REPO" > .gitmodules

"$FETCH" foo lib/foo.h v1 "$URL" "$SHA" >/dev/null 2>err
check "outside a git checkout: git clone of the tag, silently" "$(cat deps/foo/lib/foo.h) $(git -C deps/foo describe --tags) $(cat err)" "v1 v1 "

rm -rf deps/foo
GIT=false "$FETCH" foo lib/foo.h v1 "$URL" "$SHA" >/dev/null
check "without git: tarball, without its top directory" "$(cat deps/foo/lib/foo.h) $(ls -A deps/foo)" "v1 lib"

rm -rf deps
if GIT=false "$FETCH" foo lib/foo.h v1 "$URL" 0000 >/dev/null 2>err; then die "wrong checksum accepted"; fi
check "wrong checksum: rejected, nothing left behind" "$(grep -c SHA256 err) $(ls -A deps)" "1 "

if GIT=false CURL=false WGET=false "$FETCH" foo lib/foo.h v1 "$URL" "$SHA" >/dev/null 2>err; then die "fetched without any tool"; fi
check "no way to fetch: explains how to by hand, nothing left behind" "$(grep -c 'by hand' err) $(ls -A deps)" "1 "

if GIT=false "$FETCH" foo lib/bar.h v1 "$URL" "$SHA" >/dev/null 2>err; then die "accepted a tarball without the expected file"; fi
check "tarball without the expected file: rejected, nothing left behind" "$(grep -c 'does not contain' err) $(find deps -mindepth 1 ! -name '*.tar.gz')" "1 "

# A project which is a git checkout, with the dependency as a submodule
cd "$WORK"
git init -q super
cd super
git submodule add -q "$REPO" deps/foo 2>/dev/null
git commit -q -m "add foo"
git submodule deinit -q -f deps/foo

"$FETCH" foo lib/foo.h v1 "$URL" "$SHA" >/dev/null 2>&1
check "in a git checkout: submodule" "$(cat deps/foo/lib/foo.h) $(git -C deps/foo rev-parse --show-superproject-working-tree | grep -c super)" "v1 1"

git submodule deinit -q -f deps/foo
git config submodule.deps/foo.update none
"$FETCH" foo lib/foo.h v1 "$URL" "$SHA" >/dev/null 2>&1
check "in a git checkout, submodule skipped by git: falls back to a clone" "$(cat deps/foo/lib/foo.h)" "v1"
git config --unset submodule.deps/foo.update

check "pin check: pinned to the tag" "$("$FETCH" --check-pin foo v1 | grep -c 'pinned to v1')" 1
echo v2 > "$WORK/foo/lib/foo.h"
git -C "$WORK/foo" commit -q -a -m v2
git -C "$WORK/foo" tag -a -m v2 v2
if "$FETCH" --check-pin foo v2 >/dev/null 2>&1; then die "pin check missed a pin behind its tag"; fi
printf 'ok: %s\n' "pin check: pinned behind the tag"
