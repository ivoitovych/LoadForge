#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs the package, runs the installed copy, removes it, and checks nothing
# is left. Needs root (dpkg -i): in CI run it with sudo.
#
#   packaging/test-deb.sh PACKAGE.deb
#
# Exit code: 0 installed, ran clean, removed without a trace; anything else, the
# first thing that was not so. The package is removed on the way out whatever
# happened, so a failed run does not leave the machine changed.
set -euo pipefail

deb="${1:?usage: test-deb.sh PACKAGE.deb}"
[ "$(id -u)" -eq 0 ] || { echo "test-deb: dpkg -i needs root" >&2; exit 1; }
[ -f "$deb" ] || { echo "test-deb: $deb does not exist" >&2; exit 1; }

if dpkg -s loadforge > /dev/null 2>&1; then
  echo "test-deb: loadforge is already installed; refusing to replace it" >&2
  exit 1
fi

log="$(mktemp)"
trap 'dpkg --purge loadforge > /dev/null 2>&1 || true; rm -f "$log"' EXIT

want="$(dpkg-deb --field "$deb" Version)"
echo "== package: $(dpkg-deb --field "$deb" Package) $want ($(dpkg-deb --field "$deb" Architecture))"
echo "   Depends: $(dpkg-deb --field "$deb" Depends)"

dpkg -i "$deb"
[ "$(command -v loadforge)" = /usr/bin/loadforge ] || { echo "test-deb: loadforge is not /usr/bin/loadforge" >&2; exit 1; }

reported="$(loadforge --version)"
echo "== installed: loadforge --version -> $reported"
[ "${reported/-/\~}" = "$want" ] || { echo "test-deb: version '$reported' does not match package '$want'" >&2; exit 1; }

# No library the package did not ask for: every dependency resolves.
if ldd /usr/bin/loadforge | grep -q 'not found'; then
  ldd /usr/bin/loadforge >&2
  echo "test-deb: the installed binary has an unresolved library" >&2
  exit 1
fi

loadforge stress --test sort --duration 2s > "$log" 2>&1 || {
  tail -n 30 "$log" >&2
  echo "test-deb: the installed loadforge failed a two-second stress run" >&2
  exit 1
}
grep -q '^verdict: clean$' "$log" || { echo "test-deb: no 'verdict: clean'" >&2; exit 1; }
echo "== installed: two-second stress run, verdict: clean"

files="$(dpkg -L loadforge | grep -v '^/\.$')"
dpkg --purge loadforge
left=0
while IFS= read -r f; do
  if [ -f "$f" ] || [ -L "$f" ]; then
    echo "test-deb: left behind after purge: $f" >&2
    left=1
  fi
done <<< "$files"
if command -v loadforge > /dev/null; then
  echo "test-deb: loadforge is still on the PATH after purge" >&2
  left=1
fi
[ "$left" -eq 0 ] || exit 1
echo "== removed: no file of the package is left"
