#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes the Debian package of a dynamically linked loadforge binary.
#
#   packaging/build-deb.sh BINARY OUTDIR
#
# Prints the path of the package on standard output (and nothing else there).
#
# The package holds the one executable, the licence, and the documents.
# Depends is not written by hand: dpkg-shlibdeps reads the binary's DT_NEEDED
# entries and the symbol versions it uses against the libraries of the machine
# that builds it, so the package asks for the oldest libc6 and libstdc++6 that
# carry every symbol the binary binds to. Built on Ubuntu 24.04 that is
# libc6 >= 2.38-ish and libstdc++6 >= 13: Ubuntu 24.04 is the declared minimum
# for the dynamic artifact (docs/IMPLEMENTATION.md Q11), and the static
# artifact covers everything older.
#
# The Debian version is the binary's --version with the first '-' turned into
# '~': "1.0.0-rc1" is a pre-release of "1.0.0" and must sort before it, and a
# plain '-' would make "rc1" a Debian revision, which sorts after.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:?usage: build-deb.sh BINARY OUTDIR}"
out="${2:?usage: build-deb.sh BINARY OUTDIR}"

[ -x "$binary" ] || { echo "build-deb: $binary is not an executable" >&2; exit 1; }
for tool in dpkg-deb dpkg-shlibdeps dpkg; do
  command -v "$tool" > /dev/null || { echo "build-deb: $tool not found (dpkg-dev)" >&2; exit 1; }
done

upstream="$("$binary" --version)"
version="${upstream/-/\~}"
arch="$(dpkg --print-architecture)"
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$root" log -1 --format=%ct 2>/dev/null || echo 0)}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
pkg="$work/pkg"
install -d "$pkg/DEBIAN" "$pkg/usr/bin" "$pkg/usr/share/doc/loadforge"
install -m 0755 "$binary" "$pkg/usr/bin/loadforge"
install -m 0644 "$root/README.md" "$root/NOTICE" "$pkg/usr/share/doc/loadforge/"
for doc in architecture safety telemetry-sources hardware-validation; do
  install -m 0644 "$root/docs/$doc.md" "$pkg/usr/share/doc/loadforge/"
done

cat > "$pkg/usr/share/doc/loadforge/copyright" <<'COPYRIGHT'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: LoadForge
Source: https://github.com/ivoitovych/LoadForge

Files: *
Copyright: 2026 Iaroslav Voitovych
License: GPL-3.0+

License: GPL-3.0+
 This program is free software: you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free Software
 Foundation, either version 3 of the License, or (at your option) any later
 version.
 .
 On Debian systems, the complete text of the GNU General Public License
 version 3 can be found in /usr/share/common-licenses/GPL-3.
COPYRIGHT
chmod 0644 "$pkg/usr/share/doc/loadforge/copyright"

# dpkg-shlibdeps wants a source tree with debian/control.
mkdir -p "$work/src/debian"
printf 'Source: loadforge\n\nPackage: loadforge\nArchitecture: any\n' > "$work/src/debian/control"
depends="$(cd "$work/src" && dpkg-shlibdeps -O -e"$pkg/usr/bin/loadforge" 2> "$work/shlibdeps.err" |
  sed -n 's/^shlibs:Depends=//p')" || {
  cat "$work/shlibdeps.err" >&2
  echo "build-deb: dpkg-shlibdeps failed" >&2
  exit 1
}
[ -n "$depends" ] || { echo "build-deb: dpkg-shlibdeps produced no Depends" >&2; exit 1; }

size_kib="$(du -sk --apparent-size "$pkg/usr" | cut -f1)"
cat > "$pkg/DEBIAN/control" <<CONTROL
Package: loadforge
Version: $version
Architecture: $arch
Maintainer: Iaroslav Voitovych <yaroslav.voytovych@gmail.com>
Installed-Size: $size_kib
Depends: $depends
Section: utils
Priority: optional
Homepage: https://github.com/ivoitovych/LoadForge
Description: self-verifying hardware reliability and load-testing tool
 LoadForge runs complete computational workloads on every CPU, checks every
 result against an independent reference, and records how the machine reacted
 (temperature, power, frequency, memory) on one timeline. A wrong result is
 reported as a finding and not as a crash.
 .
 This is the dynamically linked build. It needs the libraries listed under
 Depends; on older systems and on live media use the static build.
CONTROL

# Owner root:root without fakeroot; the timestamps are the commit's.
find "$pkg" -exec touch -h -d "@$epoch" {} +
mkdir -p "$out"
deb="$out/loadforge_${version}_${arch}.deb"
dpkg-deb --root-owner-group --build "$pkg" "$deb" > /dev/null
echo "$deb"
