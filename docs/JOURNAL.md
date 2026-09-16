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

### 1.9 Coverage counters from a forked child are discarded.

A child that ends with `_exit(2)` skips the atexit handlers — including the one gcov
installs to write its counters. **Anything executed only inside a forked child is absent
from the coverage report**, and absent reads as "never executed", not as "lost data".

*Evidence:* the first version of the worker-launcher tests exercised `execv`, `prctl` and
`getppid` exclusively in forked children. The gate reported all three as never executed,
with the code demonstrably working and its assertions passing.

*Rule:* call anything you need measured **in the parent process**. That is usually
possible even for process-control calls, and often more direct:

- A **failing `exec`** does not replace the image, so it returns normally and exercises
  the whole function — argument marshalling included.
- `prctl`, `getppid` and `fork` itself can all be called by the test process, which then
  reaps the child it made.

A child is still the right place to test what *only* a child can show — that an exit code
survives a real process boundary, say — just don't expect its coverage to count.

### 1.10 `ERESTARTNOINTR` is kernel-internal and never reaches userspace.

`fork(2)` lists it among its errors. The kernel restarts the call rather than returning
it, and glibc does not define the constant at all.

*Evidence:* the compiler refused to name it, in a loop enumerating fork's documented
errnos. A manual page listing an error is not proof that userspace can observe it.

### 1.11 The three monotonic clocks disagree, and each disagreement means something.

Measured on the development machine after 140 seconds of uptime:

```
CLOCK_MONOTONIC      139.916862507
CLOCK_MONOTONIC_RAW  139.490723645     426 ms behind
CLOCK_BOOTTIME       139.916895306     33 us ahead of MONOTONIC
```

- **`MONOTONIC_RAW` vs `MONOTONIC` is NTP slew** — here about **0.3%**. Over a five-hour
  run that is nearly a minute. RAW is the honest ruler for *how long did this take*,
  because nothing adjusts it underneath a measurement already in progress.
- **`BOOTTIME` vs `MONOTONIC` is suspend, and nothing else** — they share the same NTP
  discipline, so their difference isolates the time the machine spent asleep. The 33 µs
  here is just the gap between the two syscalls.

*The trap:* computing suspend as `BOOTTIME − MONOTONIC_RAW` looks equivalent and is not.
It would have reported a **426 ms "suspend" on a machine that never slept**, because that
pair does not share the discipline.

*Why it earns its place:* silicon cools while a machine sleeps, so a temperature curve
spanning a suspend describes two experiments glued together. A tool that reported it as
one would be publishing a fabricated result.

### 1.12 An invariant true of two clocks is not true of two readings

`CLOCK_BOOTTIME` is `CLOCK_MONOTONIC` plus suspend, so it can never advance less. That is
true **of the clocks, at one instant**, and false of two readings taken at different
instants — which is all any interval calculation actually has.

The two are separate syscalls, so every reading carries a gap between them, and the gap
*differs* between readings. When it shrinks, the BOOTTIME delta comes out microseconds
smaller and the difference goes negative.

*Evidence:* a strict check refusing any negative value passed the debug build and failed
under **both** sanitizer presets — slower, so more variance in the gap — reporting the
clocks as contradictory by 2989 ns.

*The general rule, which is the part worth carrying:* before enforcing a relationship
between two measured quantities, ask whether it holds of the *quantities* or of the
*measurements*. Non-atomic reads turn every exact inequality into an approximate one, and
the tolerance has to be argued from how the measurement is taken.

The local lesson is smaller and more embarrassing: the same reasoning had already been
applied to the suspend threshold a few lines earlier and simply was not carried to the
contradiction check. **When you write one tolerance, look for its mirror.**

### 1.13a `cpu0` has no `online` file, and that is normal

Every CPU directory under `/sys/devices/system/cpu/` carries an `online` attribute **except
`cpu0`**, because the boot CPU cannot be offlined on most configurations, so the kernel
never creates the file.

*Evidence:* probed directly — `cpu0/online` does not exist while `cpu1/online` reads `1`, on
an ordinary healthy 4-CPU machine.

