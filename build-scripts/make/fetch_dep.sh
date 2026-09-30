#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.

# Populates deps/NAME with a dependency, trying in order:
#   1. its git submodule, within a git checkout
#   2. a git clone of TAG (e.g. in fbcode, which is not a git checkout)
#   3. the tarball at URL, checked against SHA256
# A way succeeds only if it provides deps/NAME/FILE. It then records TAG in the stamp
# deps/.NAME-TAG; a tree it replaces is kept as deps/NAME.previous.
# Usage: fetch_dep.sh NAME FILE TAG URL SHA256 [--recursive]
#        fetch_dep.sh --check-pin NAME TAG
# Run from the directory holding .gitmodules. GIT, CURL, WGET and TAR override the tools.

set -eu

GIT=${GIT:-git}
CURL=${CURL:-curl}
WGET=${WGET:-wget}
TAR=${TAR:-tar}

die() {
    printf 'error: %s\n' "$*" 1>&2
    exit 1
}

repo_url() {
    $GIT config -f .gitmodules "submodule.deps/$1.url" 2>/dev/null
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1
}

if [ "$1" = --check-pin ]; then
    name=$2 tag=$3
    pin=$($GIT rev-parse "HEAD:deps/$name")
    commit=$($GIT ls-remote "$(repo_url "$name")" "refs/tags/$tag" "refs/tags/$tag^{}" | tail -n 1 | cut -f1)
    [ "$pin" = "$commit" ] || die "deps/$name is pinned to $pin, but $tag is ${commit:-not found}"
    printf 'deps/%s is pinned to %s\n' "$name" "$tag"
    exit 0
fi

name=$1 file=$2 tag=$3 url=$4 sha256=$5 recursive=${6:-}
dir=deps/$name
tmp=deps/.$name.tmp
stamp=deps/.$name-$tag
tarball=deps/$(basename "$url")

rc=
for sig in 1 2 3 13 15; do eval "trap 'exit $((sig + 128))' $sig"; done
trap 'rc=$?; set +e; rm -rf "$tmp" "$tarball.part"; exit $rc' EXIT
mkdir -p deps

fetched() {
    rm -f "deps/.$name"-*
    touch "$stamp"
    exit 0
}

# Replaces $dir with the fetched tree, in one step
install_tmp() {
    if [ -d "$dir" ] && [ -n "$(find "$dir" -mindepth 1 -maxdepth 1 | head -n 1)" ]; then
        rm -rf "$dir.previous"
        mv "$dir" "$dir.previous"
        echo "Moved the previous $dir to $dir.previous"
    fi
    rm -rf "$dir"
    mv "$tmp" "$dir"
    fetched
}

# git reports success when it skips a submodule (e.g. `update = none`), hence the check of $file
if $GIT rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
    $GIT submodule update --init --single-branch --depth 1 ${recursive:+"$recursive"} "$dir" &&
    [ -f "$dir/$file" ]; then
    fetched
fi
# Shallow clone of $tag into $tmp. `git clone --branch` would warn about annotated tags.
clone_tag() {
    $GIT init -q "$tmp" &&
        $GIT -C "$tmp" remote add origin "$repo" &&
        $GIT -C "$tmp" fetch -q --depth 1 origin tag "$tag" &&
        $GIT -C "$tmp" -c advice.detachedHead=false checkout -q "$tag" &&
        { [ -z "$recursive" ] || $GIT -C "$tmp" submodule update -q --init --recursive --depth 1; }
}

if repo=$(repo_url "$name"); then
    rm -rf "$tmp"
    if clone_tag && [ -f "$tmp/$file" ]; then
        install_tmp
    fi
fi

if [ ! -f "$tarball" ] || [ "$(sha256_of "$tarball")" != "$sha256" ]; then
    echo "Downloading $url"
    rm -f "$tarball"
    $CURL -fsSL -o "$tarball.part" "$url" || $WGET -q -O "$tarball.part" "$url" ||
        die "could not fetch $dir. Fetch it by hand, with one of:
  git submodule update --init $recursive $dir
  git clone --depth 1 --branch $tag ${recursive:+--recurse-submodules }$(repo_url "$name") $dir
  download $url and extract it into $dir"
    [ "$(sha256_of "$tarball.part")" = "$sha256" ] || die "$url does not match its SHA256"
    mv "$tarball.part" "$tarball"
fi
rm -rf "$tmp"
mkdir "$tmp"
$TAR -xzmf "$tarball" -C "$tmp" --strip-components=1
[ -f "$tmp/$file" ] || die "$url does not contain $file"
install_tmp
