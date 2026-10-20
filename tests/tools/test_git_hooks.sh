#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for tools/git-hooks and tools/install-git-hooks.sh.
#
# The hooks enforce at commit and push time what tools/check-commit-identity.sh
# enforces in CI. Each case builds a throwaway repository, installs the hooks
# with the installer under test, and runs real git commands, so a refusal here
# is git refusing, not a function returning a code.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The caller's own git configuration must not leak into the cases.
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
unset GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL GIT_DIR GIT_WORK_TREE
pass=0; fail=0
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

expect() { # description, expected-exit, actual-exit, [needle, output]
  if [ "$2" -eq "$3" ] && { [ $# -lt 5 ] || grep -q "$4" <<< "$5"; }; then
    echo "  PASS  $1"; pass=$((pass + 1))
  else
    echo "  FAIL  $1 (expected exit $2, got $3)"; fail=$((fail + 1))
    [ $# -ge 5 ] && printf '%s\n' "$5" | sed 's/^/          /'
  fi
}

OWNER_N="Owner"; OWNER_E="owner@example.invalid"
STRANGER_E="stranger@example.invalid"

# A repository with the project's tooling copied in, the hooks installed, an
# admitted identity configured, and a bare repository as origin.
mkrepo() { # name
  local r="$tmp/$1"
  git init -q -b main "$r"
  mkdir -p "$r/.github" "$r/tools"
  printf '# admitted\n%s\n' "$OWNER_E" > "$r/.github/commit-identities"
  cp "$ROOT/tools/check-commit-identity.sh" "$ROOT/tools/install-git-hooks.sh" "$r/tools/"
  cp -r "$ROOT/tools/git-hooks" "$ROOT/tools/lib" "$r/tools/"
  git -C "$r" config user.name "$OWNER_N"
  git -C "$r" config user.email "$OWNER_E"
  git -C "$r" config commit.gpgsign false
  "$r/tools/install-git-hooks.sh" >/dev/null
  git init -q --bare "$tmp/$1.git"
  git -C "$r" remote add origin "$tmp/$1.git"
  echo "$r"
}
commit() { # repo, message  -> exit status of git commit
  echo x >> "$1/f"; git -C "$1" add f
  out="$(git -C "$1" commit -q -m "$2" 2>&1)"
}
SOB=$'\n\nSigned-off-by: Owner <owner@example.invalid>'

echo "pre-commit"
r="$(mkrepo a)"
commit "$r" "first$SOB"
expect "an admitted identity commits"                          0 $?
git -C "$r" config user.email "$STRANGER_E"
commit "$r" "second$SOB"
expect "an identity not in the allowlist is refused"           1 $? "stranger@example.invalid" "$out"
git -C "$r" config user.email "$OWNER_E"
GIT_COMMITTER_EMAIL="$STRANGER_E" commit "$r" "third$SOB"
expect "a committer not in the allowlist is refused"           1 $? "COMMITTER" "$out"
git -C "$r" config commit.gpgsign true
commit "$r" "fourth$SOB"
expect "a signing configuration is refused"                    1 $? "gpgsign" "$out"
git -C "$r" config commit.gpgsign false

echo "commit-msg"
commit "$r" "five"
expect "a missing sign-off is refused"                         1 $? "Signed-off-by" "$out"
commit "$r" $'five\n\nCo-authored-by: Someone <s@example.invalid>\nSigned-off-by: Owner <owner@example.invalid>'
expect "a Co-authored-by trailer is refused"                   1 $? "Co-authored-by" "$out"
commit "$r" $'five\n\nSession: https://example.invalid/s/1\nSigned-off-by: Owner <owner@example.invalid>'
expect "an unanticipated trailer is refused"                   1 $? "Session" "$out"
commit "$r" $'five\n\nSigned-off-by: Owner <stranger@example.invalid>'
expect "a sign-off for someone else is refused"                1 $? "Signed-off-by" "$out"
commit "$r" $'buried\n\nCo-authored-by: Someone <s@example.invalid>\n\nmore prose'"$SOB"
expect "a co-author line above the trailer block is refused"   1 $? "Co-authored-by" "$out"
commit "$r" "six: prose mentioning Co-authored-by is not a trailer$SOB"
expect "a sign-off in the right form is accepted"              0 $?

echo "pre-push"
git -C "$r" checkout -q -b feature/ok
out="$(git -C "$r" push -q -u origin feature/ok 2>&1)"
expect "an admitted history on a conventional branch pushes"   0 $? "" "$out"
git -C "$r" checkout -q -b Bad_Branch
out="$(git -C "$r" push -q origin Bad_Branch 2>&1)"
expect "a branch name outside the convention is refused"       1 $? "Bad_Branch" "$out"
out="$(git -C "$r" push -q origin Bad_Branch:main 2>&1)"
expect "pushing to main is allowed by name"                    0 $? "" "$out"
git -C "$r" checkout -q feature/ok
# Make a commit by an identity outside the allowlist, bypassing the commit hooks.
echo y >> "$r/f"; git -C "$r" add f
GIT_AUTHOR_EMAIL="$STRANGER_E" git -C "$r" commit -q --no-verify -m "outsider$SOB"
out="$(git -C "$r" push -q origin feature/ok 2>&1)"
expect "a pushed commit by a non-admitted author is refused"   1 $? "stranger@example.invalid" "$out"
git -C "$r" reset -q --hard HEAD~1
echo z >> "$r/f"; git -C "$r" add f
git -C "$r" commit -q --no-verify -m "no trailer-free sign-off" -m "Co-authored-by: Someone <s@example.invalid>"
out="$(git -C "$r" push -q origin feature/ok 2>&1)"
expect "a pushed commit with a co-author trailer is refused"   1 $? "Co-authored-by" "$out"
git -C "$r" reset -q --hard HEAD~1
out="$(git -C "$r" push -q origin :feature/ok 2>&1)"
expect "deleting a remote branch is not judged"                0 $? "" "$out"
# Only the commits a push introduces are judged: old history that the remote
# already holds is not re-examined.
git -C "$r" checkout -q -b fix/new
commit "$r" "seven$SOB"
out="$(git -C "$r" push -q origin fix/new 2>&1)"
expect "a new branch is judged on its own commits"             0 $? "" "$out"

echo "pre-push: sign-off and merges, hooks bypassed with --no-verify"
r="$(mkrepo e)"
commit "$r" "base$SOB"
git -C "$r" push -q origin main 2>/dev/null
GH=noreply@github.com
nv() { # message : a commit made past the commit hooks
  echo "$RANDOM" >> "$r/f"; git -C "$r" add f
  git -C "$r" commit -q --no-verify -m "$1"
}
try() { git -C "$r" push -q origin "HEAD:refs/heads/$1" 2>&1; }
git -C "$r" checkout -q -B fix/nosob main
nv "no sign-off"
out="$(try fix/nosob)"
expect "a commit with no sign-off is refused"                  1 $? "no Signed-off-by" "$out"
git -C "$r" checkout -q -B fix/othersob main
nv $'other\n\nSigned-off-by: Other <other@example.invalid>'
out="$(try fix/othersob)"
expect "a sign-off with another email is refused"              1 $? "no Signed-off-by for <owner@example.invalid>" "$out"
git -C "$r" checkout -q -B fix/goodsob main
nv "right$SOB"
out="$(try fix/goodsob)"
expect "a correct sign-off is accepted"                        0 $? "" "$out"

# Merges: build the two sides, then commits with every field set by hand.
git -C "$r" checkout -q -B side-a main; nv "a$SOB"; a="$(git -C "$r" rev-parse HEAD)"
git -C "$r" checkout -q -B side-b main
echo b > "$r/g"; git -C "$r" add g; git -C "$r" commit -q --no-verify -m "b$SOB"; b="$(git -C "$r" rev-parse HEAD)"
mkmerge() { # author-email committer-email message [extra-file]
  local tree idx
  tree="$(git -C "$r" merge-tree --write-tree "$a" "$b" | head -1)"
  if [ -n "${4-}" ]; then
    idx="$(mktemp -u)"; GIT_INDEX_FILE="$idx" git -C "$r" read-tree "$tree"
    GIT_INDEX_FILE="$idx" git -C "$r" update-index --add --cacheinfo \
      "100644,$(echo smuggled | git -C "$r" hash-object -w --stdin),$4"
    tree="$(GIT_INDEX_FILE="$idx" git -C "$r" write-tree)"; rm -f "$idx"
  fi
  GIT_AUTHOR_NAME=A GIT_AUTHOR_EMAIL="$1" GIT_COMMITTER_NAME=C GIT_COMMITTER_EMAIL="$2" \
    git -C "$r" commit-tree "$tree" -p "$a" -p "$b" -m "$3"
}
push_sha() { git -C "$r" push -q origin "$1:refs/heads/$2" 2>&1; }

sha="$(mkmerge "$OWNER_E" "$GH" $'Merge pull request #9\n\nCo-authored-by: Someone <s@example.invalid>' smuggled.txt)"
out="$(push_sha "$sha" fix/forged)"
expect "a forged GitHub merge adding content is refused"       1 $? "Co-authored-by" "$out"
expect "  ... also for want of a sign-off"                     1 1 "no Signed-off-by" "$out"
sha="$(mkmerge "$OWNER_E" "$GH" "Merge pull request #9")"
out="$(push_sha "$sha" fix/genuine)"
expect "a genuine clean GitHub merge with no trailers is accepted" 0 $? "" "$out"
sha="$(mkmerge "$OWNER_E" other@example.invalid "Merge x$SOB" extra.txt)"
out="$(push_sha "$sha" fix/unadmitted)"
expect "a merge with an unadmitted committer and content is refused" 1 $? "committer <other@example.invalid>" "$out"
sha="$(mkmerge "$OWNER_E" "$OWNER_E" "Merge main$SOB")"
out="$(push_sha "$sha" fix/human)"
expect "a human merge with a sign-off is accepted"             0 $? "" "$out"
sha="$(mkmerge "$OWNER_E" "$OWNER_E" "Merge main")"
out="$(push_sha "$sha" fix/humannosob)"
expect "a human merge without a sign-off is refused"           1 $? "no Signed-off-by" "$out"

echo "install-git-hooks.sh"
r="$(mkrepo b)"
hd="$r/.git/hooks"
before="$(cat "$hd"/pre-commit "$hd"/commit-msg "$hd"/pre-push | md5sum)"
"$r/tools/install-git-hooks.sh" >/dev/null; rc=$?
after="$(cat "$hd"/pre-commit "$hd"/commit-msg "$hd"/pre-push | md5sum)"
[ "$before" = "$after" ]; same=$?
expect "a second run succeeds"                                 0 $rc
expect "a second run changes nothing"                          0 $same
ok=0; for h in pre-commit commit-msg pre-push; do
  [ -x "$hd/$h" ] && grep -qx '# LOADFORGE-REPO-HOOK' "$hd/$h" || ok=1; done
expect "the hooks are executable and carry the marker"         0 $ok
cfg_before="$(git -C "$r" config --local --list | md5sum)"
"$r/tools/install-git-hooks.sh" >/dev/null
[ "$cfg_before" = "$(git -C "$r" config --local --list | md5sum)" ]
expect "the installer changes no git configuration"            0 $?

r="$(mkrepo c)"
printf '#!/bin/sh\necho mine\n' > "$r/.git/hooks/pre-commit"; chmod +x "$r/.git/hooks/pre-commit"
out="$("$r/tools/install-git-hooks.sh" 2>&1)"; rc=$?
expect "a foreign hook is reported"                            0 $rc "not ours" "$out"
grep -q 'echo mine' "$r/.git/hooks/pre-commit"
expect "a foreign hook is left untouched"                      0 $?
grep -qx '# LOADFORGE-REPO-HOOK' "$r/.git/hooks/pre-push"
expect "the other hooks are still installed"                   0 $?

r="$(mkrepo d)"
commit "$r" "base$SOB"
git -C "$r" worktree add -q "$tmp/d-wt" -b feature/wt
echo w >> "$tmp/d-wt/f"; git -C "$tmp/d-wt" add f
git -C "$tmp/d-wt" -c user.email="$STRANGER_E" commit -q -m "wt$SOB" 2>/dev/null
expect "a linked worktree is covered by the same hooks"        1 $?

echo; echo "  git hook tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