This matters because it is a **P7 absence that is completely ordinary**. Code that read
per-CPU `online` files and treated a missing one as an error would fail on every machine it
ever ran on. It is also why discovery reads the top-level `online` list instead: one file,
authoritative, and present everywhere.

Two other shapes probed at the same time, recorded so nobody has to guess:

- `smt/control` reads the **string** `"notsupported"`, not a boolean. `smt/active` is `0`
  or `1`, and the whole `smt/` directory is absent on some architectures — which is why
  `multithreaded()` is derived from the sibling sets instead.
- `core_siblings_list` is **package**-scoped despite its name (it is the older alias for
  `package_cpus_list`). `thread_siblings_list` is the one that means "shares a physical
  core". Reaching for the obvious-sounding name gets the wrong answer.

### 1.13 `--exclude-throw-branches` does not exclude the branches *inside* a landing pad

gcovr's `--exclude-throw-branches` drops the edges gcov labels `(throw)`. It does **not**
drop the ordinary branches sitting inside the cleanup block that a throw edge jumps to —
those are unlabelled, and nothing else recognises them either.

That block exists whenever a function builds something destructible and then calls
anything the compiler must assume can throw. Destroying a `std::string` carries a branch
(the small-string check), so the pattern

```cpp
return SomeError{kind, path_, describe(error)};   // two allocating members
```

emits a branch that **no test can ever take**: it runs only if `describe` throws after
`path_` has been copied, or if the `Result` constructor throws after the temporary is
built.

*Evidence:* the topology `Source` module measured 99.4% branch coverage with every test
passing, the four missing branches all on `return Unavailable{...}` lines. Raw
`gcov -b -c` showed them as `branch N never executed` immediately after a
`call N never executed`, with no `(throw)` label — which is why the exclusion flag left
them.

*Two fixes, and the second is the one that generalises.* Hoisting the second allocation
into its own statement removes the half-built-object cleanup, because the only throwing
construction then happens when nothing needs destroying. That handles the aggregate but
not the `Result` constructor, which was still assumed-throwing for want of a specifier.
Declaring the truth —

```cpp
constexpr Result(E error) noexcept(std::is_nothrow_move_constructible_v<E>)
```

— removed the rest. `std::variant`'s in-place constructor is itself `noexcept` exactly
when the alternative's is, so the condition is precise rather than optimistic: a type
whose move can throw still gets a throwing `Result` and the landing pad it genuinely
needs.

*The measurable result:* project-wide branch count fell from **650 to 565**. Those 85
edges were never the code's own decisions; they were exception plumbing being counted as
though they were, and the 646/650 they inflated was a less honest number than the 565/565
that replaced it.

*The rule worth carrying:* **a missing `noexcept` is not only a performance question — it
manufactures unreachable branches.** When an uncoverable branch appears on a line that
contains no `if`, look for the cleanup path before reaching for an exclusion. This is
rung 2 of the exclusion ladder approached from the other side: rather than making the edge
reachable, delete the edge.

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

**Implemented, and two details the design did not anticipate.**

*The orphan check must not compare against pid 1.* An orphan is reparented to the nearest
**subreaper**, not to init, whenever anything in the process tree has called
`PR_SET_CHILD_SUBREAPER` — which is the normal case inside a container and under
`systemd --user`. The comparison is against the supervisor's pid captured *before* the
fork, whatever the new parent turns out to be.

*Splitting the fork from what the child does is a testability requirement, not a style
choice.* A single `spawn()` has to call `_exit()` in the child, and `_exit` cannot be
driven from a test — it takes the test process with it. Two functions (`fork_worker`
returning which side you are on, `become_worker` returning the exit code the child must
use) put every decision above the seam and shrink the untestable remainder to two lines of
glue. The same split is what makes the death-signal failure and the race both reachable
from a fake.

A related small thing worth keeping: the fork result is returned as a **type that has to be
asked** which side it is on, not as a raw pid. `if (pid == 0)` written backwards in a
supervisor means the supervisor execs itself away and the run ends with no diagnosis.

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

### 2.6 "Absent" and "vanished" are different facts, and only a *stateful* reader can tell them apart

