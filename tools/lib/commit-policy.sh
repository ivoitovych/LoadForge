# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The commit predicates every gate shares. Sourced, never run: the commit-msg
# and pre-push hooks and the two CI scripts (check-commit-identity.sh,
# check-dco.sh) all read these functions, so a rule cannot be tightened in one
# place and forgotten in another. Run inside the repository being judged.
#
# THE SIGN-OFF RULE
# -----------------
# A commit carries a Signed-off-by trailer whose email is its author's.
#
# THE MERGE EXEMPTION
# -------------------
# GitHub's own merge of a pull request is committed and signed by GitHub, and
# its message begins with the pull request title, which git may parse as a
# trailer. Nobody can sign such a commit off, so it is exempt from the
# committer, signature, trailer and sign-off checks. But "two parents and a
# GitHub committer" are writable fields: anyone can forge them. What cannot be
# forged is the tree. A commit is exempt only when
#   * it has exactly two parents,
#   * its committer is GitHub, and
#   * its tree is exactly the clean automatic merge of its parents
#     (git merge-tree --write-tree; a conflicted merge exits non-zero and
#     never qualifies).
# Such a commit adds no authored content, so no trust in its committer is
# needed. Its author is still judged by the identity gate. Every other merge,
# whoever committed it, is judged like any commit, sign-off included.
# Needs git >= 2.38 (merge-tree --write-tree); ubuntu-24.04 ships 2.43+.

CP_GITHUB_COMMITTER="noreply@github.com"

cp_lower() { tr '[:upper:]' '[:lower:]'; }

# cp_signoff_matches <author-email>   (message on stdin)
# Succeeds when a Signed-off-by trailer names the author's email.
cp_signoff_matches() {
  local author email trailers
  author="$(printf '%s' "$1" | cp_lower)"
  trailers="$(git stripspace --strip-comments | git interpret-trailers --parse)"
  while IFS= read -r email; do
    [ "$(printf '%s' "$email" | cp_lower)" = "$author" ] && return 0
  done < <(sed -n 's/^Signed-off-by:.*<\(.*\)>.*/\1/p' <<< "$trailers")
  return 1
}

# cp_is_clean_merge <commit>: two parents, tree equals the automatic merge.
cp_is_clean_merge() {
  local parents merged
  read -r -a parents < <(git log -1 --format='%P' "$1")
  [ "${#parents[@]}" -eq 2 ] || return 1
  merged="$(git merge-tree --write-tree "${parents[0]}" "${parents[1]}" 2>/dev/null)" || return 1
  [ "${merged%%$'\n'*}" = "$(git rev-parse "$1^{tree}")" ]
}

# cp_is_github_merge <commit>: the exempt shape described above.
cp_is_github_merge() {
  [ "$(git log -1 --format='%ce' "$1" | cp_lower)" = "$CP_GITHUB_COMMITTER" ] &&
    cp_is_clean_merge "$1"
}
