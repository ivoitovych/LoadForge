#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The identity gate: every commit in the history of a given commit was authored
# and committed by an identity the project has admitted, is unsigned, and
# carries no trailer but Signed-off-by.
#
# WHY CI, WHEN LOCAL HOOKS ALREADY CHECK THIS
# -------------------------------------------
# Local hooks are a convenience and every one of them can be skipped:
# `git commit --no-verify` skips the commit hooks and `git push --no-verify`
# skips the push hook. This is the check that cannot be skipped, because it
# runs on the evidence after it has arrived.
#
# AN ALLOWLIST, NOT A LIST OF NAMES TO REFUSE
# -------------------------------------------
# Identities are admitted by email in .github/commit-identities. Anything not
# admitted fails, whoever it is, so the gate needs no list of identities to
# keep out, and the repository never has to spell one. Trailers are admitted
# the same way: Signed-off-by is the only one a commit may carry, so a
# co-author line, a session link, or any trailer nobody anticipated fails
# without being named.
#
# GITHUB'S OWN MERGE COMMITS
# --------------------------
# A pull request merged on GitHub produces a merge commit whose committer is
# GitHub, signed with GitHub's key, whose author is whoever pressed the button,
# and whose body begins with the PR title, which git may parse as a trailer
# ("Config: a typo is a refusal"). For exactly that shape, two parents and a
# GitHub committer, the author must still be admitted; the committer, the
# signature and the trailers belong to GitHub and are not judged. A GitHub
# committer on any other commit is not that shape and fails.
set -uo pipefail

HEAD_REF="${1:?usage: check-commit-identity.sh <head-ref> [repo] [identities-file] [exclude-spec]}"
REPO="${2:-.}"
IDENTITIES="${3:-.github/commit-identities}"
# Optional: a rev-list exclusion such as --remotes=origin, so that the pre-push
# hook judges only the commits a push introduces. CI passes none and walks the
# whole history.
EXCLUDE="${4:-}"
GITHUB_COMMITTER="noreply@github.com"

cd "$REPO" || { echo "check-commit-identity: no repository at $REPO" >&2; exit 1; }

git rev-parse --verify --quiet "$HEAD_REF^{commit}" >/dev/null || {
  echo "check-commit-identity: '$HEAD_REF' is not a commit in this repository." >&2
  echo "  Nothing was checked, which is a failure, not a pass." >&2
  exit 1
}

[ -r "$IDENTITIES" ] || {
  echo "check-commit-identity: cannot read $IDENTITIES." >&2
  exit 1
}
mapfile -t admitted < <(grep -vE '^\s*(#|$)' "$IDENTITIES" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')
if [ "${#admitted[@]}" -eq 0 ]; then
  echo "check-commit-identity: $IDENTITIES admits nobody; every commit would fail." >&2
  exit 1
fi

is_admitted() { # email
  local wanted
  wanted="$(echo "$1" | tr '[:upper:]' '[:lower:]')"
  local e
  for e in "${admitted[@]}"; do [ "$e" = "$wanted" ] && return 0; done
  return 1
}

if [ -n "$EXCLUDE" ]; then
  mapfile -t commits < <(git rev-list "$HEAD_REF" --not "$EXCLUDE")
else
  mapfile -t commits < <(git rev-list "$HEAD_REF")
fi
bad=0
report() { echo "  REFUSED $(git log -1 --format='%h %s' "$1"): $2" >&2; bad=$((bad + 1)); }

for c in "${commits[@]}"; do
  author="$(git log -1 --format='%ae' "$c")"
  committer="$(git log -1 --format='%ce' "$c")"
  parents="$(git log -1 --format='%p' "$c" | wc -w)"

  is_admitted "$author" || report "$c" "author <$author> is not in $IDENTITIES"

  if [ "$committer" = "$GITHUB_COMMITTER" ] && [ "$parents" -eq 2 ]; then
    continue  # GitHub's merge commit: only the author is ours to judge.
  fi

  is_admitted "$committer" || report "$c" "committer <$committer> is not in $IDENTITIES"

  git cat-file commit "$c" | sed '/^$/q' | grep -q '^gpgsig' &&
    report "$c" "signed; commits here are unsigned"

  while IFS= read -r key; do
    [ -z "$key" ] || [ "$key" = "Signed-off-by" ] || report "$c" "trailer '$key'; only Signed-off-by is allowed"
  done < <(git log -1 --format='%(trailers:only,unfold)' "$c" | sed -n 's/^\([^:]*\):.*/\1/p')

  # A "-by:" line outside the trailer block is not a trailer to git, but it
  # reads as one. Anywhere in the message, Signed-off-by is the only one.
  while IFS= read -r key; do
    report "$c" "a '$key' line; only Signed-off-by is allowed"
  done < <(git log -1 --format='%B' "$c" | grep -oiE '^[a-z][a-z0-9-]*-by:' | grep -viE '^signed-off-by:$')
done

echo "identity: ${#commits[@]} commit(s) checked, ${bad} problem(s)"
[ "$bad" -eq 0 ]