The capability model (F3) said absent, empty and malformed are three different things. The
topology `Source` module added a fourth that does not fit that list: a source that was
here, was read, and is **gone now**.

The errno is identical. `ENOENT` is `ENOENT`, whether the kernel never offered the
attribute or a CPU was offlined between two samples. Nothing in the failure distinguishes
them — only the history does, which is why `Source` carries state at all. It is one
`bool`, and it is the entire reason the module is a class rather than a free function.

*The rejected alternative* is the obvious one: `read_cpu_list(fs, path)`, stateless,
simpler in every other respect, and structurally incapable of making the distinction.

*Why the distinction is worth a class:* reporting a vanished source as absent says "this
machine never had that" about a machine that did. Every number gathered before the change
then silently belongs to a different machine than the summary claims — and nothing
downstream would question it, because "absent" is a perfectly ordinary thing for a
telemetry source to be.

The symmetric decision matters as much: `EACCES` is **not** promoted to vanished once a
source has been read. A device going away does not revoke a permission, and telling a user
their hardware changed when what they need is a group membership sends them looking in the
wrong place. `EISDIR` and `ENOTDIR` get the same treatment for the same reason — both mean
*our path is wrong*, and folding them into "absent" would turn a bug in our own path
construction into a confident permanent statement about the user's hardware.

*Falsified, not asserted:* three mutations were applied and each was killed — setting
`seen_` on entry rather than on success (6 tests), promoting `EACCES` to vanished
(2 tests), folding unknown errnos into absent (4 tests).

### 2.7 A P7 fixture can be *built* as well as committed — and some cannot be committed at all

`tools/check-test-obligations.py` required a module declaring **P7** to name fixtures under
`tests/fixtures/`. That is right for the states a committed tree can hold — a malformed
value, an unexpected layout, a dangling symlink — and wrong for the two that matter most
here:

- **"Vanishes mid-run" is an event, not a state.** No static tree can hold it. Only a test
  that reads a file and then removes it produces it.
- **A mode-000 file arrives from a clone readable.** git records the execute bit and
  nothing else, so a committed `eacces` fixture would be read successfully and the test
  would pass *having demonstrated the opposite of its claim*.

So the gate now accepts `fixture_builders` — a test file that constructs the hostile state
at runtime — as an alternative to `fixtures`. This is deliberately **not** a loophole: the
named file must exist *and* carry the marker `LOADFORGE P7 FIXTURE BUILDER`, because
without a marker the check degenerates into "some file exists", which is the vacuous shape
this project has now shipped four times (§4.7). Four new gate tests cover the escape hatch
in both directions, including the one that matters: a builder that does not declare itself
fails.

*The general shape, which is the part worth carrying:* when a gate blocks correct work, the
question is whether its **intent** or its **spelling** is wrong. Here the intent — a P7
claim must be backed by something that really puts a source in that state — was right, and
the spelling recognised only one implementation of it. Weakening the intent would have been
the wrong fix; so would inventing a prove-nothing fixture to satisfy the spelling, which is
exactly what the gate's own P2 comment already warns against.

### 2.8 Discovery refuses when its sources contradict each other, rather than picking one

The kernel publishes the CPU topology several times over, in files that must agree: a CPU's
thread siblings must include the CPU itself, two CPUs that call each other siblings must
publish the *same* sibling set and report the same core, and every sibling must be online.
Nothing outside the kernel enforces any of it.

*The decision:* when the checks fail, discovery **refuses**. It does not pick whichever file
it read first, and it does not drop the CPU that does not fit.

*The rejected alternative* is the accommodating one, and it is what most tools do — take
`online`, take `core_id`, assume they line up. It is tempting because it always produces an
answer. That is exactly the problem: a wrong core count is not a visible failure, it is a
number every later report is silently attributed against, and nothing downstream would
think to question it. A refusal is loud once; a plausible wrong topology is quietly wrong
forever.

*Why the agreement is evidence rather than a tautology (F21):* these files are written by
different parts of the kernel from different internal structures — `online` from the
hotplug machinery, `thread_siblings_list` from the scheduler's topology masks, `core_id`
and `physical_package_id` from the architecture's CPU identification. A bug in LoadForge
cannot make them agree.

