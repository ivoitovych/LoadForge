# LoadForge — Engineering Journal

**What this is:** the working knowledge that does not belong in a design document —
facts established by experiment rather than by reasoning, mistakes worth not repeating,
decisions whose *rejected alternatives* matter as much as the choice, and the habits that
turned out to earn their cost.

**Why it exists:** `PLAN.md` records what the project decided. `IMPLEMENTATION.md` records
what gets built and in what order. Neither records *what was learned along the way*, and
that knowledge is the first thing lost when work is picked up after a gap. A finding that
took a C probe and an hour to establish is worth two lines here forever.

**How to use it:** read §1 before writing code that touches the kernel or the toolchain —
several entries there contradict what the manual pages appear to say. Read §3 before
writing a test or a gate. Append to it when something surprises you; an entry costs two
minutes and saves the next person the experiment.

**How to add to it:** entries are dated by milestone, not by calendar. State the fact, then
how it was established. "I believe" is not an entry; "I ran this and got that" is.

---

## 1. Established by experiment — facts that contradict the obvious reading

These were all wrong in my head first, and each one was corrected by running something.
They are listed with the evidence, so that a future reader can re-run it rather than
trust it.

### 1.1 `open(2)` on a directory succeeds. `EISDIR` comes from `read(2)`.

The obvious reading — that opening a directory for reading fails — is wrong.
`open(path, O_RDONLY)` on a directory **succeeds** and returns a valid descriptor; the
error arrives on the first `read(2)`.

*Evidence:* a real-kernel test asserted `EISDIR` from the open step and failed. A
standalone C probe confirmed the descriptor is returned and `read` is what fails.

*Why it matters:* a file reader that only handles open-failure will hand a caller a
descriptor to a directory and then behave oddly on the read. The error-path taxonomy for
any file-reading code must place `EISDIR` under read, not open.

### 1.2 The dynamic loader is a `DT_NEEDED` entry on AArch64 and not on x86-64.

On x86-64 this project's binary reaches the loader through `PT_INTERP`, so it never
appears in the `DT_NEEDED` list. On AArch64 it *is* a `NEEDED` entry
(`ld-linux-aarch64.so.1`). A runtime-dependency allowlist built from x86-64 evidence
therefore fails on the first ARM run, which is exactly what happened.

*Evidence:* the gate's first ARM64 CI run reported
`THIRD-PARTY RUNTIME DEPENDENCY: ld-linux-aarch64.so.1`. Locally, `/usr/bin/gcc-13` on
x86-64 is a real binary that *does* list `ld-linux-x86-64.so.2` as `NEEDED`, which gave a
genuine local subject to test the fix against.

*The general lesson, which is the more valuable half:* **the evidence in front of you is a
smaller set of facts than the project ships on.** The allowlist was correct for every
binary I could inspect and still wrong for the project. Where a property varies by
architecture, toolchain or libc, either run the check on every matrix entry or state
explicitly that it is only checked on one.

### 1.3 `ldd` may execute the binary. `readelf` does not.

`ldd` resolves dependencies by, on some configurations, *running* the object. Running an
artifact in order to inspect it is poor practice generally and worse in CI, and it cannot
inspect a binary built for another architecture. `readelf -d` reads the ELF structure
without executing anything and works cross-architecture.

*Consequence:* any linkage check in this project uses `readelf`. `readelf -h` runs first,
to establish that the subject is an ELF object at all — otherwise a text file reports zero
dependencies and passes (§3.1).

### 1.4 glibc's GNU `strerror_r` never returns null or empty.

The defensive null-check on its return value is unreachable code. The coverage gate found
it, correctly.

*Consequence:* the branch was **deleted**, not marked excluded. See §3.4 on the exclusion
ladder.

### 1.5 `actions/checkout` checks out `refs/pull/N/merge` — a synthetic merge commit.

On a pull request, the checked-out `HEAD` is not the branch tip. GitHub creates a merge
commit between the head and the base, and that commit is authored by GitHub, carries no
sign-off, and is not in either branch's history.

*Symptom:* a sign-off gate walking `HEAD~n..HEAD` failed **every** pull request regardless
of whether the commits were signed off — including the pull request that introduced the
gate.

*Fix:* use the event payload's explicit `base.sha..head.sha` range and `--no-merges`. An
empty range fails rather than passes (§3.1 again).

*General lesson:* in CI, never assume the working tree corresponds to the branch under
test. Derive ranges from the event payload, not from the checkout.

### 1.6 Sanitizers need `libclang-rt-<version>-dev` to link locally.

