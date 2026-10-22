#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the release artifacts for the machine this runs on, and checks them.
#
# Run by .github/workflows/release.yml once per architecture, and run by hand
# the same way (packaging/README.md):
#
#   packaging/build-release.sh [--out DIR] [--jobs N] [--only static|dynamic]
#
# For each flavour it
#   1. configures and builds the binary (static: the "static" preset; dynamic:
#      the same Release flags without the static link, tests off);
#   2. reads the binary back with the gates the project already has
#      (tools/check-no-test-hooks.sh on both, tools/check-runtime-deps.sh on
#      the dynamic one, a no-INTERP/no-DYNAMIC check on the static one);
#   3. makes a tarball: loadforge-VERSION-linux-ARCH-FLAVOUR.tar.gz;
#   4. unpacks the tarball into a scratch directory and runs the unpacked copy:
#      --version, and a two-second verified stress run that must exit 0.
# The dynamic flavour also becomes a .deb (packaging/build-deb.sh). Last, a
# checksum file for everything in DIR: loadforge-VERSION-linux-ARCH.SHA256SUMS.
#
# VERSION is whatever the binary prints for --version. This script does not
# choose a version; the project() call in CMakeLists.txt and
# LOADFORGE_MILESTONE do.
#
# Exit code: 0 everything built and checked; anything else, the first failure.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/build/release-artifacts"
jobs="$(nproc)"
only="both"

usage() {
  echo "usage: $0 [--out DIR] [--jobs N] [--only static|dynamic]" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --out) [ $# -ge 2 ] || usage; out="$2"; shift 2 ;;
    --jobs) [ $# -ge 2 ] || usage; jobs="$2"; shift 2 ;;
    --only) [ $# -ge 2 ] || usage; only="$2"; shift 2 ;;
    *) usage ;;
  esac
done
case "$only" in static | dynamic | both) ;; *) usage ;; esac

case "$(uname -m)" in
  x86_64) arch=x86_64 ;;
  aarch64 | arm64) arch=aarch64 ;;
  *) echo "build-release: unsupported machine $(uname -m): x86-64 and ARM64 only" >&2; exit 1 ;;
esac

# Release builds use the compiler CI uses unless the caller names one.
export CC="${CC:-gcc-13}"
export CXX="${CXX:-g++-13}"

mkdir -p "$out"
out="$(cd "$out" && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# The archive's timestamps are the commit's, so building twice from one commit
# gives the same bytes in the tarball.
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$root" log -1 --format=%ct 2>/dev/null || echo 0)}"

version=""
built=()

build_static() {
  echo "== static build"
  (cd "$root" && cmake --preset static && cmake --build --preset static -j "$jobs")
  binary="$root/build/static/loadforge"
}

build_dynamic() {
  echo "== dynamic build"
  (cd "$root" &&
    cmake -S . -B build/release-dynamic -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLOADFORGE_BUILD_TESTS=OFF &&
    cmake --build build/release-dynamic -j "$jobs")
  binary="$root/build/release-dynamic/loadforge"
}

check_static() {
  # Genuinely static: no program interpreter, no dynamic section.
  if readelf -l "$1" | grep -q 'INTERP' || ! readelf -d "$1" 2>&1 | grep -q 'no dynamic section'; then
    echo "build-release: $1 is dynamically linked" >&2
    exit 1
  fi
}

# Unpack the tarball somewhere else and run what is in it.
smoke_tarball() {
  local tarball="$1" name="$2" want_version="$3" dir
  dir="$(mktemp -d -p "$scratch")"
  tar -xzf "$tarball" -C "$dir"
  local copy="$dir/$name/loadforge"
  [ -x "$copy" ] || { echo "build-release: $tarball holds no $name/loadforge" >&2; exit 1; }
  local reported
  reported="$("$copy" --version)"
  [ "$reported" = "$want_version" ] || {
    echo "build-release: $tarball reports '$reported', expected '$want_version'" >&2
    exit 1
  }
  "$copy" stress --test sort --duration 2s > "$dir/stress.log" 2>&1 || {
    echo "build-release: the unpacked $name failed a two-second stress run:" >&2
    tail -n 30 "$dir/stress.log" >&2
    exit 1
  }
  grep -q '^verdict: clean$' "$dir/stress.log" || {
    echo "build-release: the unpacked $name did not end with 'verdict: clean'" >&2
    exit 1
  }
  echo "   $name: --version '$reported', two-second stress run clean"
}

package_flavour() {
  local flavour="$1" reported name stage tarball
  reported="$("$binary" --version)"
  if [ -z "$version" ]; then
    version="$reported"
  elif [ "$version" != "$reported" ]; then
    echo "build-release: static and dynamic builds disagree on the version" >&2
    exit 1
  fi

  "$root/tools/check-no-test-hooks.sh" "$binary"
  if [ "$flavour" = static ]; then
    check_static "$binary"
  else
    "$root/tools/check-runtime-deps.sh" "$binary"
  fi

  name="loadforge-$version-linux-$arch-$flavour"
  stage="$scratch/stage-$flavour/$name"
  mkdir -p "$stage"
  cp "$binary" "$stage/loadforge"
  strip --strip-unneeded "$stage/loadforge"
  cp "$root/LICENSE" "$root/NOTICE" "$root/README.md" "$root/packaging/validate-machine.sh" "$stage/"
  mkdir -p "$stage/docs"
  cp "$root/docs/safety.md" "$root/docs/telemetry-sources.md" "$root/docs/architecture.md" \
    "$root/docs/hardware-validation.md" "$stage/docs/"
  tarball="$out/$name.tar.gz"
  tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    -C "$scratch/stage-$flavour" -cf - "$name" | gzip -n -9 > "$tarball"
  smoke_tarball "$tarball" "$name" "$version"
  built+=("$tarball")

  if [ "$flavour" = dynamic ]; then
    local deb
    deb="$("$root/packaging/build-deb.sh" "$stage/loadforge" "$out")"
    built+=("$deb")
  fi
}

if [ "$only" != dynamic ]; then
  build_static
  package_flavour static
fi
if [ "$only" != static ]; then
  build_dynamic
  package_flavour dynamic
fi

sums="$out/loadforge-$version-linux-$arch.SHA256SUMS"
(cd "$out" && sha256sum "${built[@]##*/}" > "$sums")
echo "== done: $version on $arch"
cat "$sums"