Two smaller decisions inside it:

- **A core is a (package, core_id) pair**, never `core_id` alone. `core_id` is unique only
  within a package, so counting by it merges every socket's core 0 and reports half the
  machine.
- **An empty `online` is a contradiction, not an empty reading.** `CpuList` is right to
  accept an empty value — `offline` is empty on every healthy machine — but a discovery
  layer knows something the parser does not: this code is executing, so at least one CPU is
  online. That is the difference between a parser, which must accept whatever the format
  allows, and a layer that knows something about the world.

### 2.9 `core_id` of −1 means the kernel does not know, and that is not a malformed file

Real kernels write **−1** into `core_id` and `physical_package_id` when they cannot
determine them, which happens on some virtualised and arm64 machines.

This forced the sysfs integer reader to be **signed**. An unsigned parser would refuse `-1`
as "not a digit" and report a malformed file — which is a lie, because the file is exactly
what the kernel meant to write. Reading it faithfully and then refusing to build a topology
on it lets the message distinguish *"your kernel wrote something strange"* from *"your
kernel does not expose this"*, and only the second has an answer the user can act on.

The same reader also shows that **the right answer to one input can differ between two
callers of the same class**: an empty value is a success for `cpu_list()` and malformed for
`integer()`, because no kernel attribute holding a number is ever written blank. That is
not an inconsistency to be tidied away; it is the two formats genuinely differing.

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

**It happened again, and the extra assertion earned its place immediately.** A later run of
that same test reported all twelve files missing instead of one. The cause: gcovr writes
its report on a *single line*, and the fixture removed the victim with `grep -v` on the
line containing it — deleting the entire document. The original two assertions were still
satisfied. The count assertion is what failed.

Two lessons, and the second is the general one:

- **Do not line-edit structured data.** The fixture now *renames* the entry rather than
  deleting a line, which touches one attribute and cannot depend on how the XML is wrapped.
- **A test fixture is code, and it fails the same ways code does.** This one had a bug
  that made the test vacuous while green. Fixtures deserve the same "what would make this
  fail?" question as the thing under test.

**A third instance, and the sharpest one, because the code under test contained the exact
bug its own comment denied.** `Source::integer` guards against a run of digits long enough
to overflow its accumulator. It was written as:

```cpp
magnitude = magnitude * 10 + (digit - '0');
if (magnitude > kMaxMagnitude) { /* refuse */ }
```

under a comment stating — correctly — that *"overflowing a signed integer is undefined
behaviour rather than a big number, so there would be nothing left to test for once the
loop had finished."* That is precisely what the code then did: it multiplied first and
inspected the result afterwards. By the time the comparison runs, the overflow has already
happened.

The test fed it twenty-six nines, asserted the refusal, and **passed** — because a wrapped
value happens to exceed the bound on some later digit, so the expected error arrived
without the guard ever having worked. Only the UBSan build reported it:
`signed integer overflow: 999999999999999999 * 10 cannot be represented in type 'long int'`.

Three things worth carrying:

- **Check before the operation, not after it, whenever the operation is the thing that can
  destroy the evidence.** The guard is now `magnitude > (kMaxMagnitude - digit) / 10`,
  which keeps the value in range instead of detecting that it left.
- **A comment stating the hazard is not a defence against it.** This one named the exact
  failure and sat directly above it. Prose does not execute.
- **Test the boundary, not a value far past it.** "Twenty-six nines is refused" can be
  satisfied by an accident; "10^18 is accepted and 10^18 + 1 is refused" cannot, and
  neither case can overflow anything, so neither can pass for the wrong reason.

### 4.7 Declaring something a gate does not check

The obligation ledger has per-class rules — P7 must name its fixtures, P2 must owe an
injecting tier — each added when a module first declared that class. **P6 had no rule at
all.** Declaring it asked for nothing and the gate printed `ok`.

Found by declaring P6 for the first time, for the worker launcher, and noticing the gate
had no opinion about it. That is §5.4 wearing different clothes: a promise in a document
with nothing behind it, except here the document was the gate's own input.