ASan and TSan builds configure fine and fail at link without it. It is easy to conclude
"sanitizers only work in CI" and defer; installing the runtime package takes a minute and
keeps the local loop honest.

### 1.7 Under ASan and TSan, a child that raises `SIGSEGV` does not die by signal.

The sanitizer installs its own handler, prints its report, and calls `_exit`. So the child
**exits with a code** instead of being terminated by a signal, and any test asserting
"terminated by SIGSEGV" fails on exactly the two presets most likely to be run before a
release.

*Evidence:* a real-kernel test forking a child that raised `SIGSEGV` passed on the debug
preset and failed under both sanitizers, with the code under test behaving correctly the
whole time. The failure output was ASan's own SEGV report, followed by the assertion
failing because `termination()` said `kExited`.

*Rule:* a test that needs a genuinely fatal signal must use one the sanitizers do not
intercept — `SIGTERM` and `SIGUSR1` are safe, `SIGSEGV`, `SIGBUS`, `SIGFPE` and `SIGILL`
are not. Decoding of the intercepted signals is still testable; just not by raising them.

### 1.8 The `W*` wait-status macros do not partition the integers.

`WIFEXITED`, `WIFSIGNALED`, `WIFSTOPPED` and `WIFCONTINUED` between them leave
**16,777,215 of the 2³² possible status words** matching none of the four. The smallest
positive one is `0xff`.

*Evidence:* enumerated all 2³² values in a C loop, which takes about four seconds. This
started as an attempt to prove the opposite — that an "unrecognised" arm was dead code
that the exclusion ladder said to delete.

Two neighbours found the same way:

- `0x7f` — the stopped marker carrying signal 0, which is not a signal — classifies as
  **stopped**, because `WIFSTOPPED` tests only the low byte.
- `exit(256)` produces a status word of `0`, **indistinguishable from `exit(0)`**. Only
  the low 8 bits survive. Consequence for this project: no exit code above 255 may ever
  carry meaning, or a distinguished failure code would report itself as success.

*Why it matters beyond trivia:* `waitpid` never produces an unmatched word, so it is
tempting to treat the fall-through as unreachable. But the same decoder reads status words
back from a crash journal written by an earlier run, and a truncated record must classify
as "unknown" rather than as a clean exit.

---

## 2. Decisions, and the alternatives that were rejected

The choice is in `PLAN.md`; what follows is the reasoning that does not survive
summarisation, particularly the *rejected* option, which is the part a future reader is
most likely to re-propose.

### 2.1 Worker isolation: separate processes, one executable, `fork` **and** `exec`

**Chosen:** workers are separate processes, all of them the same executable started with
different command-line switches, created by `fork` followed by `exec` of `/proc/self/exe`.

**Rejected — threads:** a thread that dies takes the process with it, or worse, corrupts
it quietly. A failed *process* is observable by the supervisor through `waitpid` with a
status the supervisor can act on and report. For a tool whose entire purpose is to
survive and characterise failure, the failure of a worker must be a first-class,
observable event rather than an outcome that destroys the observer.

**Rejected — `fork` without `exec`:** a fork-only child inherits the parent's address
space, allocator state, sanitizer state and any threads' locks in whatever condition they
were in at fork time. `fork` after threads exist is a well-known source of deadlock in
exactly the paths this project stresses. `exec` gives a clean, reproducible starting
state, at the cost of re-parsing arguments — a cost worth paying.

**Rejected — separate executables per worker type:** one executable means one build, one
set of gates, one artifact to ship, and no possibility of a version skew between
supervisor and worker.

**Consequences accepted:**
- Shared state between supervisor and workers needs `PTHREAD_MUTEX_ROBUST` and correct
  `EOWNERDEAD` handling, since a worker can die holding a lock. This is not optional and
  is itself a tested path.
- Orphan prevention uses `PR_SET_PDEATHSIG`, which has a **race**: the parent can die
  between the fork and the child's `prctl` call. The child must re-check `getppid()`
  immediately after setting it and exit if it has already been reparented.
- Every supervision test runs with **N ≥ 2** workers. With one worker, "the supervisor
  noticed a death" and "the supervisor noticed the only child exited" are the same
  observation, and the test proves nothing about supervision.

### 2.2 A substitutable syscall seam, rather than mocking at a higher level

Error paths that depend on `errno` are unreachable from a test that goes through the real
kernel: you cannot reliably provoke `ENOMEM`, `EIO` or a short read on demand. A narrow
`Syscalls` interface — `open_read`, `read`, `close` — with a real implementation that does
**errno translation and nothing else** makes every one of those paths reachable by
injection.

