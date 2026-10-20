#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The DCO gate: every commit a pull request contributes is signed off.
#
# WHY THIS IS A SCRIPT AND NOT SIX LINES IN A WORKFLOW
# ---------------------------------------------------
# It was six lines in a workflow, and the first pull request this repository
# ever opened failed it -- with every commit correctly signed off.
#
# On a `pull_request` event, actions/checkout does not check out the branch. It
# checks out `refs/pull/N/merge`: a synthetic commit GitHub creates by merging
# the head into the base, authored by GitHub and carrying no trailers. Walking
# `base..HEAD` therefore walks that merge commit too, and reported the project's
# own CI as an unsigned contribution.
#
# The failure mode matters more than the bug. A gate that fails every pull
# request regardless of the thing it checks does not stay a gate: it becomes the
# check everyone knows to ignore, and then it is worse than absent, because the
# green tick beside it is read as meaning something. Being wrong in the failing
# direction is not automatically safe.
#
# So the logic moved here, where tests/tools/test_tooling.sh can drive it against
# real repositories -- including a GitHub-shaped merge commit, which is the case
# that was never once exercised while the rule lived in YAML.
set -uo pipefail

# shellcheck source=tools/lib/commit-policy.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/commit-policy.sh" || exit 1

BASE="${1:?usage: check-dco.sh <base-ref> <head-ref> [repo]}"
HEAD_REF="${2:?usage: check-dco.sh <base-ref> <head-ref> [repo]}"
REPO="${3:-.}"

cd "$REPO" || { echo "check-dco: no repository at $REPO" >&2; exit 1; }

for ref in "$BASE" "$HEAD_REF"; do
  git rev-parse --verify --quiet "$ref^{commit}" >/dev/null || {
    echo "check-dco: '$ref' is not a commit in this repository." >&2
    echo "  The range could not be resolved, so nothing was checked. That is a" >&2
    echo "  failure, not a pass: a gate that cannot read its evidence must say so." >&2
    exit 1
  }
done

# A GitHub merge commit that adds no content -- two parents, a GitHub committer,
# a tree equal to the clean automatic merge of its parents -- cannot be signed
# off by anyone and is skipped (tools/lib/commit-policy.sh). That is a test of
# the tree, not of fields a forger can write: any other merge, whoever
# committed it, is a commit like the rest and needs its author's sign-off.
# The sign-off is the one the commit-msg hook requires: its email is the
# author's.
mapfile -t all < <(git rev-list "$BASE..$HEAD_REF")
commits=()
for c in "${all[@]}"; do
  cp_is_github_merge "$c" || commits+=("$c")
done

if [ "${#commits[@]}" -eq 0 ]; then
  # An empty range means this gate examined nothing. "I found no problems" and
  # "I had nothing to look at" must not share an exit code (docs/PLAN.md F20).
  echo "check-dco: no commits in $BASE..$HEAD_REF." >&2
  echo "  A pull request that contributes no commits is not something to certify." >&2
  exit 1
fi

missing=0
for commit in "${commits[@]}"; do
  # The trailer block is parsed properly, so a body that merely mentions the
  # words does not count as a sign-off.
  if git log -1 --format='%B' "$commit" | cp_signoff_matches "$(git log -1 --format='%ae' "$commit")"; then
    echo "  signed off: $(git log -1 --oneline "$commit")"
  else
    echo "  MISSING Signed-off-by: $(git log -1 --oneline "$commit")" >&2
    missing=$((missing + 1))
  fi
done

echo "DCO: ${#commits[@]} commit(s) checked, ${missing} unsigned"

if [ "$missing" -ne 0 ]; then
  echo "" >&2
  echo "Every commit needs a Signed-off-by trailer, certifying you have the right" >&2
  echo "to submit the work under the project's licence. Add it with:" >&2
  echo "    git commit -s            (when committing)" >&2
  echo "    git rebase --signoff $BASE   (to sign a series you already made)" >&2
  echo "See CONTRIBUTING.md. LoadForge uses the DCO, not a CLA." >&2
  exit 1
fi