**When you are the first to use a category a checker knows about, check whether the checker
actually checks it.** A vocabulary a tool accepts is not the same as a vocabulary it
enforces, and the gap is invisible precisely because the tool says nothing.

### 4.8 Verifying "every gate" from memory, and missing the one for the other language

A change to the topology module was checked before pushing against: the full test suite,
both sanitizers, the coverage gate, the coverage-completeness gate, the exclusions gate,
the obligations gate and its own tests, the runtime-dependency gate, the fetch and
dependency gates, `clang-format`, `clang-tidy` and `shellcheck`. Eleven things. CI still
came back red.

It came back red on `ruff format --diff tools/`, because the change also touched a
**Python** gate, and the recalled list was a list of the checks this project's *C++* work
usually needs. The miss was not carelessness about any one gate; it was that the list
lived in my head and was assembled by association with the last similar change, and this
change was similar-but-one-language-wider.

*The general rule:* **a checklist reconstructed from memory is biased toward the last
thing it was used for.** When a change reaches into a part of the tree you do not normally
touch, the question is not "did I run the checks?" but "what does CI run that I have not
named?" — and the answer is in the workflow files, which are readable in seconds and do
not rely on recall at all.

*The structural fix* is not another rule. The repository has no single local entry point
that runs what CI runs: a contributor forms their confidence from whichever subset they
remember, which is the same failure waiting for the next person. That belongs in its own
change rather than smuggled into this one, so it is recorded in §6 instead of fixed here —
but it is the real answer, and "be more careful" is not.

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

- **Awaiting the owner:** one manual workflow run to establish the mutation-tool pin, which
  is a hard prerequisite for the first workload milestone; branch protection on `main`
  (required status checks, no required approvals — a solo maintainer would otherwise block
  themselves); deletion of the merged branches. The credential this work runs under gets
  **403 on workflow dispatch and on deleting a ref**, so all three are genuinely the
  owner's rather than something to try harder at.
- **Unanswered question:** whether CI should hard-require a specific committer identity.
  Doing so would close the repository to outside contributors, so the current check
  verifies *shape* — one author, complete identity — and not a particular person.
- **Landed since this file was written:** the syscall seam, the runtime-dependency gate,
  `ExitStatus`, the coverage-completeness gate, the worker launcher and the pool that
  supervises a set of them (§2.1), the clock (§1.11, §1.12), the sysfs CPU-list parser, and
  the capability-classifying `Source` that decides what a *failed* reading means (§2.6,
  §2.7). Review was waived by the owner rather than performed, which is worth remembering
  when reading that history: the gates are the only thing that has inspected it.
- **Next in sequence:** the cache hierarchy and NUMA layout, the two parts of topology
  discovery still unbuilt. The CPU half — cores, threads, packages, with the cross-checks —
  has landed (§2.8, §2.9), and the contradiction question it raised is settled: discovery
  refuses rather than picking a winner. Caches bring a shape the CPU files do not: each
  `cache/indexN` has a `shared_cpu_list` that must be consistent with the sibling sets (an
  L1 shared beyond a physical core, or an L3 not shared across a package, is a
  contradiction), plus `size` in a unit-suffixed format (`48K`, `266240K`) that
  `core::byte_size` may or may not already parse correctly — worth checking rather than
  assuming.
- **Wanted: one local entry point that runs what CI runs.** There is currently none, so
  every contributor — and every session — assembles the list from memory and gets a
  different subset. That is how §4.8 happened: eleven checks run, the twelfth not recalled
  because it belonged to the other language in the tree. A `tools/verify.sh` that runs
  exactly what the workflows run, and is itself checked against the workflow files so the
  two cannot drift, would close it. Deliberately not bundled into the change that
  motivated it.
- **Deferred, and worth not forgetting:** `Source` has no "counter wraps" state, which the
  P7 taxonomy lists. It is genuinely not needed yet — nothing here reads a monotonic
  counter — but the telemetry sources at M3 will, and the decision about where wrap
  detection lives (in `Source`, or in a counter type above it) should be made deliberately
  rather than by whoever first hits it.