The discipline that makes this honest: the real implementation must contain no logic worth
testing, or the seam becomes a way to avoid testing the interesting part. Everything above
the seam is tested through the fake; the seam itself is thin enough to read.

### 2.3 Allowlist, never denylist

Applied twice, in unrelated places, for the same reason: **a denylist catches only the
spellings someone thought of.**

- *Runtime dependencies:* the gate permits the C and C++ runtime by name and the dynamic
  loader by a narrow pattern, and rejects everything else. Adding to the list is a change
  to what the project promises its users, argued in an issue — not a formality.
- *Any similar "is this acceptable" check:* state the acceptable set. A list of forbidden
  things is always incomplete and gives false confidence proportional to its length.

### 2.4 Vendored dependencies are pinned by a tree digest, not a version string

A version string identifies what was *intended*. For vendored source there is no package
manager to verify that intention, so the pin is a `files_sha256` — sha256 over the sorted
`<relative-path> <file-sha256>` lines of the vendored tree. That is the vendored
equivalent of a lockfile hash: it detects a local edit, a partial update, or a silently
different upstream tarball.

The first pin is established by trust-on-first-use — there is no way around that — but it
is recorded once, and every subsequent change to the tree is then visible in review.

### 2.5 `core::Ok` rather than `core::Unit`

A trivial naming point recorded because it will otherwise be re-proposed: `Unit` was
already taken in this codebase, by the unit-suffix parsers in `duration.cpp` and
`byte_size.cpp`. Two different `Unit` types in one project is a permanent source of
misreading.

---

## 3. The verification doctrine, and how it was learned

This is the part of the project most likely to be eroded by someone in a hurry, so the
reasoning is recorded rather than only the rule.

### 3.1 F20 — a gate that cannot read its evidence must **fail**

The failure mode is subtle and was present in several gates at once: a check that finds no
problems and a check that could not look report the same thing — success. Silence is not
evidence of absence.

Concretely, this means:
- A missing artifact is a failure, not a pass. Nothing was inspected, so nothing is
  certified.
- A missing tool (`readelf` not installed) is a failure. A gate that cannot run has not
  run.
- An empty range, an empty parse, or a shape the parser does not recognise is a failure.
- A file that is not the kind of thing the gate inspects (a text file where an ELF object
  was expected) is a failure, not a clean report of zero findings.
- Totals are reconciled against the evidence's *own* declared totals where the evidence
  declares them, and against completion markers where it does not. A truncated report must
  not read as a short clean one.

### 3.2 F21 — a model of an external tool cannot falsify itself

A gate parsing another tool's output was wrong about that tool's mutator identifiers. It
had a full fixture suite, and every fixture agreed with it — because the fixtures were
written from the same misunderstanding as the parser.

**Fixtures for an external tool's output must be produced by that tool**, captured and
committed, not composed by hand. Hand-written fixtures test the author's model of the
tool, and a model cannot falsify itself.

The same principle produced the rule that the runtime-dependency tests build **real
binaries** and inspect a **real system binary** for the loader case, rather than asserting
against soname strings typed into the test — typing the string is precisely the error that
caused the ARM64 failure in the first place.

### 3.3 A gate that has never failed has not been tested

Write the gate's *failing* case first, and make it impossible for that case to be quietly
skipped. In the runtime-dependency tests, the case that proves the gate can fail links a
genuine third-party library; if that library is unavailable in the environment, the test
counts a **failure**, not a skip, with a message saying why. A skip in the one case that
demonstrates the gate has teeth is indistinguishable from a gate with no teeth.

Corollary, learned from a real defect: a tier check that asserted the existence of a file
which always exists could never fail. Every assertion should be checked against the
question "what tree would make this fail?" — and if there isn't one, the assertion is
decoration.

### 3.4 The exclusion ladder — prefer deleting unreachable code to excluding it

When the 100% coverage gate rejects a line, work down this ladder and stop at the first
rung that applies:

1. **Test it.** Most rejections are a missing test, not a gate problem.
2. **Make it reachable** — usually by moving the dependency behind a seam (§2.2).
3. **Delete it.** If the line cannot execute, it is not defensive; it is dead. This is
   where the `strerror_r` null-check went (§1.4).
4. **Exclude it, with a written justification naming why rungs 1–3 do not apply.**

Rung 4 is the last resort and each use is a small permanent debt. A project that reaches
for it first ends up with a 100% figure that means nothing.

### 3.5 The coverage obligation: executed, asserted, falsifiable

A line is covered when it has been **executed**, when something **asserted** about the
resulting behaviour, and when the test **fails if that behaviour changes**. Execution
alone is what a coverage tool measures and is the weakest of the three. The third is what
mutation testing checks, and it is the reason the mutation gate exists.

The owner's requirement, now normative in `IMPLEMENTATION.md` §2, is stronger than line
coverage: for each line the tests must exercise the distinct *meanings* its inputs can
take — every boundary of every value domain it touches, every way each call it makes can
fail, every corner case where behaviour changes.

### 3.6 The path taxonomy, and why a coverage tool cannot discharge the obligation

Seven kinds of execution path (P1–P7): decision, error, boundary, verification-outcome,
concurrency, lifecycle, capability. **Only P1 is enumerable by a coverage tool.** A branch
report says nothing about whether every `errno` a call can return was exercised, whether
both sides of a boundary were probed, whether a worker's death mid-run was simulated, or
whether the absent-capability path was taken.

That is why the per-module obligation ledger exists as a *separate, checked* artifact:
what a tool cannot enumerate, a human declares and a gate holds them to.

### 3.7 A coverage gate can report 100% over code it has never seen

The most instructive F20 case so far, because the gate was not wrong about anything it
measured — it simply did not measure everything, and said nothing about the gap.

`gcovr` reports on the `.gcda` files it finds. A source file that was never compiled into
the measured build produces none, so it is **not reported as uncovered — it is not
reported at all**, and 100% of what remains is still 100%. The percentage is true and
worthless.

*Evidence:* the gate printed `lines: 100.0% (301 out of 301)` over a build directory that
predated a newly added file. Once the file was genuinely compiled in, the real figure was
99.7%, with an uncovered branch that then needed real work.

The part worth generalising is **why it survived so long**: CI configures and builds from
a fresh checkout on every run, so CI can never hit it. *The one place the hazard does not
exist is the one place the gate runs automatically.* Locally — where a contributor forms
their confidence before pushing — a stale build directory is the normal state.

So: when a gate is safe in CI by accident of environment rather than by construction, it
is not safe. Ask what the gate reads, and what a *missing* input makes it say.

The fix compares the report against the **source tree**, not against a checked-in list. A
list would need updating by the same person who forgot to rebuild.

---

## 4. Recurring mistakes, and the structural fixes

Each of these was made more than once, which is the signal that the fix must be structural
rather than a resolution to be more careful.

### 4.1 Reading `$?` after an intervening command — made three times

```bash
run_something
expect 0 "first assertion"     # <- this consumes and replaces $?
[ $? -eq 0 ] && ...            # <- reads the exit status of `expect`
```

**Fix:** a `run_gate` helper that captures both the output and the status into `RC` and
`OUT` in one place, and assertions that read only `RC` and `OUT`. The fix is to make the
mistake impossible rather than to remember not to make it.

The generalisation is worth stating: **after the second occurrence of a bug, stop fixing
the instance and change the shape that permits it.**

### 4.2 A test double more permissive than the thing it models

A fake syscall layer was laxer than the kernel in three distinct ways: it ignored the
descriptor entirely (so a read on a foreign or closed descriptor succeeded, where the
kernel returns `EBADF`), it silently discarded the remainder of a short read, and it
counted close *successes* where the code under test needed close *attempts*.

The middle one was measured: **1904 of 6000 bytes vanished** in a test that reported
success.

**Fix, and the standard for every future double:** a fake must be *at least as strict* as
the real thing on every dimension the code under test can observe. Where it is not, tests
pass against behaviour the kernel will reject. Before fixing the third defect, it was
first demonstrated *failing* — a fake's defect deserves the same "demonstrate the failure"
discipline as a product defect.

### 4.3 Unbounded retry on `EINTR`

A read loop retried on `EINTR` without limit — correct in the normal case, a hang under a
pathological signal storm, in a tool that is deliberately run on machines behaving
pathologically.

**Fix:** bound the *consecutive* run (128), not the total, and reset the counter on any
successful read. Bounding the total would break a long legitimate read on a busy system;
bounding the run catches the case where no progress is being made. Report the abandonment
with the count, so the log distinguishes it from an ordinary `EINTR`.

### 4.4 Attributing an error to a syscall that did not fail

A file-size limit returned `EFBIG` attributed to `read` — but no `read` had failed; the
reader's own policy rejected the file. **Attribute an error to the operation that actually
produced it** (`read_file`, the reader's own operation). A caller debugging a real `read`
failure should never be shown a fabricated one.

### 4.5 Assuming a pinned tool version is optional

Two tools are pinned (the coverage reporter and the Python linter) for the same reason:
their *output format* changes between releases. Unpinned, `format --diff` fails on a tree
nobody touched, and a coverage report parses differently than the gate expects. A pin here
is not conservatism; it is the difference between a gate that measures the code and a gate
that measures the runner's package index.

### 4.6 A test that asserts a failure can pass for the wrong reason

The subtlest of these, because the test is green and the assertion looks specific.

A test fed a coverage gate a real report with **one** file's entry removed, and asserted
that the gate failed and that its message named that file. Both were satisfied — while the
gate was broken and reporting *every* file as missing. The test passed, and would have gone
on passing for a gate that could only ever say "everything is missing".

**A test that asserts a failure must also assert that the failure is the right one.** The
fix was one more assertion, that the count was exactly one. Concretely, prefer:

- the exact count, not "at least one";
- the specific message, not merely non-empty output;
- the specific exit status, not merely non-zero.

This is the negative-path twin of §3.3. A gate that has never failed has not been tested —
and a gate whose failure was checked only loosely has been tested for the wrong thing.

---

## 5. Process knowledge

### 5.1 Everything through pull requests, and why it is worth the friction

Every change goes through a pull request so that CI runs on it before it reaches `main`.
The friction is real — a fix that would be one commit becomes a branch, a push and a wait
— and it has already paid for itself twice: the ARM64 loader difference (§1.2) and the
merge-commit sign-off failure (§1.5) were both found by CI on a pull request, and neither
was findable locally.

### 5.2 Parallel work is possible even with an unmerged pull request under review

Work that touches disjoint files can proceed on a branch cut from `main` while an earlier
pull request waits for review. Two practical rules learned:

- **Do not edit a file an open pull request is already touching.** A shared test runner
  edited by two branches is a merge conflict for no benefit; a second test file that can
  be merged into the first later costs nothing.
- **Cut from `main`, not from the open branch**, unless there is a real dependency. A
  branch stacked on an unmerged branch cannot merge until its parent does.

### 5.3 Some things genuinely require the repository owner

Not everything is a matter of trying harder. Manually dispatching a workflow returns a
`403` with the available token. Branch protection settings and merges are the owner's.
When a task is blocked on one of these, say so plainly with the exact action needed,
rather than working around it — a workaround here means an ungated change.

### 5.4 Write the gate before the thing it gates

The runtime-dependency promise sat in the design document for several revisions, described
as "enforced by a CI check", with no such check existing. The only linkage test asserted
that the *static* artifact was static — true, and silent about the dynamic build, which is
what a distribution actually packages.

**A promise in a document with no gate behind it is a promise that has never been tested.**
When a document claims something is enforced, the first question is *by what*, and the
answer must be a file path.

### 5.5 A fix to a shared gate protects nothing until it reaches `main`

CI runs the *merge* of the pull request onto its base, so the version of a gate that judges
a branch is the base branch's version plus whatever that branch changes. A fix living only
in an unmerged branch therefore protects only that branch.

This was not a hypothesis. The sign-off gate's fix (§1.5) sat on two unmerged branches, and
the next pull request opened from `main` failed on the identical bug — the same synthetic
merge commit, the same misleading message — while a correctly signed-off commit sat
underneath it.

**Two practical consequences:**

- Fixes to shared infrastructure are worth merging ahead of the feature work that
  motivated them. Every parallel branch pays the tax again until they land.
- Until then, port the fix into each affected branch rather than waiting. A cherry-pick of
  the same patch no-ops once the base carries it, and waiting for one's own pull request to
  merge is still waiting.

The corollary is worth stating separately, because it is easy to get backwards: a red check
on a branch is not automatically that branch's defect. Before changing anything, establish
whether the same failure reproduces on the base — if it does, the branch is a bystander, and
"fixing" it locally would mean changing code that was never wrong.

---

## 6. Open threads

Kept short and current; move an item to the relevant document once it is settled.

- **Awaiting the owner:** review/merge of the platform-seam pull request; branch protection
  on `main` (required status checks, no required approvals — a solo maintainer would
  otherwise block themselves); one manual workflow run to establish the mutation-tool pin,
  which is a hard prerequisite for the first workload milestone.
- **Unanswered question:** whether CI should hard-require a specific committer identity.
  Doing so would close the repository to outside contributors, so the current check
  verifies *shape* — one author, complete identity — and not a particular person.
- **Next in sequence:** worker process management (§2.1), then the clock abstraction, then
  topology discovery.
