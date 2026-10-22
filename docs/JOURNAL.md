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

**What it is not:** a log of what happened when. That is `docs/history/`, one
timestamped file per working session; this file keeps only what was learned.

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

### 1.14 What sysfs writes for caches, and why the config parser cannot read it

Four facts, each probed from the running machine before the cache module was designed:

- **`cache/indexN/size` is written as `48K`, `2048K`, `266240K`** — a bare `K` meaning
  1024, staying in `K` even at 260 MiB. `core::parse_byte_size` **rejects** it: run against
  `"48K"` it returns `"unit suffix is not recognised"`, because it accepts `KiB`/`MiB`/`GiB`
  and, deliberately, a percentage. That parser reads what a *user* writes in a config file.
  Pointing it at a cache size would fail on every real machine, and teaching it `K` would
  make `70%` a parseable cache size. So topology has its own, and a test pins the boundary
  in both directions.
- **A shared cache is published once per CPU that can see it.** `index3/id` reads `0` from
  all four CPUs — one L3, four directory entries. Deduplication was on `(level, type, id)`
  when this was written — *superseded three bullets down: `id` is absent on arm64, and the
  identity is now `(level, type, shared_cpu_list)`* — and reading all four *first* is what
  makes them checkable against each other.
- **`id` is unique only within a level, and L1 Data and L1 Instruction share one.** cpu0's
  L1d and L1i are both `id=0`. An identity of `(level, id)` would merge them.
- **Some virtualised machines publish no `cache/` directory at all.** Zero caches is a fact
  about the machine, not a failure to read it — the scan terminates on the first *absent*
  index, and nothing else terminates it.
- **`cache/indexN/id` does not exist on arm64.** Found by CI, not by probing: the
  `ubuntu-24.04-arm` jobs failed with
  `/sys/devices/system/cpu/cpu0/cache/index0/id: not offered by this kernel` while every
  x86 job passed. The kernel's cacheinfo only publishes `id` when the architecture supplies
  one — x86 derives it from CPUID leaf 4; arm64 generally does not — and the same
  visibility rule hides `size`, `coherency_line_size` and `ways_of_associativity` whenever
  the value is unknown, rather than writing zero. So a module that keyed on `id` refused
  every arm64 machine. The identity of a cache is now `(level, type, shared_cpu_list)`,
  which is what a cache *is* from the CPUs' side, and `id`/`size`/`line` are optional
  fields. This is the second time the ARM64 CI matrix found something no local run could
  (§1.2 was the first), which is the whole argument for the matrix.

### 1.15 What sysfs writes for NUMA, and the number that does not add up

Probed from the running machine, before the module was designed:

- **`node/online`, `possible`, `has_cpu`, `has_memory` are CPU-list format** — the kernel
  writes node masks with the same formatter — so `CpuList::parse` reads them unchanged.
- **`nodeN/meminfo` is multi-line and every line is prefixed `Node N `**: 36 lines on this
  kernel, zero without the prefix. The prefix is a cross-check for free — a file naming a
  different node is not the file we think it is. `/proc/meminfo` has the same shape *without*
  the prefix, which is why the MemTotal parser is not yet shared.
- **`nodeN/distance` is single-space-separated integers, one per online node** (written
  with `for_each_online_node`), so the row length is a certain cross-check against the
  online set. Self-distance reads 10; that is the kernel's `LOCAL_DISTANCE` and the ACPI
  SLIT convention, and is *not* checked.
- **Node memory does not sum to system memory.** node0 — the only node — reports
  `MemTotal: 5081316 kB` (4962 MiB) while `/proc/meminfo` reports 16481980 kB (16095 MiB),
  and all 128 online 128-MiB memory blocks sit *under node0*. The node's own accounting
  undercounts its own blocks. Whatever the cause in this VM's memory hotplug, the lesson is
  what it always is: **a cross-check that would refuse this ordinary machine is wrong, not
  the machine.** So there is no node-memory total, and nothing compares node memory to
  anything.
- **`cpu/cpuN/nodeM` is a link from the CPU's side to its node.** Independent of
  `nodeM/cpulist`, so agreement between them is evidence (F21). It is used as
  corroboration in the T8 test — a test that can fail — not as a rule in discovery that
  could refuse a machine.
### 1.16 Affinity: three facts the kernel makes plain, and one it hides

Probed on the running machine before the module was designed, with `errno` read on its
own line — the first probe read it inside a `printf` argument list and reported `errno=0
Success` for every failing call, which is §4.4 exactly, in a throwaway program, ten minutes
after re-reading §4.4.

- **`EINVAL` is one word for three facts:** an empty mask, a mask naming only CPUs the
  machine lacks (`{63}` on a 4-CPU box, `{1000}`), and a mask naming only CPUs the cpuset
  withholds. The kernel does not say which. The requested mask in the message is what lets a
  reader tell.
- **`ESRCH` for a pid that does not exist**, from both `sched_getaffinity` and
  `sched_setaffinity`, including pid −1.
- **As root, `sched_setaffinity(1, …)` succeeds.** The probe pinned this container's init
  to CPU 2 and had to put it back. CI runs as root. So every real-kernel test targets the
  *calling* process, and the seam's own doc says nothing above it passes a pid it did not
  fork.
- **The one it hides:** when the mask overlaps the cpuset only partly, the call *succeeds*
  and the thread is confined to the intersection, with no word about what was withheld. That
  silence is the whole reason `missing_from` exists — the capability question has to be
  asked, before or after, because the kernel will not volunteer the answer.

---

### 1.17 Memory: what the kernel rounds, what it does not, and what root cannot see

Probed on the running machine before the module was designed, `errno` on its own line every
time — three throwaway probes still read it inside a `printf` argument list first, and one
lost its child's output entirely by calling `_exit` without flushing. §4.4 is apparently a
lesson each session relearns; the seam's `send_signal` comment is the one that survives.

- **`MAP_HUGETLB` rounds the length up; `munmap` does not.** A one-byte `MAP_HUGETLB`
  request maps and takes a whole 2 MiB page; `munmap` of that mapping with length 1 is
  `EINVAL` and the mapping stays; `munmap` with 2 MiB succeeds. So the length a `Mapping`
  remembers is the *rounded* one, computed above the seam where the overflow arm can be
  tested, and the fake refuses the unrounded length exactly as the kernel does. A fake that
  forgave it would have let a huge page leak on every explicit-huge-page run, silently, in
  a destructor.
- **`MAP_HUGETLB` with no pages reserved is `ENOMEM` from `mmap`**, not a `SIGBUS` on first
  touch: with a private anonymous mapping the reservation is taken at map time. "Present
  but exhausted" is therefore a reported refusal, not a crash later. With two pages
  reserved, two one-byte maps succeed and the third is `ENOMEM`; a 2 MiB + 1 byte request
  with one page left is `ENOMEM` too, because it needs two.
- **Every call on an unmapped range has a different word.** `mlock` and `madvise` say
  `ENOMEM`; `mbind` says `EFAULT`; `munmap` says nothing at all and succeeds. The fake
  models the first three and is *stricter* than the kernel on the fourth — an unmap of an
  address it never handed out is `EINVAL` — because a double unmap is a bug the suite
  should see, and stricter is not laxer (§4.2).
- **`mbind` has no glibc wrapper.** It is `syscall(SYS_mbind, …)` with a raw node bitmap,
  and the `maxnode` argument is *one more* than the highest bit the kernel should read: the
  kernel decrements it before use. An empty mask is `EINVAL`; a node without memory
  (node 7, node 1023 on this one-node VM) is `EINVAL`; an unaligned address is `EINVAL`.
  Node 0 succeeds. No `libnuma`, on purpose — the static release links nothing but the C
  and C++ runtime.
- **As root, `mlock` ignores `RLIMIT_MEMLOCK`.** CI runs as root, so the two refusals an
  unprivileged process meets — `ENOMEM` past the limit, `EPERM` under a limit of zero
  without `CAP_IPC_LOCK` — cannot be provoked in the test process. They are provoked in a
  forked child that lowers the limit and drops to `nobody`, and reports both errnos over a
  pipe. Not root → the test *skips*, visibly, rather than passing on an assertion a
  privileged process cannot make.
- **`MADV_POPULATE_WRITE` works on this kernel** (6.x; it needs 5.14). It is preferred to
  `MAP_POPULATE` because the flag's failure is silent — `mmap` succeeds and the pages are
  simply absent — and a memory test that believed its pages resident when they were not
  would measure the page-fault handler. The `madvise` reports.
- **A sanitizer answers `mlock` itself.** Under ASan, an `mlock` of an unmapped range
  "succeeded", and an unprivileged child with `RLIMIT_MEMLOCK` of 4 KiB locked a mebibyte
  without complaint — because ASan and TSan intercept `mlock`, `munlock` and `mlockall` and
  return 0 without calling the kernel. Deliberate on their part: their runtimes reserve
  terabytes of shadow address space and a real lock of it would be ruinous. So the tests
  that assert the *kernel's* answer to `mlock` skip, visibly and by name, under a sanitizer,
  and the successful-lock test reads `VmLck` from `/proc/self/status` afterwards — the
  kernel's own accounting is the oracle (F21) that a lock happened at all. The fake and the
  unsanitized builds carry the assertions; a suite whose only proof of `mlock` was "it
  returned 0" would have proved nothing under exactly the build meant to catch memory bugs.
- **Reserving huge pages is not something a test suite does to a host.** Writing
  `/proc/sys/vm/nr_hugepages` on a machine that already has a reservation in use would
  disturb whatever is using it. So there is no T8 test that reserves; the real-kernel test
  reads the 2 MiB pool's `free_hugepages` and asserts the answer *that* pool implies — no
  pool: `EINVAL`; empty pool: `ENOMEM`; pages free: success, and then the unrounded-length
  `EINVAL`. Three arms, one assertion each, no skip. The pool is the module's P7 state, and
  it is built in the fake (§2.13).

---

### 1.18 Config: what the TOML library coerces, what the stream throws, and what a v1 cgroup looks like

- **toml++ answers `value<int64_t>()` for a boolean.** `threads = true` read as one thread
  and `enabled = 1` would have read as true. The library documents the coercion; the schema
  exists to refuse it. Every integer and boolean read now checks the node's own type first,
  and the test that found it (`threads = true` accepted where the message expected "found
  boolean") is the test that keeps it found. A float with an integral value coerces the same
  way, so `95.0` is refused as "found floating-point" rather than read as 95.
- **libstdc++ throws from reading a directory; it does not set `badbit`.** `std::ifstream`
  opens a directory (the kernel allows `open(dir, O_RDONLY)`, as the seam's T8 established
  for the raw call), and the first read throws `std::ios_base::failure` with `EISDIR`
  regardless of the stream's exception mask. The `in.bad()` check written first was
  unreachable and the exception escaped the test. The file is now read with C stdio, where
  `ferror` reports the same fact as a value: "cannot read the file", distinct from "cannot
  open the file" for an absent one and from an empty document (F3: absent, unreadable, empty
  and malformed are four facts).
- **A `catch` clause is a branch no test can take.** The handler's type match is an edge
  gcov counts — matched, or rethrown — and the rethrow arm is unreachable for a handler that
  catches what is thrown. `--exclude-throw-branches` does not remove it. So the vendored
  parser is built with `TOML_EXCEPTIONS=0`, `toml::parse` returns a `parse_result` that
  config reads like any other `Result`, and there is no `try` in the module. This is §1.13's
  landing-pad finding from the other side: not a cleanup block, a handler.
- **A top-level key written after `[run]` belongs to `[run]`.** Obvious once said; a test
  placed `workload = []` after the run table and asserted a top-level refusal it could not get.
  Recorded because a user's file will do the same and the message — "run.workload: unknown
  key" — is correct and will look wrong to them.
- **This container is cgroup v1.** `/sys/fs/cgroup/memory.max` does not exist; the memory
  controller is at `/sys/fs/cgroup/memory/` and `/proc/self/cgroup` lists `memory:/…`
  separately from the unified `0::/` line. The reader that resolves a memory share (F7) will
  meet both hierarchies and must treat "no cgroup limit found" as *unlimited by the cgroup*,
  not as a failure — the ceiling is then `MemAvailable` alone. `resolve_memory` takes an
  optional for exactly that reason.

---

### 1.19 Shared memory across exec: what a memfd does, and what a refused attach must not do

Probed before the heartbeat region was designed, then confirmed by the real-kernel tests:

- **A `memfd` mapped `MAP_SHARED` survives `exec` through its descriptor, not its mapping.**
  The worker is a fresh image and inherits nothing mapped; it inherits the descriptor
  (once `FD_CLOEXEC` is cleared for exactly that one) and maps the same file. A write on
  either side is visible on the other; the probe did it across a fork + exec of a different
  program entirely. A fresh memfd is zero-filled, so a region's slots start as "never
  published" with nothing written.
- **`memfd_create` is close-on-exec by default (`MFD_CLOEXEC`), and that is the right
  default.** Everything the controller opens stays close-on-exec; `become_worker` clears the
  flag for the descriptors *that* worker needs, after the orphan race check and before
  `exec`, so a worker starts with its own heartbeat region and record pipe and no sibling's.
  A worker that cannot keep a descriptor has a startup failure of its own (code 123), like
  the three before it.
- **A refused `attach` must not close the descriptor it was handed.** The first version
  built the region object before validating the header, and the object's destructor closed
  the descriptor on the way out. In one process with one descriptor table — the fake, and
  equally two views in one test — that closed the *creator's* descriptor, and the next
  iteration of the header-corruption test failed with `EBADF` from `mmap` instead of the
  layout fault it was staging. A region owns a descriptor only once it has attached;
  before that it unmaps and leaves the descriptor alone.
- **A failure arm exercised only in a forked child reads as never executed — §1.9, again.**
  The first tests for `EFBIG` from `ftruncate` and `EMFILE` from `memfd_create` lowered the
  limits in a forked child and reported the errnos over a pipe; the arms ran and the gate
  still showed the lines at zero, because the child `_exit`s and flushes nothing. They now
  run in the test process under a *soft* limit lowered for the scope and restored after
  (`SIGXFSZ` ignored meanwhile, or the kernel kills the process instead of answering). The
  rule stands: measure in the process that is measured.
- **Two mappings of one file share an address in the fake, and must be counted, not
  keyed.** The fake hands out the file's own storage for every `map_shared` of the same
  descriptor (that is what makes the in-process "two views" tests honest), so a map keyed by
  address silently merged a raw test mapping with the region's own, and the first unmap
  erased both. A multimap counts them. Stricter than the kernel in one place: mapping past
  the file's length is `EINVAL` here, where Linux maps it and `SIGBUS`es on first touch.

### 1.20 Pipes and poll: what the kernel reports at each end, and the two things it does not

Probed for the record channel, then pinned by the real-kernel tests:

- **`POLLHUP` and `POLLIN` arrive together.** A pipe whose writer has closed with bytes still
  queued polls as readable *and* hung up; only once the bytes are drained is it hung up
  alone, and `read` then returns 0. So `Readiness` is four booleans rather than an enum, and
  the controller must drain on either flag and treat the read that returns 0 — not the flag
  — as the end of the stream. A loop that stopped draining on `POLLHUP` would lose a
  worker's last records.
- **`poll` does not fail for a closed descriptor, and ignores a negative one.** A number
  that was open a moment ago comes back `POLLNVAL` with the rest of the entries answered
  normally; a negative number is skipped entirely and reports nothing. Both verified; the
  fake is stricter about the second (reports it invalid), because nothing above the seam
  has a reason to poll a descriptor it does not hold.
- **The one `poll` failure a process can provoke on itself is `EINVAL` past
  `RLIMIT_NOFILE`,** and the limit can be lowered after the descriptors are open without
  affecting them. That is how the failing arm of `poll_readable` runs in the test process
  (§1.9 rule) rather than being a permanent exclusion.
- **`write` to a pipe with no reader is `EPIPE` only if `SIGPIPE` is not delivered.** The
  default disposition kills the process before `write` can answer. The test ignores the
  signal for its scope; the worker entry point will ignore it for the process, so that a
  controller that died is a reported `EPIPE` (and then the death-signal exit) rather than
  a worker that vanished with signal 13 — which would look, to a supervisor, exactly like
  a fault.
- **A `write` on the read end, or on a descriptor opened read-only, is `EBADF`,** which is
  why the fake answers `EBADF` to a write on anything but a pipe's write end. And a frame
  larger than the pipe buffer (64 KiB payload against a 64 KiB default buffer) is written
  whole by the short-write loop while the reader drains concurrently; the fork test sends
  one to prove the loop is not decorative.

### 1.21 Signals and a stepping clock: what the worker entry needed from the seam and the fake

- **`sigaction` refuses `SIGKILL` with `EINVAL`,** which is the failing arm of
  `ignore_signal` against the real kernel; glibc's own `sigaction` query is the oracle for
  the disposition before and after. The worker ignores `SIGPIPE` first thing, because a
  write to a controller that has gone must be a reported `EPIPE` and not a death by signal
  13 — which a supervisor could not tell from a fault.
- **A loop that reads the clock each tick needs a clock that moves inside one call.** The
  fake's clocks were fixed values per id; `advance_clocks_per_read` makes each id step on
  each of its own reads, so three clocks read together stay within a step of one another
  (far below the suspend threshold) and tick *k* sees *k* steps. The stepping is applied
  only when asked for, because a clock test sets a deliberately nonsense reading and must
  get it back exactly — the first version normalised it and broke that test. A negative
  step, from a base far from zero, is a clock running backwards: the one case
  `Clock::between` refuses, reachable now without a scripted sequence.
- **A failure on the *N*th read of a clock** (`fail_clock_on_read`) reaches the loop's
  mid-run clock arm without failing its first reading, which is a different line.

### 1.22 What the controller needed, and the two traps the supervision tier set

- **`waitpid(-1, WNOHANG)` is the one reap a loop that owns a `poll` may call.** It is also
  the one whose failing arm a test process can reach: with no children at all it answers
  `ECHILD` at once, so `reap_any_nonblocking` carries its own branch where `wait_any` had to
  move its decision above the seam. A worker's exit closes its record pipe, which wakes the
  poll, so reaping after the poll sees a death within one tick without ever blocking.
- **A pipe the test holds across a `fork` of the controller is held by the workers too.** The
  controller-killed test gives the forked controller a pipe to report worker pids through
  and reads to end of file. The first version used `pipe()`: the workers inherited the write
  end through the controller's fork and kept it across their `exec`, and the end of file
  arrived only when the last worker exited — sixty seconds later, by which time the test
  had nothing left to kill. `pipe2(O_CLOEXEC)` closes it. The same rule the controller's own
  pipes already followed, met from the other side.
- **The fake needed a reference count per pipe end.** One `write_open` flag could not model a
  worker that holds the write end after the controller closes its own copy, and that is the
  normal state of every record pipe for the whole run. `worker_holds_end` and
  `worker_releases_end` are the test's way of saying "the worker has it" and "the worker is
  gone"; the controller's close then leaves the far side open exactly as the kernel's
  descriptor table would. The hold must be taken while the number is still open in the
  controller, which is why the orchestration tests take it from the reporter's
  `run_starting` callback — the one moment between the pipes' creation and the fork.
- **In the fake, the child's `getppid` must be the fake's own `getpid`.** The orchestration
  passes `process_id()` as the supervisor pid; the launcher's orphan check compares it with
  `parent_pid()`. A fake whose two answers differ makes every modelled child an orphan
  (code 121), which the first child-path test duly reported instead of the 120 it expected.
- **Memory the fake hands out as a descriptor is gone once the region is released,** so a
  test that wants the controller's own beat count must read it *during* the run — from the
  mapping it already took to write worker beats — not after.
- **A defaulted comparison in a header can read as uncovered the moment a second
  translation unit uses it.** `MemoryBudget`'s `operator==` had been at 100% since the
  config increment; the controller's tests compared budgets from two more units and the
  gate reported the struct's two member lines at zero. The mechanism, found by decoding the
  per-unit gcov records: the linker keeps one COMDAT copy of an inline function, the other
  units' copies are discarded with their counters at zero, and gcc's line table filed the
  kept copy's member lines under `Config::operator==` while the discarded copies were
  "unknown function 150" — two records gcovr keeps apart, one of them zero. No test change
  can execute a discarded copy. The fix is one definition: the comparison is defined in
  `config.cpp`, so there is one record and it is the one that runs. Other defaulted
  comparisons in headers have not shown this; when one does, this is the move.

### 1.23 What the misbehaviour catalogue showed about the kernel, and about the old tests

Established while building the fake worker (§2.19) and its T7 tests:

- **The parent-death signal does not survive a fork.** The kernel clears it in the child of
  every `fork`, so a worker's grandchild has none. The grandchild test
  (`GrandchildrenOfDeadWorkersAreKilledWithThem`) failed against the controller as it was:
  both grandchildren were still spinning ten seconds after their workers were reaped, and
  nothing in the design would ever have ended them. The fix is §2.20; with the controller's
  group sweep switched off again, the test fails again the same way.
- **The old controller-killed test could not see the death signal at all.** With the launcher
  temporarily arming signal 0 instead of `SIGKILL`, `AControllerKilledLeavesNoWorkerBehind`
  still passed (every worker left on the hang-up, exit 124), while the new deaf-worker test
  failed with both workers alive past its bound. The new test is the only one that would
  notice the signal gone.
- **`kill(-0, …)` is `kill(0, …)`: the caller's own process group.** The first version of the
  teardown sweep signalled `-pid` for every unreaped worker, and a spawn that fails after
  the fork leaves later workers with pid 0 recorded. The unit test's exact list of signals
  showed `(0, 9)`. Against the kernel that is `SIGKILL` to the controller's own group, the
  user's shell job included. The sweep now skips a pid it never recorded, and the test that
  found it pins it.
- **`/proc` says when a child is ready, so a test need not guess.** `/proc/<pid>/cmdline`
  is the test binary's argv until the worker execs and the worker's afterwards, and the
  launcher arms the death signal before the exec, so a `--worker` in it means armed.
  `/proc/<pid>/status`'s `SigIgn` mask shows a disposition the moment it is set, which is
  how the ignored-`SIGTERM` test holds the run until the escalation cannot find the signal
  still at its default.
- **A sanitizer's handler can be bypassed below its interceptors.** §1.7's rule (a fault
  signal raised under ASan or TSan becomes an exit code) would make the fault rows test the
  sanitizer. The fake worker resets the disposition with the raw `rt_sigaction` system call
  (the kernel's `struct sigaction` is handler, flags, restorer, mask, all zero for
  `SIG_DFL`, with an 8-byte kernel sigset) and sends the signal with a raw `tgkill`, so the
  status word says `SIGSEGV` on every preset. It also sets `RLIMIT_CORE` to 0 and itself
  undumpable, and the test matches `killed by signal 11` as a prefix, because whether a
  core was written is the machine's business.
- **Under a subreaper, the controller reaps strangers.** The test process takes the orphaned
  grandchildren as subreaper, and the controller's own `waitpid(-1)` can meet one first and
  skip it as not a worker. The first version of the test waited its whole bound for each
  grandchild it could no longer reap; `ECHILD` is an ending too.
- **An instrumented test executable reports its template instantiations as `src/` lines.**
  The fake worker uses `core::Result` over its own types; compiled with `--coverage`, those
  instantiations of `src/core/result.hpp` entered the report, and the error constructor and
  `error()` of each showed as never executed: 99.8% of lines over code no test of `src/` is
  meant to reach. The fake is now linked with the coverage runtime, which the instrumented
  libraries it links need, and not compiled with instrumentation itself.
- **A seqlock reader's retry bound is microseconds; a descheduled writer is milliseconds.**
  Under TSan with four suites running on four cores, the real-binary test
  `TwoWorkersCompleteForTheConfiguredDuration` saw a worker `SIGTERM`ed as stale 619 ms into
  a run whose allowance was 2000 ms. By elimination the only path that could do that was a
  slot the controller could not read: a worker taken off its core between the odd and the
  even store of a publish tears every one of the reader's 64 retries, and the controller
  called one torn read a death. The worker side had the same flaw for the controller's slot,
  leaving with 124. Ten full TSan runs under the same load pass since §2.22.

### 1.24 Memory limits and OOM kills: what the kernel counts, where, and what a fresh cgroup says

Probed on the container this was written in, a hybrid machine (kernel 6.18, root), before
the budget reader and the OOM classification were designed; `errno` on its own line. The
memory controller is on v1 at `/sys/fs/cgroup/memory`; the unified hierarchy is mounted
beside it at `/sys/fs/cgroup/unified` with no memory files at all; this process's cgroup
carries a 13.36 GiB limit, below the machine's 15.6 GiB of MemAvailable, so here the cgroup
is the ceiling (PLAN F7's container case, on a real machine).

- **A memcg OOM kill reaches the parent as a plain SIGKILL** (status `0x9`). A child moved
  into a fresh v1 cgroup limited to 16 MiB, with `memory.swappiness` 0, that touched 256 MiB
  was killed with nothing in its status to say why. The status alone cannot tell the OOM
  killer from `kill -9`; `ExitStatus::killed_by_sigkill`'s comment was right.
- **The v1 count is the victim's own cgroup's, and only its.** That cgroup's
  `memory.oom_control` went from `oom_kill 0` to `oom_kill 1`; its parent's stayed at 0. So
  the count to read is the cgroup the workers are in -- the controller's own, since a child
  is born into its parent's -- not an ancestor. (On v2, `memory.events` also counts
  descendants, per cgroup-v2.rst; no v2 host was available to measure it.)
- **`/proc/vmstat`'s `oom_kill` rose by one too.** The machine-wide count includes a
  cgroup's own OOM kills, so it moves for every OOM kill of every process on the machine.
- **`kill -9` moves neither count.** Asserted on every run by
  `RecordRealTest.ThisMachinesCountIsReadAndASigkillThatIsNotAnOomKillDoesNotMoveIt`, and the
  real OOM kill above by `AChildReallyOomKilledInAV1CgroupMovesThatCgroupsCountByOne`, which
  skips, saying why, where no v1 memory cgroup can be made.
- **A fresh v1 child of a limited cgroup reads the unlimited sentinel** in its own
  `memory.limit_in_bytes` -- `9223372036854771712`, LONG_MAX rounded down to a 4 KiB page --
  while its `memory.stat` reads `hierarchical_memory_limit 14343688192`, the parent's limit,
  which is the one that binds. The leaf's own file is the wrong answer; the kernel's
  hierarchical figure is the right one. A write of 12345 reads back 12288 (rounded down to a
  page); a write of -1 reads back the sentinel. The sentinel differs with the page size,
  which is why the reader treats everything within 256 KiB of LONG_MAX as "no limit".
- **Read in `mm/oom_kill.c`, not measured:** `__oom_kill_process` counts the kill
  (`count_vm_event(OOM_KILL)`, then the memcg event) before it sends the SIGKILL, and has done
  since both counts arrived in 4.13. So a reading taken after a reap includes that reap's
  kill -- and can include a kill whose victim has not been reaped yet, which is the race the
  attribution rule in §2.23 is shaped around.

### 1.25 A test can put a whole run under a memory limit by moving itself

Established while writing the OOM row of the misbehaviour catalogue (§2.24), on the same
hybrid machine as §1.24 (v1 memory controller, root):

- **A process moved into a fresh v1 cgroup takes its future children with it and nothing
  else.** The test writes its own pid to a new child cgroup's `cgroup.procs`; the workers
  the controller then forks are born there, and `/proc/self/cgroup` names the new cgroup,
  so a `CgroupOomKillRecord` made afterwards reads that cgroup's `memory.oom_control` --
  the same way `loadforge stress` opens its record. Memory charged before the move stays
  charged where it was (`memory.move_charge_at_immigrate` is 0 by default), so the move
  brings no history into the limit. Moving back and removing the directory leaves the
  original cgroup as it was; `rmdir` succeeds once the last dead child is released.
- **Inside a memory cgroup the OOM killer still chooses by the whole process, not by its
  charge.** Read in `mm/oom_kill.c`, not measured: `oom_badness` scores a candidate by its
  RSS, swap entries and page tables -- all of its memory, wherever charged -- plus
  `oom_score_adj` times the cgroup's limit in pages over 1000. So a test process with a
  large RSS charged elsewhere could in principle outscore the worker that crossed the
  limit. The allocating fake worker raises its own `oom_score_adj` to 1000 first (no
  privilege is needed to raise it), which adds the whole limit to its score and makes
  the choice certain rather than likely.
- **The count moves by exactly one.** Three fake workers under 256 MiB -- one allocating,
  one dying by SIGSEGV, one completing -- moved the new cgroup's count from 0 to 1 on
  every run: 30 of 30 alone, and 30 of 30 with the whole supervision tier repeated under
  `ctest -j4`.
- **CI's runner user cannot do any of this.** The hosted runners are cgroup v2, and the
  test steps run as an unprivileged user in a cgroup it does not own, so `mkdir` is
  refused with `EACCES`. The test skips there and quotes the refusal; CI runs it a second
  time as root, where a skip is turned into a failure (§2.24).

### 1.26 Mull's runner, read from its source; and what a sweep of this tree costs

The first full mutation run on GitHub's runners (ubuntu-24.04, 4 vCPUs), one job over the
whole project, reached the six-hour job limit and was cancelled. It was reported to this
change before 2026-10-20 10:40 UTC, and a cancelled job leaves no report, so the cause
cannot be read from it. What follows was established between 2026-10-20 10:40 and
11:40 UTC from Mull 0.34.2's source (the pinned release's tag), from replicas of its
argument and configuration parsing built with the same crate versions, and by measuring
the test binary on a 4-core machine shared with another build (load average 2.5 to 3.9,
so the times are upper bounds).

- **`--timeout` does not bound a mutant.** It bounds only the two unmutated runs, warm-up
  and baseline, and defaults to 3000 ms; either one failing is fatal. Each mutant gets
  `max(10 × baseline, --minimum-timeout)`, the floor defaulting to 30 ms
  (`rust/mull-tools/mull-runner.rs`).
- **A timed-out mutant is reported as killed.** The IDE reporter counts every status but
  "passed" and "not covered" as killed. So a timeout that cuts off a healthy run turns a
  survivor into a kill: the gate's dangerous direction, and why its floor is 20 s against a
  suite that takes under half a second.
- **Mull can wait for ever after its own timeout fires.** It kills the program it started,
  then reads that program's stdout and stderr to the end (`runner.rs`). A child the
  program forked inherits both pipes; if a mutant leaves one alive -- a broken kill, a
  broken reap, `pause()` waiting for a signal that never comes -- end of file never
  arrives and that worker thread never returns. With `--no-mutant-output` a mutant's
  output goes to `/dev/null` and there is no pipe to wait on. Nothing the gate reads comes
  from that output.
- **Without a `mull.yml`, everything compiled with the plugin is mutated.** The workflow
  passed the plugin in `CMAKE_CXX_FLAGS`, which reach every target: the unit tests' own
  code and GoogleTest's sources as well as `src/`. `includePaths` are Rust regular
  expressions matched unanchored against the absolute path the compiler recorded, applied
  twice, by the compiler plugin and again by `mull-runner`. A `mull.yml` that does not
  parse is a warning and Mull falls back to its defaults, and unknown keys are ignored --
  both silently mutate everything, which the gate's scope check now refuses.
- **Test-program arguments follow the executable, after `--`.** A replica of
  `mull-runner`'s command line, compiled from its own `clap` definitions with the same
  `clap` 4.6.0, parses `mull-runner [options] ./binary -- --gtest_filter=...` with the
  filter as the program's argument. Options after the executable but before `--` are
  still Mull's.
- **The unit-test binary is fast; it is its forks that are dangerous.** 722 tests, 0.45 to
  0.69 s for the whole binary. Run one at a time under `strace -f`, exactly 21 tests create
  a process (one more creates a thread); each already had `Real` in its suite's name, though
  so did many tests that do not fork. Without them, 701
  tests take 0.25 to 0.39 s, and 200 runs four at a time all passed: every temporary path
  the tests make carries the process id.
- **What leaving the forking tests out costs.** Source-based coverage of the 701 tests
  against all 722 differs in 15 lines, all in `src/platform/syscalls.cpp`:
  `fork_process` and `reap_any_nonblocking`, whose comparisons (`pid < 0`, `pid == 0`)
  only a test that forks executes. Five mutants under Mull's defaults; they will survive
  until the decision moves above the seam (§2.28).
- **How big a sweep is.** Mull's default operators counted from clang's AST of every
  translation unit in the test binary, deduplicated by location and weighted as
  `cxx_default` mutates them (two mutants per `<`, `<=`, `>`, `>=`; one per `==`, `!=`,
  arithmetic operator and `++`): platform 179, worker 144, topology 138, controller 111,
  resources 72, core 49, config 44, main 14, reporters 1; 752 in all. The header-only
  modules contribute none: nothing in this binary instantiates them yet.

**The estimate for a shard**, at the measured throughput of four parallel runs (0.46 s of
wall time per mutant) and assuming, pessimistically, that one mutant in five hangs and is
cut off at the 20 s floor:

| Shard | Mutants (est.) | Running them | Job, with about 8 min to install and build |
|---|---|---|---|
| platform | 179 | 4.4 min | 13 min |
| worker | 144 | 3.5 min | 12 min |
| topology | 138 | 3.4 min | 12 min |
| controller | 111 | 2.7 min | 11 min |
| resources | 72 | 1.8 min | 10 min |
| core | 49 | 1.2 min | 9 min |
| config | 44 | 1.1 min | 9 min |
| main | 14 | 0.4 min | 9 min |
| reporters | 1 | — | 8 min |

So the sweep's own work is minutes, and its cost is set-up. The six hours were not
mutants of `src/` running against a slow suite; with no output to read, the two
mechanisms above -- every target mutated, and a forked child holding Mull's pipes -- each
suffice to explain them, and neither can be confirmed until a run reports.

### 1.27 `kill(2)` succeeds on a zombie, so a successful signal proves nothing about the death

*2026-10-20 19:30 UTC.* The first independent review (EXT-01) and a real-kernel test
(`RealProcessSyscallsTest.KillSucceedsOnAnUnreapedZombieAndOnlyTheReapMakesItESRCH`):

- A child that has terminated and has not been reaped is a zombie. `kill(zombie, SIGKILL)`
  **returns success**, the signal is discarded, and `waitpid` then reports the original
  status (exit 0 in the test). `ESRCH` comes only after the reap.
- So a controller that records "signal sent" on success, and on `ESRCH`, records a signal
  that may have arrived after the death. A worker killed by the OOM killer or an outside
  `kill -9` in the same tick as an escalation was reported as the controller's stop or
  timeout, and the OOM predicate refused to read the count for it.
- `waitid(P_PID, pid, ..., WEXITED | WNOHANG | WNOWAIT)` says whether a child has
  terminated without reaping it (`si_pid` is 0 while it runs); it answers `ECHILD` once the
  child is reaped. It narrows the race but cannot close it: the worker can still die between
  the look and the `kill`, and `pidfd` does not tell which of two racing SIGKILLs ended it.
- The shared fake had modelled a dead child as `ESRCH`, the same class of fake-versus-kernel
  divergence as the SIGPIPE bug (§1.23). It now models the three states: running, zombie
  (`child_exits`: signals succeed, `has_exited` is true), reaped (a reap delivered: `ESRCH`
  and `ECHILD`). Its own tests are `tests/unit/platform/fake_syscalls_test.cpp`.

### 1.28 clang-tidy 18's static analyzer crashes on `std::bit_cast<std::uintptr_t>(p) + offset`

*Established 2026-10-20, wave 1 and wave 2 (#40, #63).* `clang-tidy-18` died with a
segmentation fault, not a diagnostic, on a translation unit that computed an address as
`std::bit_cast<std::uintptr_t>(pointer) + offset` inside one expression. The backtrace is in
the static analyzer (`clang-analyzer-*`), so the job fails with no finding to read. Binding
the converted value first, to a local or to a member set once in the constructor, and adding
the offset to that, makes the crash go away with identical code generation
(`src/verification/integrity/slice_runner.cpp` keeps `start_address_` for this reason).

*Consequence:* a tidy job that ends in a crash is a toolchain defect, not a clean pass and
not a finding. Reduce the expression, do not suppress the check.

### 1.29 clang-format will format a `CMakeLists.txt` as C++, and its ignore file has no `**`

*Established 2026-10-20, wave 1 and wave 4 (#77, #78).* Given a `CMakeLists.txt` on its
command line, `clang-format-18` does not refuse it; it guesses C++ and rewrites it, joining
and re-indenting the arguments of CMake commands until the file no longer configures.
Streams did this by passing every changed file to the formatter. `.clang-format-ignore`
(clang-format 18 and later) prevents it, but its patterns are matched like shell globs:
`*` does not cross a `/`, and `**` is not supported. The file therefore lists
`CMakeLists.txt`, `*/CMakeLists.txt`, `*/*/CMakeLists.txt` and so on to the deepest
directory in the tree, plus the `*.cmake` patterns. A new directory deeper than the last
pattern needs a new line.

### 1.30 A signed `__int128` multiplication does not link under UBSan on ARM64

*Established 2026-10-20, wave 1 (#42).* With `-fsanitize=undefined`, a multiplication of
two signed `__int128` values is compiled into a call to `__muloti4`, the overflow-checking
multiply. On x86-64 the runtime provides it; on the ARM64 runners the link fails with an
undefined reference. Unsigned arithmetic has no overflow to check, so the same
multiplication as `unsigned __int128` emits no call. The bounded-verification code
multiplies magnitudes as unsigned and applies the sign itself (`exact::product`).

### 1.31 Coverage counters updated from several threads go negative

*Established 2026-10-20, wave 1 (#67).* gcov's counters are plain increments by
default. Code that runs on several threads at once under the coverage build loses updates,
and an edge counter can end up smaller than the counter it is derived from: gcov reports
the difference as a negative count, and gcovr 8.6 refuses the report (`NegativeHits`). Seen
in the compression oracle and decoder (`oracle.cpp:48`, `decoder.cpp:121`) and in each
stream that ran tasks on the executor. `-fprofile-update=atomic` makes the increments
atomic and the counts exact. It is now set for every target in
`loadforge_enable_coverage()` (`cmake/Coverage.cmake`), so a component no longer needs its
own copy; the copies the wave-1 streams added are redundant and are being removed.


### 1.32 ThreadSanitizer inflates the resident set, so a resident-memory bound does not hold under it

*Established 2026-10-21 (#88).* The journal soak test bounds resident memory after thousands
of appends to its warm value plus 256 KiB. Under ThreadSanitizer every heap page the run
touches also touches the sanitizer's shadow and metadata pages, which count in
`/proc/self/statm`'s resident figure. The bound then holds or not depending on where the
allocator lands: it failed once in CI (`resident memory after 4320 appends against 432
warm`) and passed 5 of 5 locally under the same preset. AddressSanitizer was already
exempted for its quarantine; ThreadSanitizer now is too, and the plain and coverage builds
keep the bound.

### 1.33 `lz4 -d` accepts blocks the format document says a decoder may refuse

*Established 2026-10-20, wave 1 (stream 03, `docs/journal/03-oracle-compression.md`).* Hand-built
blocks in a legacy frame were given to `lz4 -d` 1.9.4. It accepted offset 0 (writing zeros),
a match starting 11 bytes before the end of the data, and a last token with a match length;
it refused an offset reaching before the first byte, a match over the last five bytes, and a
block ending after a match. The document leaves the end-of-block rules to the decoder
("allowed to reject"), and the tool rejects some and not others. So agreement with the
reference decoder is not the standard for a malformed block; the document is (§2.34). Every
block the reference *compressor* wrote kept the stricter rules, which is what lets the
strict oracle accept real output.

### 1.34 A single-bit flip can turn one valid encoding into another, so "every flip is caught" is false

*Established 2026-10-20, wave 1 (streams 03 and 05, fragments `03-oracle-compression.md`,
`05-workload-compression.md`).* In `runs-l1` 517 of 4,728 single-bit flips decode to the
original input by another route (an offset moved to an identical earlier copy); over one
unit's 306,944 flips, 10 were agreements. The specification counts any block that decodes to
the input as right, so no oracle can or should catch them. The tests say which of three each
flip is (malformation, different bytes, another valid encoding), prove the third
independently of the decoder, and pin the counts so a rule that stopped refusing shows as
flips moving between columns. An exit criterion that says "every flip" has to say this.

### 1.35 An unoptimised build leaves the upper ymm halves dirty, and every SSE instruction after it pays

*Established 2026-10-20, wave 1 (stream 06, `docs/journal/06-workload-dense-linear.md`).* A
whole-product test took 14 s in the debug build against 0.05 s for the same loop standalone;
samples put the time in the test's own scalar code after the AVX2 kernel had run. GCC emits
`vzeroupper` on leaving a 256-bit function only when it optimises: `objdump -d | grep -c
vzeroupper` gave 0 for the debug object and 2 for `-O2`. An explicit `_mm256_zeroupper()` at
the end of the kernel took the test to 0.6 s. Every test, coverage and sanitizer build is
unoptimised, so the cost would have landed on every tier. Stream 25's sweep gained the same
call when it moved into the kernel directory.

### 1.36 What a 32-byte vector does to a build and to a call

*Established 2026-10-20, waves 1 and 3 (streams 06, 25, 26, 30).* Four facts, each from a
failed build or a crash:
- `std::array<__m256d, N>` fails under `-Werror` (`-Wignored-attributes`: a template argument
  drops the vector attributes); a struct of named `__m256d` members does not.
- A 32-byte vector passed or returned by value in a unit built without AVX is
  `-Werror=psabi`. Pass by `const&`; make constants `static constexpr`.
- `std::bit_cast` on a vector inside a `target("avx2")` function crashed with `SIGSEGV` at
  `-O0`: it is an ordinary function emitted for the baseline target, and a vector crosses
  the call in registers the caller's target names. The rule: every function that touches a
  vector is `always_inline` into the one function that chose the target, and the cast is
  `__builtin_bit_cast`. `-Wno-psabi` only hides the note.
- `-mavx2` on one source file puts AVX encodings in every inline function that file
  emits: 93 `ymm` instructions in the one object, none in five others, and only because that
  file included nothing from the standard library. A `target` attribute per function has no
  such reach (§2.35).
Also: Clang refuses `__attribute__((target(...))) [[nodiscard]]` and takes the other order.

### 1.37 The scalar and the fused paths give the same bits on x86-64 and AArch64, measured under emulation

*Established 2026-10-20, waves 1 and 3 (streams 06, 25, 30).* The dense-linear output
digests, the graph ranks and nine ray-tracing digests were computed on x86-64 (scalar and
AVX2) and from an `aarch64-linux-gnu-g++-13` build run under `qemu-aarch64` 8.2 (scalar and
NEON): equal on every line. It follows from the kernels' order (lanes hold different entries,
never partial sums of one; a fused multiply-add is one correctly rounded operation on both
ISAs; `-ffp-contract=off` keeps the rest unfused), and `objdump -d` of the scalar kernels
showed no `vfmadd`, `fmadd` or `fmla` on either architecture. The emulator is IEEE-exact for
these operations but is not hardware; CI's ARM64 runner is the check on a real core. Under
qemu the same tests ran about thirty times slower (3 min 25 s against 8 s). A NEON kernel
measured to 100% on AArch64 before CI saw it by cross-compiling, running under qemu, and
reading with `aarch64-linux-gnu-gcov-13`.

### 1.38 A new thread inherits its creator's floating-point registers; `sched_setaffinity(0)` moves the calling thread only

*Established 2026-10-20, wave 1 (streams 07 and 08, `07-numerics-platform.md`,
`08-executor.md`).* With `fesetround(FE_UPWARD)` and MXCSR.FTZ and DAZ set before a pool of
four threads started, every task on every thread read upward rounding, FTZ and DAZ on; a
write in the child did not reach the parent. So "establish before any thread starts, assert
on each thread's start" is correct. Pid 0 in `sched_setaffinity` is the calling *thread*:
four threads pinned themselves, read their masks back as exactly their CPU, and
`sched_getcpu()` agreed over 160 tasks, with the caller's mask unchanged. The kernel refuses
a thread's placement (CPU 1023, an empty set) with `EINVAL` exactly as the shared fake
models it.

### 1.39 CPUID with a leaf above the highest returns the highest basic leaf's data, not zeros

*Established 2026-10-20, wave 1 (stream 07).* A probe that reads leaf 7 on a CPU without it
reads another leaf's bits as feature flags. The probe reads leaf 0 first and answers
scalar-only below leaf 7; `XGETBV` without OSXSAVE faults on real hardware, so the fake
records either read as a fault. The machine's `/proc/cpuinfo` flags and the probe agree
(`avx2`, `fma`, `avx512f`): Linux removes a flag whose xfeature the OS has not enabled, so
the flag list is the kernel's verdict on the OS-support half of the question.

### 1.40 The runner's working set is bigger than its footprint, and an unprivileged run cannot lock it

*Established 2026-10-20, wave 1 and the M2 integration (streams 02 and 11,
`02-input-integrity.md`, `11-integration-m2.md`).* Under the default 8 MiB
`RLIMIT_MEMLOCK`, as `nobody` (`setpriv --reuid=65534`): a 1 MiB footprint ran, 7,000,000
bytes was refused with `mlock(8960384 bytes): Cannot allocate memory`. The set adds strides
rounded to lines, the regeneration buffer, scratch and slack, and a page for each of three
control copies. Sort's smallest unit is 2,621,440 bytes, so at 3 MiB its runner maps
8,437,824, which is 49,216 over. This is what failed every CI job at the first sweep
(2026-10-21 09:11 UTC): the jobs are unprivileged with 8 MiB, where §1.17's "root ignores the
limit" had hidden it locally. The steps that run `ctest` now lift the allowance for their
shell (`prlimit --memlock=unlimited`); the runner's always-lock policy stays an open
contract question (§6).

### 1.41 A perf event that excludes the kernel counts nothing the kernel does

*Established 2026-10-21 (streams 13 and 21, and the M3 corrections,
`m3-contract-corrections-2026-10-21-1145-UTC.md`).* `perf_event_open(PERF_TYPE_SOFTWARE,
CONTEXT_SWITCHES, pid = getpid(), inherit = 1)` read after each of five 20 ms sleeps: with
`exclude_kernel = 1`, 0 0 0 0 0 as root and as uid 65534; with it clear, 1 2 3 4 5 as root,
`EACCES` as uid 65534 at `perf_event_paranoid` 2, and 1 2 3 4 5 at paranoid 1. A zero
standing in for a value was present and valid, found only by a run against the real kernel;
no fake could have shown it. Also from the VM: the four hardware events fail `ENOENT`; the
power PMU (type 9) has only `energy-psys`, scale exactly 2^-32 J, and refuses `exclude_hv`
and `inherit` with `EINVAL`; and `perf_event_paranoid` is 0 in one container and 2 in the
next, so it is a property of the machine, which is why the source reads and names it.

### 1.42 What the development VM does and does not offer

*Established 2026-10-20, waves 1 and 2 (streams 09, 10, 13, 21).* A Firecracker VM, Linux
6.18.44, 4 vCPUs, one node, no SMT: no `/sys/class/hwmon`, an empty `/sys/class/thermal`, no
cpufreq, no powercap, no EDAC, no hardware performance counters; memory, `/proc/stat`, the
CPU lists and caches are there. A "present" tree for any of the absent ones cannot be
captured here, so tests build them in the layout the kernel documents and say so; none is a
capture, and T11 (a thermal result on hardware) stays open. A real run of the M3 stack saw
10 of 23 metrics, took 3 ms in 11 to 12 rounds of a 1 s run (0.28 % of one CPU at 10 Hz),
and every absent metric printed with its remedy. `sizeof(Point)` is 120 bytes on x86-64, so
a five-hour run over 96 series at 1 Hz is at most 414 MB resident, ten times that at 10 Hz.

### 1.43 More shapes of §1.13, and one refinement

*Established 2026-10-20, waves 1 to 4 (streams 00, 01, 02, 03, 04, 09, 12, 13, 19, 20, 24,
28, 30).* Every stream found uncoverable branches on lines with no `if`. The refinement
(stream 09): a cleanup branch comes from a *partly built* or *temporary aggregate*, not from
a named local string; `std::string detail = ...; return Unavailable{kind, path_,
std::move(detail)};` has none, `Unavailable{kind, a(), b()}` has one. What works is
assigning into an object that is already complete (`emplace_back()` then fill the reference,
default-construct then assign a field at a time). The shapes met beyond §1.13's: a ternary
whose arms build strings (20 branches in one renderer); a temporary aggregate passed to
`push_back`; `new (std::nothrow) T[n]` (a null check no test takes); a throwing constructor
under a `new` (declare it `noexcept`, and let the `bad_alloc` end the process as the
workload's own allocation would); a `switch` over every enumerator (the implicit no-match
edge; make the last case `default:`); a template instantiated for several types (gcovr keeps
instantiations apart; a reader written over a `std::variant` instead); a variant built on the
first line of a function; and a `std::sqrt` that carries an errno arm (`-fno-math-errno` on
that library, results unchanged). A defaulted `operator==` in a header, or a short-circuit `&&`
chain defining one, still reads as uncovered (§1.22); a comparison written as a xor of the
fields tested against zero has no branch. `noexcept` was not used to hide any of them where
the function allocates: `bugprone-exception-escape` is on.

### 1.44 A file that is only a table is absent from the coverage report

*Established 2026-10-20, wave 3 (stream 27, `docs/journal/27-oracles-wave3.md`).* A generated
`twiddle_table.cpp` holding one `const std::array` initializer has no executable line, so
gcovr wrote no record for it and `tools/coverage.sh`, which requires every `src/**.cpp` in the
report, failed at 100.0 % with "1 source file(s) absent". The table is a `.inc` included by
the accessor's `.cpp`. A data-only `.cpp` in `src/` fails the gate however well it is tested.

### 1.45 At -O3 GCC 13 cannot see that the last alternative of a variant is held

*Established 2026-10-20, waves 1 and 2 (streams 01, 02, 11).* `return
f(*std::get_if<Last>(&v));` after `get_if` tests for the other alternatives compiles in the
debug, coverage and Clang builds and fails the `static` and `release` presets
(`-Wnull-dereference -Werror`: "potential null pointer dereference", reported inside
`basic_string::size()`). Calling the function out of line with `[[gnu::noinline]]` moved the
warning into the wrapper. `std::get` for the last arm compiles but trips
`bugprone-exception-escape`. What works: `std::visit` over per-alternative overloads.
`oracle.hpp`'s helpers were corrected this way (#84, item 3, with a static-header
preset so the contract headers compile under the release flags); `provenance_of` in
`record.hpp` still has the shape, and the controller avoids it with a visit.

### 1.46 A multiplication by 0.001 is not the nearest double; a division by 1000 is

*Established 2026-10-20, wave 2 (stream 09, `docs/journal/09-telemetry-sources-a.md`).*
`20016 * 0.001` is `20.016000000000002` and `20016 / 1000.0` is `20.016`; over the millidegree
values 20000 to 99999 they differ for 10,654 of 80,000. A reading carries a multiplier and a
divisor, and a temperature divides. A related misreading: the temperature range, absolute zero
to one thousand degrees, does not catch a chip that reports a fixed `-128000` for "not
connected", because that is above absolute zero. The first comment said it did and the test
that tried it failed; catching it needs the chip named, which needs a chip to verify against.

### 1.47 How far real computations sit inside the derived bounds

*Established 2026-10-20, waves 1 to 3 (streams 04, 06, 22, 24, 26, 27).* Worst residual over
tolerance: dense linear 0.0071 (against a naive loop; 0.0030 to 0.23 over 43 evaluation orders
and short vectors); FFT under a twentieth of the bin tolerance, `numpy.fft` at 1e-5; stencil
`scipy.ndimage` 1.6e-2, the nearest to its limit; particles 0.0061; sparse CG
`||r||/||b||` 2.25e-16 at worst of 2,000 solves against the specification's 1.97e-11, which
confirms the assumption its derivation flagged (a factor of 8.7e4, on the scalar path).
The bounds are not hair-triggers on healthy output, and they are not vacuous: a flipped
exponent or upper mantissa bit breaches them. What they let through is the low significand
bits: 116 of 1,184 sampled single-bit flips of dense-linear outputs (every one a low
mantissa bit); mantissa bits 0 to 14 of a Monte Carlo batch sum at tolerance 2^-38. Both
are the specification's choice of a rounding bound, recorded so nobody expects the low bit
to be seen.

### 1.48 Parseval's residual cannot be placed within 2^-20 of its tolerance

*Established 2026-10-20, wave 3 (streams 22 and 27).* The specification asks every bounded
oracle's tolerance to be tested at (1 ± 2^-20) T'. The FFT's Parseval residual is a
difference of two doubles near 64, so it moves in steps of one ulp (about 4.5e-13) while T'
is about 6e-9: a step is about 2^-13 of T', and no output lands nearer the limit. Scaling
the input does not help. The bin check, the stencil, the sparse rows and the particles
reach epsilon by arranging the exact residual to be zero so that an offset *is* the
residual; for Parseval the test finds by bisection the nearest representable residual on
each side and requires both on the right side. The specification should say so (§6).

### 1.49 A confidence interval from replicates is too narrow once the correction subtracts a noisy baseline

*Established 2026-10-20, wave 3 (stream 28, `docs/journal/28-explore.md`).* A coverage
property (600 seeded searches, 5 replicates, noise on candidate and reference) measured the
nominal 95 % interval covering the true mean 81 % of the time: every corrected score
subtracts a baseline built from the same few reference readings, so the error is shared and
the sample spread cannot see it. Adding the propagated reference noise brought coverage to
95 to 98 %. A successive-difference estimate of that noise was dominated by drift (a clear
winner reported indistinguishable); the residual of each interior reading from the line
through its neighbours is not.

### 1.50 The executor's hand-off costs about 75 µs a run, almost all of it in the kernel

*Established 2026-10-20, wave 1 (stream 08).* Two million runs of one to thirteen trivial
tasks on four threads: 151.5 s wall, 21 s user, 162 s system (the futex calls of the condition
variables, four threads woken per run); no index lost or repeated, five threads before and
after, no allocation. It matters only to a workload whose unit is a few microseconds, and
none specified is. Under ThreadSanitizer the process has one thread more than the pool made
(the runtime's own, started once the process has made a thread), so the exact count is
asserted outside the sanitizers and "unchanged across a million runs" everywhere. A hundred
thousand runs took 7.7 s debug, 9 s TSan, 8 s ASan: the guess that sanitizers would be much
slower, which had cut the soak to a tenth, was wrong (§4.21).

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

### 2.10 Cache cross-checks: which ones, and — more importantly — which ones not

The cache hierarchy gets the same treatment as the CPU topology (§2.8): read everything,
check the readings against each other, refuse on contradiction. Four checks, each certain
of any machine:

- a cache's shared set contains the CPU it was read through;
- every CPU in that set is one the CPU topology found online;
- every CPU sharing a cache publishes an **identical** record for it — sharing a cache is an
  equivalence relation, exactly as sharing a core is;
- **sharing only widens with level**: the CPUs sharing a CPU's L1 are a subset of those
  sharing its L2. This is the defining structural property of a hierarchy.

*What was deliberately left out, and why it matters more than what went in:* a check that
refuses a real machine is worse than no check at all, because a refusal is loud and
permanent. Two plausible checks fail that test:

- "every CPU reports the same set of levels" — **false on big.LITTLE**, where big and
  little cores can have different cache depths;
- "sizes grow with level" — nearly always true and not guaranteed; L1i and L1d already
  differ at the *same* level (32K and 48K here).

The T8 test against the real `/sys` is what guards the checks that *were* kept: if any is
too strict, that test is the one that says so.

*And one that the arm64 CI job forced (§1.14):* the first version was itself the kind of
check that refuses a real machine — not a cross-check, but a **required attribute that is
not universal**. Requiring `id` refused every arm64 kernel. The rules that came out of it:

- Identity is `(level, type, shared_cpu_list)`; nothing depends on `id` being present.
- **Absent is a reading; anything else is a failure.** An optional attribute that is
  *denied* or *unreadable* still fails discovery, because "we could not read it" and "the
  kernel does not know it" are different facts, and folding the first into the second would
  hide a broken attribute behind a shrug. A test makes each optional attribute a directory
  (real `EISDIR`) and asserts the failure.
- A level total is `optional`: **one unknown size makes the sum unknown, not smaller.**
- `id`, where two sharers both report it, must agree — that is the identical-record check,
  which compares every field. But "two caches of one kind with different sharers have
  different ids" is *not* checked: probably true of every kernel, and "probably" is the
  wrong standard for a check whose failure mode is refusing a real machine.

*Two smaller decisions:*

- The scan's terminator is **absence and nothing else**. A denied or unreadable `indexN`
  is a failure, not the end of the list — otherwise a machine whose L3 we could not read
  would silently report as a machine with no L3, a plausible wrong answer.
- `read_index` returns `Result<optional<Cache>, DiscoveryFailure>`. That is the
  Result-of-optional shape §2.6 warned against, and it is right here because there are
  genuinely three outcomes — end of list, a cache, a failure — and collapsing any two would
  lose the distinction above.

*On shape:* the first `discover()` did the reading, all four checks, deduplication and
sorting in one body, and clang-tidy measured its cognitive complexity at **79** against a
threshold of 25. Splitting it into `read_index`, `check_served_cpus_online`,
`check_nesting`, `deduplicate` and `ordered_before` made each check a named claim rather
than a paragraph in a loop — which is what the doctrine says a cross-check *is*. The
finding was correct and the fix was better code, not appeasement.

### 2.11 NUMA: "not exposed" is an answer, and there is no memory total

Two decisions, both about what *not* to produce.

**A kernel that publishes no `node/` directory yields a layout with no nodes.** Not a
refusal — nothing is wrong with a kernel built without NUMA — and not a fabricated single
node holding every CPU and all memory. The fabrication is the tempting one, because every
caller would find it convenient; it is also a plausible wrong answer about a node the kernel
never described. `exposed()` says which case a caller is in, and `node_of()` answers null
rather than a default, because there is no node to default to. Only *absence* of
`node/online` means this; a denied or unreadable one is still a failure, with a real-`EISDIR`
test to hold the line.

**There is no total-memory accessor.** §1.15's finding — node0 reporting 4962 MiB on a
16095 MiB machine — makes any sum misleading, and an accessor that is misleading on the
first VM it meets should not exist. Per-node `memory_bytes` is offered with the caveat
written on it.

*The open thread from §6 is settled:* the multi-line reader is `Source::lines()`, a
primitive on `Source` with the same `seen_` memory as `text()` — so a source that yielded
lines once and is gone now is *vanished* whichever reader asks. What is *not* shared yet is
the `Key: value kB` parsing, because `nodeN/meminfo` carries a `Node N ` prefix that
`/proc/meminfo` does not, and generalising over one example is guessing. It lives in
`numa.cpp` until M3's procfs reader gives it a second caller.

*Also settled by the third copy:* `from_source` and `contradiction`, the two constructors
of a `DiscoveryFailure`, are now public in `cpu_topology.hpp`. Two private copies were
tolerable; the third was the signal.

*And one compiler finding worth keeping:* a range-for over `parse(list).value().ids()`
walks freed memory. Only the *final* temporary in a range expression has its lifetime
extended in C++20; the `Result` that owns the vector dies before the first iteration.
`-Wdangling-reference` caught it in a test fixture before it ran. Materialise the owner
first; the loop borrows from a named object.
### 2.12 Affinity asks the kernel; it does not pre-empt it

`Affinity::place` sends every mask to the kernel — empty ones, ones naming CPU 1000 on a
four-CPU machine — and classifies the answer. It could refuse those above the seam for
free. It does not, because that would be this code deciding what the kernel will accept
instead of asking, and the kernel is the authority on three things that can all change
between a check here and the call: the cpuset, which CPUs are online this instant, and
whether the thread still exists. The one thing decided above the seam is the *meaning* of
the answer — `EINVAL` is "no usable CPU", `ESRCH` is "the thread is gone", `EPERM` is "not
ours to move" — because that is where a test can reach every arm.

*The alternative rejected:* a `CpuMask` that cannot be empty by construction. Tidy, and it
would have removed a real case from the tests: the fake must refuse an empty mask *because
the kernel does*, and asserting that is how the fake is shown to be no laxer than the thing
it models (§4.2).

*Where the P7 fixture is:* not on disk. The hostile state is a cpuset narrower than the
topology, which cannot be committed and cannot be arranged in CI. It is built in
`FakeSyscalls`, which models the kernel's intersection semantics rather than a caller's
hopes. The ledger names the test file as the builder, and the marker comment says why the
"tree" is a process attribute this time.

---

### 2.13 Memory: the steps have an order, and a refusal names the step

`Memory::map` runs map, bind, advise, populate, lock — in that order and no other. The NUMA
policy and the huge-page advice govern how pages are *faulted*, so both must precede
anything that touches the range; populating and locking touch it. A failure at any step
unmaps what the earlier steps built, and the refusal names the step, because "cannot map
memory" from a five-step operation tells a user nothing about which of five remedies
applies. `Stage::kPopulate` is distinct from `Stage::kAdvise` although both are `madvise`:
the syscall is the same and the meaning is not.

**The huge-page strategy is a field of the request, never a default the kernel makes.**
docs/PLAN.md F7 asks for an explicit recorded choice because the strategy changes TLB
behaviour and therefore the result; a run record that did not say which was in effect would
describe a measurement nobody can repeat. Explicit huge pages also *name their size* in the
mmap flags — 2 MiB, always — rather than taking the kernel's default, which is 512 MiB on an
arm64 kernel with 64 KiB base pages. "Huge pages" meaning a different size on a different
machine would not be a record either.

**Two requests are refused before the kernel is asked, and the refusal says so.** A
huge-page rounding that overflows `size_t`, and a node id past 1023, which no Linux kernel
can number. Both would otherwise reach the kernel as a wrapped length or an unbuildable mask.
The refusal's `cause.call` names the *check* — "huge-page rounding", "node mask" — rather
than a syscall, because a message that said `mbind(…): Invalid argument` about a call that
was never made is §4.4 with extra steps. Everything else — a zero-length request, a node the
machine lacks, an empty pool — goes to the kernel and the answer is classified (§2.12).

*The alternative rejected:* `MAP_POPULATE`. Free, and silent when it fails. §1.17 has why.

*A core change this forced:* `Result::value() &&`. `Mapping` is the first move-only success
value in the project — an owner of a range that must not be copied — and a `Result` whose
`value()` returned only `const T&` had no way to give it up. The ref-qualified pair is the
minimum; `value_or` stays as it is, since a fallback for a mapping makes no sense.

*Where the P7 fixture is:* the huge-page pool, in `FakeSyscalls`, consumed and refilled in
whole pages the way the kernel accounts for it, and refusing the unrounded `munmap` length
the way the kernel does. The ledger names `memory_test.cpp` as the builder and the marker
comment says why the state cannot be committed or arranged in CI.

---

### 2.14 Config: an unknown key is a refusal, a wrong type is a refusal, and there is no default duration

`config::parse` refuses any key it does not know, at every level, naming the key and the
keys it does know. The alternative — ignore what you do not understand, as most parsers do
— means `max_temprature_c = 95` runs with no thermal limit and says nothing. There is no
default duration and no default mode for the same reason: a file that forgot them would run
for some length the user never chose. The exit criterion this serves (IMPLEMENTATION.md §4,
M1 item 5) says every rejection path is tested for the message it produces, and it is: the
tests assert the exact text, the dotted key and the line.

**Domains are stated once and bounded on purpose.** A temperature limit is 1..150 °C because
no silicon this project will meet has a junction limit past ~125, so 950 is 95 with a digit
stuck to it and a run that trusted it would never stop. A duration must exceed zero. A byte
size must exceed zero — the way to ask for no memory is to leave the key out. Threads is a
positive integer or `"all"` and is *not* bounded against a CPU count here, because the
topology is the only honest ceiling and it is not known until discovery runs; the scheduler
compares the two.

**`schema_version` is required and must be the one this build reads.** A file from a later
LoadForge is refused whole rather than half-read: the key this build does not know may be
the one that bounds the run, and the unknown-key rule would catch it anyway — but "schema 2
is not one this build reads" is the message a user can act on, and "unknown key `thermal`"
is not.

**Memory resolution is arithmetic here and reading elsewhere.** `resolve_memory` takes a
`MemoryBudget` — `MemAvailable` and an optional cgroup limit — and answers a byte count:
a share is a share of the lower of the two, rounded down, computed without overflow; an
absolute size larger than the ceiling is refused, never clamped, because a run that quietly
used less would report results for a workload nobody configured. Reading the budget is
platform knowledge (procfs, and both cgroup hierarchies per §1.18) and lands with the
controller. F7's "reserve headroom" is deliberately not baked in as a constant: it belongs
in the config as a stated number, once there is a workload that needs it.

*The alternative rejected:* routing the file read through the syscall seam so that a failing
open could be injected. PLAN §4.2 is explicit that config reading is ordinary file I/O, and
the real failures — an absent path, a directory — are produced with real files in T8 and by
the built binary through `--check-config`. The ledger row says so rather than claiming a
T2 it does not have.

*The round trip:* `to_toml` renders through the library's own formatter and the test parses
the rendering back to an equal `Config`, for both the minimal and the full document. A run
record can carry exactly what it ran with, in a form the reader accepts.

---

### 2.15 Heartbeats are seqlock slots with no lock, and "torn" is the EOWNERDEAD with nothing to recover

PLAN §4.6 says every process-shared lock must be robust and every acquirer must handle
`EOWNERDEAD`, and in the same breath prefers designs with no shared lock at all. The
heartbeat region is that design: one writer per slot (slot 0 the controller, slot i+1 worker
i), a sequence counter per slot, and nothing a reader ever waits for. The writer bumps the
sequence to odd, writes the fields, bumps it to even, each step released; a reader acquires
the sequence, reads the fields, fences, re-reads the sequence, and keeps the sample only if
the value was even and unchanged. The synchronisation argument is written out above the P5
test, where it runs under TSan against a writer thread publishing twenty thousand beats whose
every field derives from one number — a torn sample is detectable by inspection, and none
appears.

**A writer that dies mid-update leaves its sequence odd forever.** Every reader then reports
`kTorn` after a bounded number of retries (64) instead of spinning. That is the whole
EOWNERDEAD rule, discharged by construction: there is no protected state to make consistent
because no reader ever depended on the writer finishing. The ledger's obligation for
process-shared locks will record this module as holding none, and the gate for that lands
with the supervision increment. (It landed with the fake worker: §2.21.)

**The header is validated on attach (F18).** Magic, version, slot count and slot size each
have their own fault and their own message naming found and expected. A worker that mapped
the wrong descriptor would otherwise read somebody else's bytes as heartbeats, and a region
from a later LoadForge would be half-read rather than refused.

**What a beat carries is deliberately small.** A `CLOCK_MONOTONIC` timestamp (the one clock
every process shares a reading of), a progress counter, a phase, and a pid. `liveness()`
turns the timestamp into never-beat / fresh / stale against an allowance the caller states;
`progressed()` turns the counter into "working" versus "alive and stuck". Those two words are
the difference between a slow worker and a wedged one, which the misbehaviour catalogue
(IMPLEMENTATION §4) requires the controller to tell apart. A beat from the future counts as
fresh: both clocks are the same clock, so it can only mean the reader's own reading is the
older one.

*The alternative rejected:* a pipe per worker carrying heartbeat records. It needs no
shared memory, but a wedged worker holds its pipe open and no EOF arrives — PLAN F9's
correction to revision 6 — so staleness would still have to be judged by timestamp, and a
pipe adds a reader loop and a framing format to get the same answer the slot gives in one
acquire load.

### 2.16 Records are checksummed frames, and a reader that finds one bad frame stops reading

The second IPC: a worker's *findings* travel to the controller over a pipe, one per worker,
the worker holding the write end across `exec` and the controller the read end. Where the
heartbeat slot answers "is it alive", the record pipe answers "what did it find", and the
two have opposite requirements: a beat may be lost or torn and the next one replaces it; a
record must arrive exactly as sent or be known not to have.

**Every frame is a header and a CRC-32 over the header and the payload.** Magic, version,
type, length, checksum, then the payload, little-endian and written down in
`worker/ipc/record.hpp` so a reader can be checked against the document rather than the
writer. The checksum covers the *header* too, so that a corrupted length or type is a
checksum fault and not a plausible frame of a different shape. The CRC is IEEE 802.3 /
zlib, pinned to the published check value `crc32("123456789") == 0xCBF43926` and to zlib's
answers for three other inputs, because a checksum that was merely self-consistent could
not be told from one whose table had a typo (F21 again: the test must reach outside the
code).

**Each corruption has its own classification, and the reader never resynchronises.** Bad
magic, bad version, oversized, bad checksum, truncated — found and expected in the detail,
in the order a reader can check them: magic and version before the length is believed, the
length before anything is allocated or waited for, the checksum once every covered byte has
arrived. The first fault is final: later `next()` calls, more bytes, and `finish()` all
report the same fault, and the source stops reading. *The alternative rejected* is the
usual one — scan forward for the next magic and carry on — and it is rejected because a
reader that heals around a corrupt frame turns one bad byte into a silently shorter stream
of findings, and a shorter stream of findings is a hardware verdict with evidence missing.
Garbage in must never become a verdict out; a channel that went bad is reported as a
channel that went bad, and the controller decides what that says about the worker.

**The exhaustive corruption test is the module's reason to exist, so it is exhaustive.**
Every byte of a five-frame stream is corrupted in turn, twice (a flipped bit and a random
replacement), under random chunking so the reader's answer cannot depend on how the bytes
arrived. The requirement is three-fold: a fault at or before the corrupted frame; every
frame before it delivered intact; no record ever delivered that was not sent. Random noise
is fuzzed as well, for the crash case.

**The size limit binds both ends.** `kMaxPayloadBytes` (64 KiB) is refused by `encode` as
well as by the reader, so a writer cannot produce a frame no reader would accept — and a
reader that believed a corrupt length field would be the easiest thing on the machine to
crash with one byte.

**A drain either succeeds whole or reports the fault.** Records completed by the same read
as the corruption are not handed back alongside it. The controller's answer to a corrupt
channel must not depend on partial findings, and "the records before the corruption" is a
set whose boundary the corruption itself may have moved.

**The ends do not own the descriptor.** `RecordWriter` and `RecordSource` write to and read
from a number they are given; the pipe ends belong to whoever created the pipe and handed
one across `exec`. Ownership arrives with the supervision increment, where the controller
holds every read end and the launcher hands each write end to its worker; putting it in the
channel classes now would have decided the controller's design from below.

### 2.17 The worker process: an internal switch, a null workload, and four ways out

`loadforge --worker …` is the internal switch of PLAN §4.6: the controller re-executes the
binary with it and nothing else does. It is not in `--help` because it is not a command a
person runs, and it is not a test hook because it does exactly what a worker does in
production and nothing a test could ask for — the fake worker with the misbehaviour
catalogue is a separate executable, as IMPLEMENTATION §4 requires.

**The arguments are a round trip.** `argv_for` builds what the controller execs and
`parse_args` reads it back; a test says the two agree, which is what keeps them from
drifting (the config module's rule). Every key is required exactly once, every value has a
stated domain, and a refusal names the key — the controller writes these, so a refusal is
a bug report, and `kBadArguments` (125) is its own exit code. Two bounds were deliberately
*not* put in the parser: which ids a CPU mask can name is `mask_of`'s rule and which the
machine has is the kernel's, so `--cpus` is bounded to the type and the refusal comes from
the authority (and the first version bounded it in the parser, which made the `mask_of`
arm in `run` unreachable — a branch with no test is a branch nobody has shown to work).

**One tick: read the clock, beat, then judge.** The beat goes out before the duration check
so a worker about to complete has still reported itself alive, and before the controller
check so a worker about to leave has left its final progress. Then one `poll` on the
control pipe is the tick's wait: a byte is a stop request (a clean completion, how a run
ends early on request), a hang-up is the controller gone, the timeout is the interval. The
controller's own beat is judged against the allowance each tick; a controller that *never*
beat is given the allowance and then treated as stale, because a controller publishes its
first beat before it spawns anyone. A controller slot that cannot be read — a controller
that died inside `publish` — is its own reason, not an error, because the worker's answer
is the same: leave. (Once it has stayed unreadable for longer than the allowance, since
§2.22: one torn read is also what a controller descheduled mid-publish looks like.)

**Four exit codes, none colliding with the launcher's.** 0 completed; 119 setup failed
(could not attach, report, or be placed; or the loop itself failed); 124 controller lost
(hung up, stale, unreadable — not the worker's fault and not a completion); 125 bad
arguments. The launcher's 120–123 sit between, and a `static_assert` keeps it so. Every
way out that still has a channel writes an error record first, so the controller learns
*why* from the pipe and not only *that* from the status word; a way out whose channel is
the thing that failed does not try twice.

*The alternative rejected:* a `SIGTERM` handler for graceful stop. The default disposition
terminates, which the controller can classify as "stopped on request" when it was the one
that asked; a handler would add a P5 path (signal during publish) for no new capability
the control pipe does not already give.

### 2.18 The controller: one loop, one blocking call, and a classification that is a table

M1 exit criterion 1 — the controller runs the null workload for the configured duration and
honours its limits — is `controller/orchestration`, with `supervision` (what became of a
worker), `safety` (the escalation ladder and the limits) and `reporters/console` (the one
voice it has) beside it.

**One tick, in a fixed order, with one call that waits.** Read the clock; publish the
controller's own beat (slot 0, the one every worker watches); if the deadline has passed and
no stop has been requested, write a stop byte on every live worker's control pipe; judge
each live worker's beat and escalate where due; `poll` the record pipes for one interval —
the only call that blocks; drain what arrived; reap without blocking. The order matters
twice: the beat goes before the judgement so the controller is never the stale party, and
the reap comes after the poll because a worker's death closes its record pipe, which is
what wakes the poll. *Rejected:* a `SIGCHLD` handler. A handler adds a P5 path through every
line the loop runs and buys nothing the hang-up does not already deliver.

**The controller beats before it forks.** A worker gives a controller that has never beaten
exactly the staleness allowance and then leaves, so the first beat must exist before the
first worker can look — and the fork happens before any thread starts (PLAN §4.6; at M1
there are none, and the rule is written down for the milestone that adds them).

**Three ways a worker is stale, one decision.** Older than the allowance by timestamp (the
hang); fresh by timestamp but no progress for the allowance (the spin — "working slowly" and
"not progressing" are the catalogue's required distinction); and a slot that cannot be read
(a worker that died inside `publish`, which will not beat again). All three send `SIGTERM`
with `stale_when_signalled` set, and the escalation ladder takes it from there. (The third
was corrected by §2.22: unreadable for longer than the allowance, not unreadable once.)

**The ladder is a pure function of time.** `escalation_due(step, since, now, grace)` says
which signal is due; `step_after(signal)` says where that leaves the worker. The request
waits its grace before `SIGTERM`, `SIGTERM` its grace before `SIGKILL`, and nothing comes
after `SIGKILL`. IMPLEMENTATION §4's "escalate within a bounded time" is those two numbers.

**The classification is a table, and the table is the point.** `classify(Evidence)` takes
the status word, the last signal the controller sent and why, whether the finished record
arrived, and whether the channel went corrupt or ended inside a frame, and returns one of
eleven fates. The rows that must never be confused each have a test: a fault from a kill; a
kill the controller sent from one it did not; a stop at the deadline from a timeout; a clean
exit with its finished record from one without (exit 0 and silence is *evidence truncated*,
not completion); a corrupt channel over any exit code, because garbage in must never become
a verdict out. A `SIGKILL` the controller did not send is reported as exactly that — the OOM
killer's shape, with the kernel's own record to come (exit criterion 3) before it is called
by its name.

**A channel held open after the worker died is truncated, not waited for.** The final drain
gives each still-open channel one stop grace per attempt and a bounded number of attempts;
something a worker started that inherited the write end — the "fork a grandchild" row —
cannot hold the run hostage. (And since §2.20 that something is killed with the worker's
process group as soon as the worker is reaped.)

**The child of a failed exec returns rather than `_exit`s.** `WorkerLauncher`'s comment
promised `_exit` glue in `main`; the glue would be a line no measured process can run
(§1.9), and a permanent exclusion. The child is single-threaded, the streams were flushed
before the fork, and nothing in the controller registers an `atexit`, so returning the
launcher's code through `cli::run` and `main` is safe — and testable, which the fake does
with a fork that yields the child and an exec that fails.

**What the `stress` command decides, and what it refuses.** One worker per enabled workload
(the null workload has no threads to place; `threads` is carried, not used, until M2a);
`benchmark` and `explore` refused by name rather than run as stress; memory refused at start
against a budget when one is known and reported *not enforced* when none can be read;
temperature reported not enforced until telemetry. Exit 0 clean, 1 refused or could not
start, 2 usage, 3 the run ended with a worker that did not complete.

### 2.19 The fake worker: a second reading of the worker contract, chosen by the environment

IMPLEMENTATION §4 asks for a worker that misbehaves on demand, as a separate executable and
never a switch on `loadforge`. `tests/support/fake_worker` is that executable, and three
choices in it had real alternatives.

**The misbehaviour is selected by an environment variable,** `LOADFORGE_FAKE_WORKER`, a list
of `SLOT=NAME` entries with `*` for every other slot. The controller builds the worker's argv
and a test cannot add to it, while the environment passes through the controller's fork and
the worker's exec untouched. *Rejected:* an argv flag (the controller would need a way to
pass it, which is a test hook in the shipped binary, the very thing this exists to avoid);
one executable per misbehaviour (fifteen binaries, and no way to run two different
misbehaviours in one run, which N ≥ 2 wants); a configuration file named by the environment
(the same channel with a file to manage). An unreadable selector is refused with the
bad-arguments code, so a typo is a startup failure, never a different test.

**It reads the contract itself.** It parses the argv from `worker/entry.hpp`'s description
rather than calling `parse_args`, writes its own exit codes from the documented table, and
runs its own loop rather than `NullWorkload` (ADR 0003: two readings of one contract are two
chances to notice it is ambiguous). It does share the IPC library, which is the protocol's
provider rather than the worker's behaviour, and that library is now its own target,
`loadforge_ipc`: the fake links it and not `loadforge_worker`, so reaching for the real loop
is a link error rather than a review comment.

**Its tests do not race.** The supervision tests failed by scheduler luck twice (§4.11 and
the controller-killed test), so these are built so that no outcome depends on which of two
events lands first. The companion workers *outlast* the misbehaving one: they finish once
the controller has reaped every other worker they saw beat, and the run's deadline is thirty
seconds away and never comes into it, so a hang is a timeout and never a stop at a deadline
that happened to land first. A misbehaving worker begins with a *prelude* that waits for two
of the controller's beats after its own: the controller beats and then judges every worker
in the same tick, so two beats later it has read the worker's progress, and a fault test can
assert the timeline without hoping the controller looked in time. Where the controller must
not act before the worker is ready, the test asks the kernel (§1.23). *Rejected:* wide time
margins, which pass until the build gets slower, which is when they are needed. Each T7 test
passed 30 runs in 30.

### 2.20 Each worker leads its own session, and the controller kills its group when it is reaped

The catalogue row "fork a grandchild, then die" requires that no orphan be left holding a
core, and nothing did that (§1.23): the death signal is cleared across fork, and the
controller knew only its workers' pids.

**Chosen:** the launcher starts a session for each worker after the orphan-race check and
before exec (`setsid`, through a new seam call, `start_session`), so a worker leads a process
group whose id is its pid and everything it forks stays in it. When the controller reaps a
worker it sends `SIGKILL` to the group. `ESRCH`, an empty group, is the usual answer and says
nothing; a kill that found someone is recorded on the worker (`process group: killed what
the worker left running`), and any other refusal is recorded with its error, because a
worker that left processes behind is part of what it did. The teardown of a failed run
sweeps the groups of workers not yet reaped, never of one already reaped, and never of a
worker whose pid was not recorded (§1.23).

*Why straight after the reap:* the group id is the worker's pid, and the kernel does not
reuse a pid that is still some group's id, so while anything of the worker's remains the
number names that group and nothing else. Once nothing remains the kill can only meet a
reused number if the pid allocator wraps all the way round in between.

*Why a session and not only a group:* a process group of its own in the controller's
session is a background job, and with the terminal's `tostop` set a worker writing its
diagnostics to the terminal would be stopped by `SIGTTOU` and then timed out, reported for
the wrong reason. A session has no controlling terminal and no job control.

*Why `start_session` returns nothing:* `setsid` refuses only a process that already leads a
group, and the child of a fork never does. A failure arm would be a branch no test could take
(IMPLEMENTATION §2.6, rung 3). Its line is run in the test process, where `setsid` is made to
be refused (the process first leads a group of its own), and a real child shows the effect.

*Rejected:* `setpgid` alone (the `tostop` problem above); making the controller a subreaper
and reading `/proc/self/task/*/children` to find what to kill (more seam, more parsing, and a
process-wide setting with effects on everything else the controller reaps); cgroups (not
delegated everywhere this runs, and M1 has no cgroup code); leaving it, which the catalogue
forbids.

*Consequences:* `Ctrl-C` now reaches only the controller, and the workers end by hang-up or
death signal rather than by `SIGINT` alongside it; `Ctrl-Z` stops only the controller, and
the workers leave after the staleness allowance with 124, which the exit-code table already
describes. *A gap, recorded rather than closed:* when the controller itself is killed, nobody
is left to sweep, and a grandchild survives its worker; the real worker forks nothing, so this
waits for a workload that does (§6).

### 2.21 The two gates M1 added, and what each refuses

**No test hook in the shipped binary** (`tools/check-no-test-hooks.sh`). It reads the
binary's symbol table with `nm`, demangled, and refuses anything in `loadforge::fake_worker::`
or `loadforge::testing::` (the fake worker and the test fakes), anything in GoogleTest's
`testing::` (a test framework in a release binary is a test hook by definition), and the
string `LOADFORGE_FAKE_WORKER` anywhere in the file, since a binary that reads the selector can
be told to misbehave whatever its symbols are called. F20 shapes the rest: a missing, empty or
unreadable file fails; a stripped binary fails, so the gate runs before any strip, never after;
and a symbol table with no `loadforge::` symbol fails, because that is not a binary the gate
can vouch for. It runs on the static artifact in the release workflow, on every CI matrix
entry, and in `ctest` on the build's own binaries, where the fake worker must be refused by
name. Its tooling tests compile real binaries and refuse each kind of hook; no compiler is a
failure, not a skip. *Rejected:* checking the build system's install rules, which says what
the build meant rather than what the artifact holds.

**An `EOWNERDEAD` robustness test per process-shared lock** (`check-test-obligations.py`).
Every file in `src/` that names a lock primitive must be named by a declared lock in its
module's ledger row, and every declared lock must name a test under `tests/supervision/`
carrying `LOADFORGE EOWNERDEAD TEST: <name>` (the whole name, so one test cannot be stretched
over two locks), in a module that declares P5. The primitives are matched in the raw text,
comments included: a comment that names one is refused too, which costs an author a
rephrasing, where a comment-stripping bug could have waved a real lock through. A lock built
by hand from atomics in shared memory has no name to match; the design's answer is to have
none (§2.15), and review's is to notice one. Today there are none, and the gate prints so on
every run rather than printing nothing.

### 2.22 One torn read is not a death: a slot is unreadable once it stays so past the allowance

§2.15 made a torn sample the `EOWNERDEAD` of a design with no lock: a writer that died
mid-publish leaves its sequence odd, and readers report it rather than wait. The controller
and the worker then each took one torn read as that death, and §1.23 shows what that costs:
a writer that is merely descheduled between its two stores looks the same for as long as the
scheduler keeps it off the core, and a healthy worker was killed for it.

**Chosen:** each side remembers when the slot last gave a consistent sample, and a slot that
cannot be read is stale (the controller's view of a worker) or unreadable (the worker's view
of the controller) only once nothing could be read from it for longer than the staleness
allowance. A writer that really died inside `publish` stays odd for ever and is caught one
allowance later than before, which is the bound every other way of going quiet already has.

*Rejected:* more retries, or a sleep between them, in `observe` (any bound shorter than a
scheduling delay is wrong sometimes, and one long enough would make every reader wait on a
dead writer, the thing the seqlock exists to avoid); treating torn as fresh (a writer that
died mid-publish would then never be stale at all).

### 2.23 The memory budget, and an OOM kill told from the kernel's own record

M1 exit criterion 3: OOM-kill is classified distinctly from a fault, asserted, not logged.
Two pieces, one module: `src/resources` reads what the kernel lets this process use and
what it records when that runs out; the controller consults the second through a contract.

**Where the reader lives.** Platform-level knowledge, read through the seam -- but every
source is classified with topology's capability model (`Source`, `Availability`), and
`platform` may not depend on `topology` (topology reads through platform). So it is its own
module above topology, with its own ledger row. Moving `Source` down into `platform` was
the alternative; it drags `CpuList` with it and is a refactor of two shipped modules for
the sake of a third.

**Finding the cgroup: `/proc/self/cgroup` and `/proc/self/mountinfo`.** The obvious reader
joins the membership path to `/sys/fs/cgroup/memory` (v1) or `/sys/fs/cgroup` (v2). It is
right on a host and silently wrong in a v1 container without a cgroup namespace, which
names its cgroup `/docker/<id>` while the runtime mounts exactly that directory at
`/sys/fs/cgroup/memory`: the joined path does not exist, ENOENT reads as "no limit", and the
run is sized against the host inside a container with a wall much nearer. The mount's root
is in mountinfo and nowhere else, so mountinfo is read, its escapes undone, and the cgroup
taken relative to the first mount of its hierarchy whose root contains it by whole path
components. The directory is then probed (`cgroup.controllers` on v2, `memory.limit_in_bytes`
on v1): two files agreeing on a directory that is not there is a contradiction, never an
absent limit. A v1 hierarchy listing `memory` wins over the unified line, because on a
hybrid machine the unified hierarchy has no memory files.

**The limit that binds.** v1: `hierarchical_memory_limit` from the cgroup's `memory.stat`,
the kernel's own minimum over the cgroup and every ancestor -- including ancestors above a
container's mount root that no walk from inside could see (§1.24 shows the leaf's own file
is wrong). v2 publishes no effective figure, so the reader walks from the cgroup to its
mount and takes the lowest `memory.max`; a level with no `memory.max` (the root, or a cgroup
whose parent does not enable the controller) sets no limit there. *Rejected:* the leaf's
file alone (wrong whenever a parent sets the limit, which is how systemd slices and
container runtimes both do it); walking on v1 too (misses invisible ancestors, and the
kernel already did the walk); v2's `memory.high` (a throttle, not a wall: nothing is killed
at it); subtracting usage (usage counts page cache the kernel reclaims before killing
anything, so limit minus usage understates; the kernel offers no per-cgroup MemAvailable,
and F7's headroom belongs in the config as a stated number, §2.14).

**What is a reading and what is a failure.** Absent means "no limit here" in exactly three
places: no `/proc/self/cgroup` (no cgroups), no memory hierarchy in it, a v2 level with no
`memory.max`. Everything else that cannot be read -- denied, malformed, unreadable, a
hierarchy not mounted where this process can see it, a cgroup outside every mount of it --
is a failure with its source and reason, and the budget is then absent: `stress` runs with
memory reported NOT ENFORCED and the reason in the banner (the §2.18 rule, now with the
reader's words attached). MemAvailable missing (kernels before 3.14) is not estimated from
MemFree: an estimate is exactly the number F7 says not to use. The `Key: value kB` parsing
is still not shared with `numa.cpp`'s, though this is the second caller §2.11 waited for:
what the two share is the digits and the " kB", and sharing that would put a /proc parser
in topology's public surface or a sysfs one in this module's; the digits now go through one
strict `parse_decimal` here.

**OOM evidence: the cgroup's count, read before the first fork and after the last reap.**
Options, and why each was taken or not:

- *`/proc/vmstat` `oom_kill`* -- rejected. It counts every OOM kill on the machine (§1.24),
  so a rise while a worker died of SIGKILL is a coincidence on any shared machine, reported
  as a finding.
- *The controller's own memory cgroup's count* (`memory.events` on v2, `memory.oom_control`
  on v1) -- **chosen.** The workers are born into that cgroup, the kernel counts a victim
  of the machine-wide OOM killer there as well as one of the cgroup's own limit, and it is
  the narrowest record the kernel keeps. Absent before 4.13, and then said.
- *The kernel log* (`Killed process <pid>`) -- rejected. It names the pid, which is
  everything the counts lack, but `/dev/kmsg` is restricted on most distributions
  (`dmesg_restrict`, CAP_SYSLOG), the line's wording has changed between kernels, and the
  ring is rate-limited and overwritten under exactly the memory pressure that produced the
  line. Parsing a log is not reading a record.
- *A cgroup per worker* -- deferred, and recorded as the route to exact attribution: each
  worker's own count would name its own death. It needs write access to the hierarchy
  (delegation or root), and on v2 the controller would first have to move itself into a
  leaf of its own (no internal processes), which is a change to how a run is started, not
  to how a death is read.
- *`waitid`'s `siginfo`* -- no information: the waiter learns CLD_KILLED and the signal,
  never who sent it.

**Attribution: the whole run, all or none.** A count says how many, not which. When the
count rose over the run by at least as many as there are workers dead of a SIGKILL the
controller did not send, each is `kOomKilled`; when it rose by fewer, none is, and each
report says how many were counted for how many deaths. Two deaths and one counted kill mean
one OOM kill and one `kill -9`, and choosing between them would be the fabrication
`exit_status.hpp` warns against. *Rejected:* a window per death. The kernel counts a kill
before its victim can be reaped (§1.24), so a worker killed with `kill -9` and reaped in the
tick between another worker's OOM kill and that worker's reap takes the other's count, and
the two verdicts swap -- wrong about the run's own workers, not merely about a stranger. The
whole-run window cannot swap them; what it cannot exclude is a process outside the run but
in the same cgroup being OOM-killed while a worker died of somebody's SIGKILL, and the
report names the cgroup so a reader can judge how crowded it was. A count that went down,
or could not be read at either end, attributes nothing and says why. The record explains
SIGKILL and nothing else: a fault stays a fault, the controller's own SIGKILL stays the
controller's, and a SIGTERM from outside stays a kill.

**The contract and its consumer.** `resources/oom_kill_record.hpp` is the contract
(ADR 0003): a scope and a count, with the semantics a consumer may rely on written on it
(cumulative, never decreasing while the scope exists, counted before the victim can be
reaped, not per pid). The controller takes one through `RunOptions::oom_record` --
non-owning, and null meaning none is consulted -- rather than through its constructor, so
every existing caller still compiles and a run that consults nothing is a valid run whose
SIGKILLs are kills. Its tests drive the loop against a fake of its own; the reader's tests
drive the reader against trees and the kernel; the cross-execution of one against the
other waits for the contracts-first step (ROADMAP 12a). `cli::run_stress` reads both the
budget and the record through the seam it is given, so its signature did not change and
its tests feed the readers from the fake's read script.

### 2.24 The OOM row: a cgroup the test makes and moves itself into

M1 exit criteria 2 and 3 end to end need a real fake worker really OOM-killed under a
limit that the controller's OOM record reads, and the record is opened before the first
fork on the cgroup the workers will be born into: the controller's own.

**Chosen:** the test makes a memory cgroup, limits it to 256 MiB with swap off, moves
*itself* into it, and only then makes the record and runs the controller in-process
(`MisbehaviourCatalogue.AWorkerOomKilledUnderALimitIsOomKilledAndNotAFaultOrAKill`). On
v1 the cgroup is a child of the test's own (§1.24: v1 allows children of a cgroup that
holds processes, and the count is the victim's own cgroup's). On v2 the "no internal
processes" rule forbids that, so it is a sibling under the parent that already enables
memory for the test's cgroup, and failing that a child of the hierarchy's root, which the
rule exempts and root may write. A guard moves the test back and removes the cgroup
however the test ends. Where none of it can be done the test skips and quotes the
kernel's refusal for each place it tried; it never passes without the kill.

*Rejected:*

- *Fork a child that moves itself and runs the controller.* GoogleTest assertions in a
  forked child never reach the runner, so every check would need a protocol back to the
  parent; moving the test process and moving it back costs one guard.
- *Move only the workers, after the fork.* There is no moment to do it without a hook in
  the controller -- the test-hook-in-shipped-code the fake worker exists to avoid (§2.19)
  -- and a worker moved after its exec ran unlimited for a while.
- *Lower the limit of the test's own cgroup.* It limits everything else there too (the
  shell, `ctest`, parallel tests), and a test that died with it lowered would leave it so.
- *A cgroup per worker.* The exact-attribution design deferred in §2.23; a change to how a
  run starts, not a test of how a death is read.

*Making the victim certain:* the allocator raises its own `oom_score_adj` to 1000 before
it touches anything (§1.25), so whichever process charged the page that crossed the limit,
the kernel kills the allocator and not the controller or a companion. It also stops at
1 GiB and says so, so a run where no limit binds fails as a worker error rather than
taking the machine's memory.

*In CI:* the hosted runners are v2 and the tests run as an unprivileged user, so under
`ctest` the test skips. The `ci` workflow therefore runs it once more on every matrix
entry under `sudo`, with `LOADFORGE_REQUIRE_OOM_LIMIT` set, which turns a skip into a
failure, and checks that the filter matched one test. That makes exit criterion 3 a CI
fact rather than one true only on a developer's root shell. *Recorded, not hidden:* the
v2 branch of the guard has not run on a v2 host before that CI step's first run, because
none was available; the step fails loudly if it is wrong.

*The companion:* `ASigkillFromOutsideIsAKillWhenTheRecordCountedNone` runs the same record
against a worker killed by `kill -9` from the test. It needs no limit, only a count to
read, so it runs under `ctest` wherever the machine keeps one, CI included.

### 2.25 The topology snapshot before the fork: taken by the command, carried by the controller

Found by the M1 exit review: the state machine's first transition, START to RUNNING, is
labelled "topology snapshot taken (before fork)", PLAN §4.1 says the controller discovers
topology once before it forks, and the module ledger said the rule was the controller's to
honour. Nothing did: `loadforge stress` forked its workers without describing the machine
at all.

**Chosen:** `topology::take_snapshot` takes CPUs, caches and NUMA together, whole or not at
all, the first half to refuse being the answer. The stress command takes it from `/sys`
after resolving the worker executable and before the run, and refuses the run (exit 1,
the reason in discovery's words) when `/sys` will not describe the machine: the snapshot
is the frame every later report is attributed against, and a run on a machine described
wrongly would attribute everything wrongly. `run_stress` takes it by reference, so no run
reaches the controller without one; `RunOptions::topology` carries it, non-owning, and the
plan describes it in one line, which the console prints in the banner, before the spawn.

*Rejected:*

- *The controller discovers it in `prepare`.* Every one of the controller's unit tests runs
  over the system-call fake, whose reads are one script for every file; discovery inside
  the loop would have made each of them script a `/sys`. The budget and the OOM record are
  read by the command and handed in for the same reason (§2.23), and the snapshot follows
  that precedent. The controller still holds it before it forks, which is the rule.
- *A required field the controller refuses without.* The same cost to the controller's
  tests, and the requirement is already a reference in `run_stress`, the one caller that
  starts real runs. A run with no snapshot offered says so in its plan.
- *Run on with "topology unknown", as with an unreadable memory budget.* The budget is a
  limit, and NOT ENFORCED is a true statement about a run that can still be meaningful.
  An undescribed machine is a run whose every later attribution has no frame.
- *CPUs only.* PLAN §4.1's snapshot is the machine's topology, caches and NUMA included,
  and each half is already discovered against the CPUs; a partial snapshot would be a
  description nobody could tell was partial.

At M1 nothing reads the snapshot but the banner; telemetry attribution, placement and the
run fingerprint arrive from M3 and read it from there.

### 2.26 "Exit just before the deadline stop", tested without the race

Found by the M1 exit review: the catalogue row added for review finding R-02 had no T7
test. Its evidence was the controller's unit test over the fake, which models a write to a
pipe with no reader as `EPIPE` (the reading ADR 0003 records as the one that hid the bug),
and the real-binary stress test, which died by signal 13 in 2 runs of 12 before the fix and
so shows the fix only statistically.

**Chosen:** a fake-worker misbehaviour, `close-control`, that closes its end of the control
pipe at once and works on. The controller closed its own copy of the read end at spawn, so
at the deadline the stop byte goes to a pipe with no reader at all, on every run. The worker
finishes by itself three controller beats after its duration -- the third beat begins after
the tick that requested the stop has finished -- inside a stop grace long enough that no
escalation comes into it. With the controller's `ignore_signal(SIGPIPE)` changed to another
signal, the test process dies by signal 13 (status 141) in that test, every time.

*Rejected:* a worker that really exits just before the deadline. Whether the stop byte then
meets a closed pipe depends on whether the controller reaped the worker in the tick before
the deadline's, which is the scheduler's choice: the race §2.19 forbids in these tests.

### 2.27 A record pipe with no reader is the controller lost, not the worker's setup failure

Found by the M1 exit review, repeating the supervision tier 30 times under `ctest -j4`:
`SupervisionTest.AControllerKilledLeavesNoWorkerBehind` failed once with a worker that
exited 119, "record channel: write(fd 11): Broken pipe". The controller had been killed
before that worker sent its started record, the write met a pipe with no reader, and §2.17's
rule ("could not report" is a setup failure) called it the worker's own failure. The test
accepted the death signal and 124, the two ways out it knew of.

**Chosen:** the worker classifies an unsent record by its errno. `EPIPE` means nothing holds
the read end any more: the controller is gone (or stopped reading, which it does only after
a frame fault, when its own verdict on the channel takes precedence anyway), which is what
124 already means -- "hung up, stale or unreadable". Any other refusal, and a frame the
writer would not encode, stays 119. Both records the worker sends on its own account, the
started and the finished, follow the rule. `WorkerRunTest.ARecordPipeWithNoReaderIsTheController
LostNotASetupFailure` forces both; the setup-failure cases now use `EIO`.

*Rejected:* widening the supervision test to accept 119. It would also have accepted a
worker that failed its setup for any other reason, which that test exists to tell apart
from a worker ended by its controller's death; and it would have left the exit code saying
the worker failed when the kernel said its controller had gone.

*Consequence for the controller:* none while it runs. It holds every record pipe's read end
until it has reaped that worker or found a bad frame on it, so a live controller never
causes the EPIPE; a 124 from a worker it reaps is `kControllerLostReported` as before.

### 2.28 The mutation sweep runs one job per module, against the tests that create no process

Taken 2026-10-20 11:45 UTC, after the first full run was cancelled at six hours (§1.26).

**One shard per module.** `tools/mutation-shard.py plan` makes one CI job per top-level
module under `src/` that has a source file, derived from the tree on every run, so a new
module is swept from its first commit. A module with headers only is swept in the `core`
shard until it gains a source file; a shard of its own would mutate nothing and fail as an
empty run. Each shard writes a `mull.yml` whose `includePaths` are anchored at the
checkout's absolute path, and runs the gate with `--scope` (below). A verdict job fails the
run unless the plan and every shard succeeded, and each shard is cut off at an hour.
*Rejected:* a fixed list of shards (a second record of what exists, which drifts, and a
new module would go unswept without a word); balanced groups (with about 750 mutants the
work is minutes and set-up dominates, so balancing buys nothing yet, and a module's
shard would change name as the tree grows); building once and sharing the binary between
shards (it would leave the runner's filter as the only filter, and the scope check is
what makes two of them worth having).

**The gate judges one shard.** `--scope PATH` (repeatable): every mutant in the report
must lie under the scope, killed ones included, or the run is refused -- Mull mutated
what the shard did not ask for. A report that hides its killed mutants is therefore
refused under a scope. Only allowlist entries under the scope are applied or can be
stale; every refusal of the whole-run gate holds inside a shard (each fixture is run
both ways in `tests/tools/test_tooling.sh`). So that no entry escapes review by belonging
to no shard, the plan fails on one, and an allowlist line that is not a
`path:line:column:mutator` key is now refused whether or not a scope is given.

**Every mutant runs the tests that create no process.** The 21 tests that fork are renamed
`RealProcess*` and every mutant runs with `--gtest_filter=-RealProcess*`. A forked child is
the one thing Mull's timeout cannot contain (§1.26), and a mutant in fork or reap code can
also swap the two sides of a fork, sending the parent down the child's `_exit(0)` --
which Mull reads as a passing run. The name is checked both ways on every shard's binary
(`mutation-shard.py check-processes`, under `strace -f`): the filtered set creates no
process, and every `RealProcess*` test creates one, so the name cannot become a place to
park an inconvenient test that would otherwise kill mutants. *Rejected:* a list of
excluded tests (a second record that drifts); a separate binary (it moves tests between
targets that components own, ADR 0003); keeping the forking tests and relying on
`--no-mutant-output` alone (it ends the unbounded wait, but not a child left running into
later mutants, nor a parent sent down the child's path).

*The cost, and what follows.* The five mutants in `fork_process` and
`reap_any_nonblocking` (§1.26) are expected to survive the platform shard. They are not
equivalent mutants and do not belong in the allowlist. The remedy is the exclusion
ladder's first rung (IMPLEMENTATION §2.6): move the decision on the returned pid above the
seam into a function the in-process tests call, leaving the seam the bare call. Left to
its own change, once the first sharded run has named the exact keys.

**Timeouts.** `--timeout 120000` bounds warm-up and baseline (Mull's 3 s default would
fail a slow runner's warm-up for nothing); `--minimum-timeout 20000` is the per-mutant
floor, forty times the suite's run time, because a timeout is a kill and too short a one
fails open; `--no-mutant-output` removes the pipes Mull would otherwise wait on. All three
are passed on every run, including the integration test's, which now proves on the real
Mull that the test program's arguments arrive, that a hanging mutant is reported killed,
and that a report is accepted under its own scope and refused under another.

*Rejected:* building the mutation binary with LLVM coverage, which would make Mull skip
the mutants the filtered tests never reach and report them "not covered". Faster, and the
gate would refuse those mutants without an allowlist entry -- but it adds a path through
Mull (reading the profile, and matching it to mutants by file name when the paths differ)
that nothing has run against the real tool (F21). Worth revisiting when the sweep grows.

**The first sharded run** (run 39416345694, 2026-10-20 11:38 UTC, on the branch before it
merged) did what the six-hour run could not: every shard finished in four minutes or less,
set-up included, against the ten-minute estimate of §1.26. The gate ran on the real Mull
in every shard, and refused no report as malformed or out of scope. It found 56 unexplained
survivors in six of nine shards. Shards `core`, `main` and `reporters` passed.

| Shard | Mutants | Survivors |
|---|---|---|
| `platform` | 146 | 23 (`syscalls.cpp` 16, `worker_pool.cpp` 3, `clock.cpp` 2, `syscall_error.cpp` 2) |
| `controller` | 97 | 10 (`orchestration.cpp` 9, `safety.cpp` 1) |
| `topology` | 133 | 10 (`cache.cpp` 6, `numa.cpp` 3, `cpu_topology.cpp` 1) |
| `worker` | 117 | 8 (`heartbeat.cpp` 4, `record.cpp` 2, `entry.cpp` 1, `scheduler.cpp` 1) |
| `resources` | 70 | 4 (`cgroup.cpp`) |
| `config` | 39 | 1 (`config.cpp`) |

The `fork_process` and `reap_any_nonblocking` survivors predicted above are among the
platform shard's (`syscalls.cpp:91`, `:99`, `:410` and `:416`). Most of the others are
boundary mutants (`<` to `<=`, `>` to `>=`) that the 100% branch coverage reaches but no
test discriminates. That is the gap mutation testing exists to find. Each is killed by a
test at the boundary, or allowlisted with a written reason if it is equivalent, in
follow-up changes per module (ROADMAP item 11b). The sweep merges first: it is weekly and
gates no pull request, so a red sweep holds nothing up and names exactly what is left.

*Platform, 2026-10-20 15:44 UTC.* All 23 killed by hand, none allowlisted. The five in
`fork_process` and `reap_any_nonblocking` went as planned above: the decision on the pid
is now `fork_result` and `reap_result`, pure functions tested at -1, 0 and a positive pid,
and the seam methods are the bare call and an errno read. In the rest of `syscalls.cpp`,
a return of 0 from `open`, `memfd_create` and `write` is a real success (descriptor 0 with
standard input closed; a zero-byte write) and is now tested; `readlink` and a blocking
`waitpid` never return 0, so they test for -1, which removes the boundary. The five mask
mutants survived because the affinity tests restored the process's mask *through the
conversion under test*: a conversion that kept only CPU 0 pinned the process there, after
which the glibc cross-check saw one CPU both ways. The conversions are now public and held
to `CPU_SET`/`CPU_ISSET` on all 1024 bits (nodes too: a `cpu_set_t` is the kernel bitmap
type `mbind` reads), and the restore goes through glibc. `exec`'s reserve is killed by an
empty argument list; `describe`'s reserve is gone; the clock's two are message arithmetic,
now asserted exactly; `WorkerPool`'s two slot loops are one `std::find_if`, and `pid_at` is
killed at slot 0 of an empty pool, where the mutant reads storage never allocated. The new
tests met §1.22's trap twice: `Reaped`'s and `CpuMask`'s defaulted comparisons are now
defaulted out of line in `syscalls.cpp`.

*The controller and worker shards' 18 survivors* (2026-10-20 12:59 UTC) are all gone, and
none is allowlisted. Most fell to a test at the boundary: a memory total equal to
`limits.memory`, an allowance equal to the interval, a region of `kMaxWorkers` attached, a
frame of the largest payload read back. The fake's clocks start at zero, where `now - last`
and `now + last` agree, so both torn-slot tests (controller and worker) also run with the
clock a day past zero. The beat count, the poll timeout and the teardown's wait count are now
asserted. A reap after the last worker meets a scripted `ECHILD`, as from the kernel. A pipe
end numbered 0 (what the kernel hands out when standard input is closed) is closed by the
teardown, through the fake's new `number_pipes_from`. The rest were equivalent and are gone
from the code. The teardown's `live_ > 0` guard decided nothing that the wait loop did not.
`put_u32` is unrolled, and `slot_at`'s span takes the region's length. The seqlock writer's
two stores now both derive from the odd value, so a wrong sign leaves the sequence unchanged,
and every reader sees that. The torn reader reports the attempts it made, so 65 no longer
passes for 64. Each kill was proved by hand: mutate, build, and see the named test fail under
`--gtest_filter=-RealProcess*`.

**The `topology`, `resources` and `config` survivors (ROADMAP item 11b).** All 15 are
killed or removed; none is allowlisted. Seven were boundary mutants killed by tests at the
boundary: id 65535 accepted (`cpu_topology.cpp:46`, `cache.cpp:72`; tests
`TheLargestIdTheKernelCanAssignIsAccepted`, `TheLargestIdACacheAttributeHoldsIsAccepted`),
two L1 caches of one CPU that do not nest (`cache.cpp:236`, `TwoCachesAtOneLevelNeedNotNestInEachOther`),
a claim of an offline CPU while other CPUs are online (`numa.cpp:120`, which a one-CPU
fixture cannot tell from a correct claim), the octal escape `\377` (`cgroup.cpp:57`, `:62`),
and a configuration lying past the first 4 KiB read (`config.cpp:511`: the old padding was
a trailing comment, so a truncated read parsed the same; the padding now comes first). Eight
were equivalent as written, because the comparison sat after an inequality test or one
element past the end of a view that always has a space or NUL there, and were refactored
away: `ordered_before` is one tuple comparison (`cache.cpp:298`, `:301`, `:303`), and the
digit scans (`cache.cpp:332`, `numa.cpp:46`), the escape length check (`cgroup.cpp:51`)
and the separator search (`cgroup.cpp:188`) use `find_first_not_of`, `substr` and
`std::find`, which cannot read past the end. Each boundary kill was proved by hand: apply
the mutation, rebuild, run the module's tests, see the named test fail, revert.

### 2.29 The sort and Monte Carlo specifications

*2026-10-20 12:34 UTC.* The wave-2 prerequisite (ADR 0004: a workload's specification
lands before its wave). Each header names its workload (`kName`), states every range as
the generator produces it, pins which failure is named when several occur and what
`blocks_compared` counts, and says what a version change means: the lessons wave 1's
streams recorded against the first two specifications.

**Sort — chosen:** the output carries a witness, each key's input position beside it (a
keys region, then a u32 index region), and the order is on (key, index), so the sort is
stable and the output a function of the input alone: `BitExact`, `kExact`. The oracle
checks in three steps, the first failure deciding: the indices are a permutation (first
out-of-range or repeated index named, no difference); every key equals the input key its
index names (a `Difference` over the keys' region, its expected bytes known exactly); the
pairs strictly increase (first break named, no difference). Linear, no sort, and one
flipped key bit is reported as one flipped bit, because the index check runs first and an
index flip can only read as an index fault. The decomposition is a chunk count, which
cannot change the output, so the T6 tier can vary it as `BitExact` requires.
*Rejected:* keys alone, checked by a multiset hash (probabilistic, no expected bits), by
binary-search counting (O(n log n), a scratch array, still no expected bits), or by a
second sort (verification.md §3 forbids it); keys generated so that their rank is
computable (the check becomes a bucket placement, and the data stops being uniform 64-bit
keys); no decomposition at all, as compression has (the T6 sweep over decompositions would
have nothing to vary).

**Monte Carlo — chosen:** the estimate of pi/4 as the mean of sqrt(1 - x^2): one
multiplication, one subtraction and one square root, each correctly rounded and unfused,
so every f_k is the same double on every path and in the oracle. The input is one Philox
key per batch, so the samples are drawn inside `compute()` and the input-integrity bracket
still covers what they are drawn from. The decomposition is the chunk length of the
per-batch sum, within which the order is the ISA path's. The tolerance is a rounding
bound, derived in full in the header: two sums of the same S non-negative terms differ by
at most 2a/(1-2a) of either, a = (S-1)u, which is at most 2Su = 2^-38 whenever
2S(S-1) <= 2^53 (a compile-time assertion). The tolerance is therefore an exact
power-of-two scaling, and Sterbenz's lemma makes the residual exact wherever the verdict
could turn, so the (1 ± epsilon) tests decide the exact comparison.
*Rejected:* a statistical bound (the oracle replays the same samples, so a bound of a few
standard deviations, about 28.6 per batch, would pass nearly any corruption and
false-alarm at its own significance level); a transcendental in the integrand, or normal
variates by Box-Muller (`log`, `cos`: not reproducible, determinism.md §3); FMA in the
integrand (x86-64's baseline has no FMA instruction, so the scalar path's `std::fma` would
be a library call; unfused, every path's f_k is identical); random numbers stored in the
input (the unit becomes a memory stream, and Philox, the load the brief names, leaves
`compute()`); a tolerance that depends on the chunk length (tighter, but one per
decomposition, where the T6 comparison across decompositions needs one bound).

**Left open:** the compression and dense-linear specifications carry no `kName` and say
less about versions than these two. Bringing them level is a contract change of its own,
between waves.

### 2.30 The wave-3 specifications: FFT, stencil, sparse, graph, particles

*Written 2026-10-20 15:55 UTC.* Five frozen headers, `src/workloads/<name>/spec.hpp`, each
with a `kName`, a `kVersion` that versions the specification (the workload's own bits
are `Descriptor::version`), the exact generator, the decomposition, the class, and the
check with its bound derived in the header. Every bound was run against real
computations in a throwaway harness (a long-double reference, fused and unfused,
shuffled summation orders); the observed error is 1-5% of the bound everywhere
(FFT 2-norm 2%, stencil 3%, particles 0.8%, sparse rows 12%).

**Decisions, and what was rejected**

- *FFT.* Radix 2 and 4 only, so a butterfly is twiddle products plus sums with factors
  ±1, ±i and no rounded constant; the bound is then Higham's 2-norm recursion, proved in
  the header. Twiddles are *defined* as the correctly rounded table (unique, so two
  tables agree bit for bit; `|W - w| <= u`). Rejected: radix 8/16 (rounded √½
  constants break the stage model); libm twiddles (not reproducible, determinism.md §4);
  a per-bin bound (a rigorous one is no tighter than the 2-norm one, so the single-bin
  tolerance is about 50x looser than the observed error; accepted). Round trip left to
  the workload's tests: it needs a second FFT. Parseval is checked on all bins.
- *Stencil.* The update is `u/2 + (N+S+E+W)/8`, not `u + α(Σ - 4u)`: non-negative weights
  summing to 1 give the maximum principle, so the error is `2γ_{4T}·M0` with no growth
  factor. Ring is fixed. Tile edge is the decomposition, though the spec does not
  require bits to differ across tile edges (recorded as a possible contract question).
- *Sparse.* Plain CG, 16 fixed iterations, no preconditioner (the diagonal is constant, a
  Jacobi one would change nothing). The output carries the workload's explicit residual
  so the oracle's row-by-row check against its own SpMV is rigorous (`γ_{m+1}`). The
  residual-norm check uses the exact-arithmetic CG bound `3·5⁻ᴷ‖b‖`; finite precision is
  *not* proved, only shown to leave five orders of room (observed 2.2e-16 vs 2e-11).
  Rejected: a derived finite-precision CG bound (none is closed-form); a 2^10 safety
  margin (it would blunt the check for no gain).
- *Graph.* PageRank's arithmetic is pinned (accumulator count `a` is the decomposition,
  `c + fl(d·S)`, no FMA), so the ranks are one byte string per `a` and **the oracle is
  exact** (`Contract::kExact`), covering the whole output by a full scalar recompute
  rather than a subgraph. Class stays BitExactFixedDecomposition (bits differ across
  `a`; they happen to agree across ISA paths). Rejected: a bounded PageRank (needs a
  second verdict kind on one oracle) and splitting into two workloads.
- *Particles.* A cutoff force `(1-q/4)²·d`: a neighbour list finds exactly the pairs of
  the direct sum, so the only difference is summation order. Rejected: Barnes-Hut
  (approximation error is not arithmetic and cannot be derived). The evaluation model of
  one term is pinned, since the bound holds only for that model.

**Contract questions for the manager.** (1) Sparse check 2 rests on floating-point CG
following the exact bound; the sparse stream should record the worst
`‖r‖/‖b‖` over seeds and ISA paths. (2) The stencil and particles specs let the tile
edge / cell size change bits without requiring it; if a stream finds none, the class
is revisited by contract change. (3) The ledger fragments and
`tests/contracts/headers/workloads_<name>_spec.cpp` were added alongside, as a7791a2 did.

### 2.31 Wave 4's specifications: ray tracing and adaptive mesh (2026-10-20 13:05 UTC)

Both headers (`src/workloads/raytrace/spec.hpp`, `src/workloads/adaptive_mesh/spec.hpp`) are
contracts under ADR 0004. Their definitions are tested in
`tests/contracts/workloads/spec_raytrace_test.cpp` and `spec_adaptive_mesh_test.cpp`, each in
its own file so that the specifications other sessions add beside them cannot conflict.

**Ray tracing.** A unit is one 64 x 64 frame of 128 spheres and 128 triangles, seen from a
fixed pinhole; the output is a 16-byte record per pixel (t, primitive index) and one sum of
t per tile. *Chosen:* the arithmetic is shared and the algorithm is not. The draws'
decoding, the camera ray and the two intersection tests are defined once, as code in the
header, in an exact sequence of binary64 operations (no fused multiply-add, no reciprocal);
the workload's hierarchy and the oracle's brute force then have to produce the same bits,
so the check is exact (`Contract::kExact`), not bounded. *Rejected:* each stream writing the
intersection arithmetic from prose. A different dot-product order is one last bit, and one
last bit is a false corruption report on healthy hardware; the independence that matters is
hierarchy against brute force, and it is untouched. Shared code can share a typo, which is
why every branch of both tests is taken in the contract tests and two values are checked
against a computation made outside C++. *Also rejected:* secondary rays, shading and a
camera in the input. Each adds operations to pin and nothing the oracle needs; the camera
and the eye are fixed so every hit is at t >= 3.75 and the near root of a sphere is the hit.
*Tie rule:* the lower primitive index wins an equal t, so brute force in index order is
unambiguous. *Oracle sampling:* 64 pixels, one in each tile of the 8 x 8 tiling, so every
region is sampled whichever tile edge is configured. *Failure and count:* the items are
evaluated in a fixed order, the first failure is named, and `blocks_compared` counts the
items evaluated, the failing one included.

**Adaptive mesh.** A unit is 256 hotspots on a 512 x 512 lattice; a quadtree is refined
around them (integers only), and the output is, for each of 16 x 16 coarse cells, the
integral of a rational field over its leaves and the leaf count. *Chosen:* everything is
exact or pinned except one thing, the order in which a coarse cell's leaves are added by
the threads that finish them. That is the dynamic load balancing that makes the class
`kBoundedResidual`, and it is also what the tolerance is derived from: any order of m
positive terms is within gamma_{m-1} of the exact sum (Higham 4.2), the oracle's fixed
order likewise, so the two differ by at most 2 gamma_m c' / (1 - gamma_m); the header derives
it step by step, including that the subtraction is exact where it matters (Sterbenz), that
the denominator 1 - 2mu is exact, and a slack of 1 + 2^-40 that keeps the computed tolerance
above the real one whatever its roundings. The contract tests check that excess on both
sides for every m from 1 to 1024. *Rejected:* a stencil or an advected field, whose
oracle would be the workload run again; a field using exp(), which determinism.md §3 rules
out; a tolerance left for the oracle stream to derive, which is how dense-linear's spec was
left open and why this one is not. *Leaf counts* are compared exactly by a bounded oracle
(`BoundedMismatch`, tolerance 0.5, residual the integer difference), because
`ExactMismatch` is not valid for a bounded contract; all 256 are checked, the integrals of
32 sampled cells.

**Contract question for the manager.** The roadmap declares ray tracing
`kBitExactFixedDecomposition`. The specification makes its pixel records identical on every
ISA path and architecture, and only the tile sums depend on the tile edge, so the workload
would hold `kBitExact` over the pixels. The tile sums were added so that the declared class
is true and the decomposition (the tile edge, 8, 16 or 32) is real; if the manager prefers
`kBitExact`, the tile sums and the decomposition come out of the header and nothing else
changes.

### 2.32 EXT-01: an attempted signal is not an established cause

*2026-10-20 19:30 UTC.* Context: §1.27. Review: `reviews/review-response-2026-10-20-0352-UTC.md`
EXT-01; plan: `reviews/reaction-plan-external-review-2026-10-20-1230-UTC.md` item 1.

**Chosen.**
1. *The seam gains `has_exited(pid)`* (`waitid` with `WNOWAIT`, bare in `RealSyscalls`, the
   decision in `exit_peek_result` above the seam). `Run::send` looks before every signal. A
   worker already exited is **not signalled and no signal is recorded**; its report gets the
   note "signal N not sent: the worker had already exited". A refused look (any errno,
   `ECHILD` included) is a controller error at stage `signal`. An `ESRCH` from `kill` after
   a look that said running cannot happen to an unreaped child; if it does it is tolerated
   and **not recorded**.
2. *`Evidence::signal_sent` is renamed `signal_attempted`*: the last signal sent to a
   worker the look showed running. It is an attempt and nothing more.
3. *Every SIGKILL death is an OOM candidate*, the controller's attempted SIGKILL included,
   so the count is always consulted when a record is offered. The all-or-none rule of
   `judge_oom_kills` is unchanged and now counts those deaths too; the note no longer says
   "the controller did not send". `OomFinding` gains `ruled_out` (read at both ends, did not
   rise); `Evidence` gains `oom_inconclusive` (consulted, neither attributed nor ruled out).
4. *The verdict for a death by SIGKILL that matches an attempted SIGKILL:*
   - the count rose enough: `kOomKilled` (the rise outranks the attempt);
   - the count could not speak (unreadable, went back, rose by fewer than the deaths):
     **`kKilled`, the provenance ambiguous**, the report keeping "SIGKILL sent" as the stop
     step and the count's note as the reason;
   - the count was read and did not move, or no record was offered: the controller's stop
     or timeout, as before.

**Why a flag-free reading of existing fates and not a new outcome.** `kKilled` already
means "killed, with nothing to say who sent it". A new fate would reach every reporter and
the contract for a state that is exactly that. The companion requirement (the controller's
own kill of a running worker stays its stop) rules out calling every attempted SIGKILL
ambiguous; the count is the one witness the kernel offers, and it is used.

**Rejected.** Treating `ESRCH` differently alone (the review: a zombie kill succeeds).
Reaping before signalling (reaping inside `send` would break the one-reap-loop design and
still leave the race). A new `kProvenanceAmbiguous` fate (above). Using a separate candidate
class for the controller's kills so the pure candidates keep their precision: it would let a
partial rise attribute some deaths and not others, which is the inference the all-or-none
rule exists to refuse.

**What stays open, and is said so.** A SIGKILL from outside that lands between the look and
the controller's own kill, with the OOM count unmoved, is reported as the controller's stop.
The window is the gap between two system calls; no interface closes it. A mixed run (one
controller kill, one OOM kill, count +1) now attributes neither: the cost of the all-or-none
rule with more candidates.

**Tests, each red before the fix** (run against the unfixed logic, 13 failures including the
real-kernel one): `MisbehaviourCatalogue.AWorkerKilledFromOutsideBeforeTheControllersSigkillIsNotBlamedOnIt`
(outside SIGKILL held unreaped with `WNOWAIT`, then escalated; the old controller reported
`kStoppedByController`), its companion `TheControllersOwnKillOfARunningWorkerIsStillItsStop`,
the fake-driven `ControllerExitedTest`, `ControllerOomTest` and `AttemptedKillTest` rows.

### 2.33 EXT-02: startup is its own supervised phase, and the duration starts at the last launch

*2026-10-20 20:15 UTC.* Review: `reviews/review-response-2026-10-20-0352-UTC.md` EXT-02; plan:
`reviews/reaction-plan-external-review-2026-10-20-1230-UTC.md` item 2.

**Verified before changing anything.** All four claims held in the code: `started_` was
taken in `prepare()`; `spawn()` forked every worker through one `WorkerPool::spawn` with no
controller beat between forks; each worker's `progress_changed_ns` and `last_read_ns`, and the
"never beat" allowance in `watch()`, were measured from `started_`; and the duration was
compared against `started_`. The regression tests below were run against the unfixed
controller and all four failed.

**Chosen.**
1. *The controller beats between forks.* `WorkerPool::spawn` takes an optional observer, called
   in the supervisor after every fork that stays in it (never in a child). The controller's
   observer reads the clock, beats slot 0, and records the worker's launch time. No thread is
   added: the controller still forks before any thread exists (PLAN §4.6). A clock failure
   there is the controller's failure at stage `clock`, and the pool unwinds every worker
   already started, as it does for a failed fork.
2. *Each worker's allowance is its own.* The first-heartbeat allowance, the progress clock and
   the unreadable-slot clock all run from that worker's launch (`Tracked::launched_ns`), no
   longer from before the first fork.
3. *The configured `run.duration` starts when the last worker has been launched* (`started_`
   is overwritten by each launch and so ends as the last one's). Starting the workers is not
   part of the duration. Each worker also paces itself by the same duration from its own start
   (unchanged), so every worker runs the full duration and the whole run lasts the time the
   forks took plus the duration. Stated in the `loadforge --help` text, `config/quick.toml`,
   `config::Run::duration` and the controller header.

**Rejected.** A bigger global staleness allowance (trades false positives for slower detection
of a real hang, as the review says). A beat thread (unsafe before the last fork; the header
promises none). Counting the duration from `prepare()` (a short run on a slow machine would
ask for its stop before late workers ran). Counting it from the first launch (the first worker
would run longer than the last by the spawn time).

**What stays open, and is said so.** One fork that by itself takes longer than the allowance
still leaves earlier workers watching a beat that cannot be refreshed during that call; only
a pause between forks is covered. That is a single system call blocking for seconds, which
the controller cannot interleave without a thread.

**Consequence for the tests.** The two extra clock reads (one per launch) move every tick of
the fake clock, so the timing expectations in the existing controller tests were recomputed;
a worker that never beats is now stale a launch-interval earlier than the one forked after it.

**Tests, red before the fix:** `ControllerStartupTest` (a delay injected at the fork boundary
through the shared fake, 60 ms per fork against a 100 ms allowance): a running worker never
finds the controller's beat stale while it forks; a worker forked late is judged stale only
once the allowance has passed from its own launch; the deadline falls the configured duration
after the last launch; a clock failure between forks is the controller's failure and unwinds.
The pool's observer has its own tests in `worker_pool_test.cpp`.

### 2.34 An oracle is strict where the specification allows strictness

*Wave 1 (stream 03, `docs/journal/03-oracle-compression.md`).* The LZ4 oracle refuses every
end-of-block breach, offset 0, and a last token with a match length, though `lz4 -d` accepts
each (§1.33). A lenient oracle would agree with a compressor that writes what the document
says cannot be there, and agreement is the one answer an oracle must not give falsely.
*Rejected:* matching the reference decoder's behaviour, which is an implementation's choice.
"End of block" in the 12-byte rule is the end of the *decoded* data (the document counts
decoded bytes; the reference compressor's limits are on its input), and both margins are
applied to every match, which is the same rule checked where it breaks. The last token's
match field must be 0, which also makes a flip of that nibble a caught corruption. These
readings became the specification's words in #84. The oracle's scratch is a seam of its own,
its failure P2, rejecting a 64 KiB stack buffer (an executor thread's stack is not the
module's to assume) and thread-local scratch (64 KiB of static TLS in every thread).

### 2.35 AVX2 by a target attribute, never by `-mavx2`; no AVX-512 kernel

*Waves 1 to 4 (streams 06, 18, 26, 30).* A unit compiled with `-mavx2` compiles every inline
function it instantiates under AVX2, and the linker may keep that copy for a scalar-path
caller: an illegal instruction on a machine without AVX2, in a function that has nothing to do
with it (§1.36). Each kernel instead carries `__attribute__((target("avx2,fma")))` (`"avx2"`
alone where no FMA is used), the shared body is `always_inline` into it, and a type local to
the unit keeps its copy from being merged with another's. NEON needs nothing: it is baseline
on AArch64. No workload has an AVX-512 kernel: its lines could be covered only on a runner
that has the feature, GitHub's x86 runners are a mix (AMD EPYC Zen 3 has none), and the rule is
100 % with no exclusions, so the gate would pass or fail with the runner it was given.
`create()` refuses `kAvx512` as `kConfiguration`, never replaces it; a kernel waits for a
coverage runner that guarantees the feature. Tests of a path the machine cannot run fail,
they do not skip: a skipped path is a path no test ran. The scalar kernel does not fuse
(`std::fma` without `-mfma` is a library call per multiply-add, the one thing a stress kernel
must not be). Proposed ADR: `docs/adr/proposed/06-workload-dense-linear-vector-kernels.md`.

### 2.36 Where a workload's vector kernels live

*Waves 1 to 4 (streams 06, 18, 22, 23, 24, 25, 26, 30); settled workload by workload, not as a
rule.* Three sources read differently: `spec.hpp` and the briefs put AVX2 and NEON kernels in
the workload's directory; `dispatch.hpp` and `portability.yml` allow intrinsics only under
`src/primitives/simd/x86/` and `arm/`, which stream 06 owns. Options each stream weighed:
(a) intrinsics in the workload's directory, refused by the portability gate; (b) intrinsics in
the primitives directories, which a workload stream may not edit and whose CMake fragment
collides; (c) portable code in the workload with a function `target`; (d) scalar only, the
table's other slots left null for a later stream. Taken: dense linear (b, its own stream);
stencil, particles and ray tracing (c, the compilers' `vector_size` types); Monte Carlo (c, lane
loops); FFT and sparse (d). Graph first used intrinsics in its own directory and moved the
sweeps into the primitives layer on 2026-10-21 (b), unchanged in behaviour; rejected there were
an exemption (the rule is the point) and vector extensions (they hide the instruction choice a
bit-exact contract depends on). (c) costs that the compiler picks the instructions, no FMA is
used, and the AVX2 path does not stress the FMA units. A descriptor's `paths` is the kernel
table's in every case, so a descriptor and a factory cannot disagree. Whether a workload stream
may own files under `primitives/simd/` for its kernels is open (§6).

### 2.37 The executor: one thread per placement, the caller runs nothing, one counter

*Wave 1 (stream 08, `docs/journal/08-executor.md`).* Threads start through a seam of the pool's
own over `pthread_create`/`pthread_join`, one at a time, each placing itself and checking its
registers against its creator's. A pool of *n* placements has *n* threads and slot *s* is
thread *s* for its life; the calling thread waits. Indices are claimed one at a time with
`fetch_add` on one `seq_cst` counter; every thread wakes for every run; everything a run
publishes is under one mutex. *Rejected:* `std::thread` (a failed start throws below the seam,
and no test on a root runner can make one fail); starting all threads and collecting reports
(every seam becomes concurrent); a caller that joins in (it would run tasks on whatever CPU it
happens to be on, and a placement means something only if the thread placed is the one that
works); a static partition (a descheduled thread holds up the run); a claim under the mutex (a
lock per index); waking only as many threads as indices, or spinning before sleeping (both cut
the 75 µs of §1.50, both make which arm a test takes depend on the scheduler). Every wait's
first test is fixed by the protocol, never the scheduler (§4.11). Orderings in the primitives
(stream 20) are relaxed counters and release/acquire hand-offs, each also run at `seq_cst`;
ordering tests corroborate and cannot prove (PLAN §4.5), and the mutation table in the
fragment shows which weakenings they do catch.

### 2.38 A unit's scratch is one piece, not one per slot

*Waves 2 to 4 (streams 17, 22, 23, 24, 25, 26, 31); decided by none of them.*
`Plan::scratch_bytes` is "per executor slot", and the contract says slot *s* owns the *s*-th
piece. A sort, an FFT, a stencil, a sparse solve, a graph and a particle structure each need
*one* shared buffer, and every one of those streams met the same wall. The reading taken by
all: the unit uses the **first** piece, sized to hold it, and the other `threads() - 1` pieces
are unused; the calling thread owns the whole span between `run()` calls and builds shared
structure before the tasks, which only read it, and slots own their pieces while tasks run.
Cost: memory the runner reserves and nothing uses (3.5 MiB for 0.5 MiB at seven threads, in
the stencil). *Rejected:* slot-private scratch with a shared arena built by one task (the
unit's memory would depend on the executor); a lock or an atomic per cell (stream 31 keeps a
partial sum per slot and combines in slot order, so no word is written by two threads, and the
executor's claiming is the "shared queue" the specification names). Ray tracing sidesteps it
with `scratch_bytes` 0 and a hierarchy on the stack. A contract that named a per-unit figure
would remove the waste; it is open (§6), together with the converse, that `run()` makes the
caller's earlier writes visible to the tasks, which the sort and the primitives both rely on
(streams 17 and 20).

### 2.39 The runner: serial units, one locked working set, a vote of three

*Wave 1 (stream 02, `docs/journal/02-input-integrity.md`).* Units run one after another, the
executor going to each unit's `compute()`, which keeps §4a's temporal invariant by construction
(nothing but the unit touches its slot between its write and its closing check) and gives
`compute()` an executor of its own. *Rejected:* units over the executor with a serial one for
`compute()`, which would change what a parallel workload is measured doing. One mapping holds
every slot's input and output, the regeneration buffer and the scratch, populated and locked,
always: a page swapped out and back is a different physical page, and a finding on it would be
attributed to memory it never stayed in. Not huge pages (a recorded choice of the run, PLAN F7,
and the runner has no field for it). The control state (seed, next index, duty cycle, line size)
is three copies voted once per unit: two verified copies that agree decide; one verified beside
two that fail is `kTooFewVerified`, a winner picked and not a majority found, which F18
forbids. A vote before each step was rejected: five reads for no protection the per-unit vote
lacks, since the steps' working values live in registers. The vote guards state *at rest*, from
the end of one unit to the start of the next, because the copies are rewritten after every unit
from the values voted at its start. A run that cannot decide stops and says why (never an
`ExactBitCorruption`, never isolated). The slot geometry is not protected: a corrupted stride
crashes loudly, which the supervisor sees.

### 2.40 The isolation ladder: the nearest other core, and only a clean re-run is clean

*Wave 1 and the M2 integration (streams 10 and 11, `10-isolation.md`, `11-integration-m2.md`).*
The ladder's second rung re-runs a unit on **the nearest other physical core**, which departs
from the brief ("a different cache domain and NUMA node where the topology offers one"),
because verification.md §7 reads the two answers only if the other core shares memory and
uncore with the original: a reproduction across a cluster is barely consistent with the
original's cache, and a clean one there reports a core-local fault for a fault in a shared L2
or an L3 (`InjectedFault.ASharedClusterCacheIsFoundNotCoreLocal`). Distance adds nothing to the
finding for a reproduction, which is already "not core-local, high confidence". The order is:
same node, lowest shared cache level, same *shape* (thread count and private caches, the
snapshot's stand-in for core kind on hybrid and big.LITTLE parts), then CPU id; never an SMT
sibling. Heterogeneous layouts are modelled, not captured. In the integration (stream 11) a
re-run is a process of the same binary with `--rerun`, pinned to the rung's CPU, answering by
exit code (0 clean, 20 reproduced, 21 not performed, 22 bad arguments), and **any other exit,
any signal, a timeout is "not performed", never "clean"** (§2.32): the ladder's most alarming
finding is reached through a clean re-run. `stress --test` runs one single-threaded worker per
CPU, each pinned and each with its own seed (`seed + slot × 0x9E3779B97F4A7C15`), because that
is the shape in which a finding names a CPU; a multi-threaded unit leaves `logical_cpu` unset
and is verified and not isolated, and says so on the finding. An unprivileged run that cannot
lock its working set is refused cleanly, not degraded (§1.40).

### 2.41 What the controller believes of a worker, and what it keeps

*M2 integration (stream 11).* Error records come in frames of type 16, tallies in type 17 (a
version byte and seven counters), **cumulative** and sent after every slice, so a frame never
read costs a stale figure and not a wrong one; the 64-byte heartbeat slot was not widened (it
would change the M1 protocol for no reader that needs a lock-free tally). A record is kept only
if the frame's checksum verified, the payload decodes to a record the contract allows, and it is
about the workload the worker was told to run; a tally smaller than the last is not a running
total. Each refusal closes the channel and makes the worker `kChannelCorrupt` with no finding:
tested over seven kinds of garbage, a bit flipped at each of 400 positions of a frame, and 300
well-framed random payloads. The controller keeps the first 256 error records of a worker and
counts the rest, and isolates at most 16 wrong results per worker, after the workers are gone,
one at a time. A persistent fault fills the 64 KiB record pipe faster than one read a tick
empties it and blocks the worker in `write(2)`, which looks like a hang, so a woken channel is
drained until `poll` says empty, capped at 64 reads a tick. A hang is ten seconds for a
verifying worker and two for a null one, a constant and not a key, since a key is a thing to
tune until a failure goes away. The cache-line size is the topology's or the run is refused: an
assumed 64 would classify every multi-bit error by a boundary nobody measured.

### 2.42 Counters: wrap, reset, estimate, and the unit the number is in

*M3 integration and its corrections (streams 13 and 21; `m3-contract-corrections-2026-10-21-1145-UTC.md`,
#92; ADR 0004 on wrap).* Four findings from running the real stack, each a test registered
`WILL_FAIL` until the contract moved; the options are written out in that file.
- **Unit.** +20 J rendered as "delta 20000000 joules". Taken: name the unit the raw value is
  in (`Unit::kMicrojoules`) and state that a counter's raw value is in its metric's unit,
  exactly. *Rejected:* a scale on `Metric` (every consumer must apply it, and one that forgets
  is wrong by 10^6, silently); converting in the source (an integer counter can only go to a
  finer unit, which it already is in).
- **An estimate is not a counter.** A perf count scaled for multiplexing went from 2000 to 1500
  and `counter_delta` read the step down as a wrap: 18,446,744,073,709,551,116. Taken: report
  the raw count, which only increases, and one gauge, `counters.pmu_share`, the least
  `time_running / time_enabled` of the hardware events in the round. *Rejected:* a gauge for the
  count (no delta, and the run's instructions are what a counter is for); the raw count plus
  enabled and running times as ten more counters (complete and noisy); the raw count alone
  (honest about the number, silent about its quality).
- **A step down in a counter that cannot wrap is a reset.** `modulus` now means "wraps within a
  run" (absent: does not). The advance across a reset is unknown, not guessed, exactly as across
  an `Unavailable` reading, and the reading after is the new baseline; `counter_advance` returns
  it or nothing, and `CounterSummary::resets` counts them (result schema 1 to 2). EDAC declares
  no modulus: at a thousand errors a second a 32-bit counter wraps in 50 days, longer than a
  run, and what makes it go down is `reset_counters` or a driver reload. *Rejected:* a `wraps`
  flag (a second way to say what an absent modulus says, and the two can disagree); reading a
  step down as a wrap only when the previous reading was near the modulus (a heuristic in a
  contract, right by luck). Wrap detection stays above the source (ADR 0004), and the Load
  Signature never adds an advance across a gap.
- **Software events are opened with the kernel included** (§1.41), the only way to count a
  context switch. At paranoid 2 without `CAP_PERFMON` the metric is *denied, with a remedy that
  says so*, not present and 0; hardware events stay user-space only and need nothing at
  paranoid 2. *Rejected:* keeping `user_only` and never having the metric; dropping it.

### 2.43 Sampling points, phase windows, and who probes

*M3 integration and its corrections (stream 21, `docs/journal/21-integration-m3.md`; the M3
corrections file; stream 11's merge of both).* Sampling is in the controller's loop, as rounds at
four fixed points: before the first fork (a counter's baseline before any worker can move it),
every interval, at the deadline on the instant the next phase is marked, and after the last
reap. *Rejected:* a sampler thread (the controller's only thread, a P5 argument, for no gain at
1 to 10 Hz, and fork before threads is the rule); rounds in the workers (the observer inside the
observed). A round not later than the last is skipped, not refused, because two ticks can read
the same clock tick; a round the timeline refuses ends sampling and is reported, never the run.
**A phase's window closes one nanosecond past its mark** (`statistics::kClosingMargin`,
`run_window`, `phase_window`), so the reading on a mark is in both neighbours and every pair of
consecutive readings is in exactly one phase: phases sum to the run. *Rejected:* a window that
owns the pair whose *earlier* reading is inside it, or whose *later* is (both change `delta` for
every window that ends between readings, and the second assigns the advance to the wrong
phase). The obvious `[mark, next mark)` gave phase deltas of 2 where the run's was 5. The
sampler's `probe()` is the only prober and asking again is free (`probe_capabilities()` is
removed): it must call `probe()` to register the series anyway, and a source that cannot take a
second probe would otherwise leak or double-register. The capability report reaches the
controller as rendered text from the caller's sampler, so the controller cannot probe twice
(*rejected:* a `CapabilityReport` in `RunOptions`; always sampling).

### 2.44 A counter blind for part of a run is not a claim of cleanliness; the exit codes

*M3 integration, its corrections and the M2 merge (streams 11 and 21).* `derive_verdict` now
reads the error counters: one that was in the signature and has `unavailable_samples` or
`resets` above zero makes the run `incomplete`, which outranks corrected errors for a counter
that counted some and was blind for the rest. *Rejected:* any metric with unavailable samples
(a fan that dropped a reading would void a run that verified everything); also requiring the
capability report to list the counters as present. An *absent* counter still does not stop a
run being clean: the capability report says it was never read. Exit codes, one table after the
two integrations gave 4 and 5 different meanings: 0 clean; 3 a worker did not complete; 4 a
result file could not be written; **5 a fault** (a wrong result, or an error the hardware
reported, which `derive_verdict` calls `findings` and `corrected errors`); **6 a check was not
made**. Precedence 5, 4, 3, 6, 0: a fault is the finding and survives a failure to write about
it. Main already had 4 and 5 in its usage text and tests, and M2's numbers were unreleased, so
M2's meanings moved. A run that checked nothing is not a pass. The output directory is checked
before the run (a five-hour run that finds it missing at the end has lost its result), and files
are written whole or not at all.

### 2.45 One document tree rendered by each reporter; absence is `null` and its reason

*Wave 2 (stream 12, `docs/journal/12-results-and-reporters.md`).* `to_document()` builds one
tree and both writers serialise it, so JSON and CSV hold the same facts by construction and
"nothing summarised away" is checked once. Absence in JSON is `null` with `<member>_unavailable`
beside it (*rejected:* an object in place of the value, whose reader discriminates on a key; a
sentinel string); in CSV an empty `value` with `status`, `reason`, `source_path`, `detail` and
`remediation` in their own columns. A NaN or infinite real is the text `nan`/`inf`, because null
would say "not recorded" about a value that was. CSV is long form, one row per leaf with the
dotted path in `field` and the schema version first in every row. JSON text is validated UTF-8
with each bad byte one U+FFFD, nothing dropped. Availability carries a stable code (`absent`,
`vanished`, `denied`, `malformed`, `unreadable`) beside its phrase, because a reader should not
parse prose; `scope` still prints as prose (§6). Two fingerprints on one machine are equal in
every field but `temperature_at_start_c`, and a test over 30 runs asserts that every differing
field is such a moment field. The fixtures for a laptop (battery, cpufreq, thermal zone) are
built from the documented formats, not captured.

### 2.46 Telemetry sources: discovered by number, named only by the driver, absent told from unavailable

*Wave 2 (streams 09 and 13, `09-telemetry-sources-a.md`, `13-telemetry-sources-b.md`).*
`platform::FileSystem` cannot list a directory, and `std::filesystem` would bypass the seam and
leave discovery's P2 paths untestable, so hwmon chips, `tempN`, `fanN` and thermal zones are
found by number up to a bound (64, 255, 64), and EDAC controllers from `mc0` to the first without
a `ce_count`. A sensor is `thermal.package` or `thermal.core` only for `coretemp`'s exact
`Package id N` and `Core N` and `k10temp`'s `Tdie`; anything else is `thermal.sensor` with the
chip as provider. `Tctl` is not the package (an offset on some parts), and a rule for `acpitz`,
`nvme` or `iwlwifi` would be an unverified confident statement (F3). Zones stay beside hwmon and
are not merged (deduplicating compares live values, which is a guess). A fan at 0 rpm is a
reading. Frequency reports tick counters and not a gauge, which would need the previous reading
in a stateless source. Memory is seven fields from one read, so one round is one instant. The
`perf_event_open` errno table: `EACCES` and `EPERM` denied; `ENOENT`, `ENODEV`, `EOPNOTSUPP`,
`ENOSYS` and `EINVAL` absent; the rest unreadable, a failure of the attempt and not a fact about
the machine (`EINVAL` as absent is a judgement, since it is what the power PMU returned for bits
it refuses). The paranoid level chooses the remedy: above 2 lower it; at 2 or below the sysctl is
not what refuses, and advice to change it would be worse than none. Power tries powercap, then
the PMU's `energy-pkg`, with one remedy per actionable provider. The set of metrics is fixed at
the probe, and a metric denied at the probe is not re-tried at `sample()`. Capability absences are
grouped by name, state and remedy together ("x256" for 256 CPUs).

### 2.47 Tolerances are derived, and a sampled check says what it guarantees

*Waves 1 to 3 (streams 04, 19, 27; `04-bounded-verification.md`).* The dense-linear tolerance
bounds |kernel - oracle| when *both* are evaluations of the same sum: the oracle's value is not
exact, so Higham's γ_n S alone false-alarms at up to twice that, and a chosen multiple is a
constant verification.md §5 forbids. Eight `nextafter` round-up steps are a proof margin and a
test cannot tell 8 from 2 (the proof needs seven), so a surviving mutant there is expected and
the derivation is what discharges it. The underflow term is kept though correct inputs never
underflow, because the range check admits a subnormal. Sampling is one entry per tile, drawn per
unit from the index, every sampled entry compared and the worst named; the first version's claim
that a fault confined to a tile "meets a check" was wrong (external review W1-02): a fault
altering *k* of a tile's 1,024 entries is caught in one unit with probability k/1024, a
persistent fault is caught across units, and grid 256 checks every entry. `blocks_compared` (64
or 65,536) is what tells sampled from complete. Monte Carlo is summed by Neumaier compensation,
*not* a `long double` accumulator (it differs by architecture) or a pairwise tree (error grows
with log n); the oracle's sum equals Python's `math.fsum` bit for bit on six keys. A sampling
oracle's sample is arithmetic on a mixed index with a stride coprime to the modulus (no rejection
loop, no branch a particular index must reach); a sample shared between two units is a birthday
collision, not a defect. A plan is held to the specification even where the oracle does not use
it: a wrong decomposition is a wiring mistake the registry should see at once. Oracles allocate
nothing where they can avoid it, and take one scratch through a factory seam where they cannot
(`docs/adr/proposed/27-oracles-wave3-scratch-and-sampling.md`).

### 2.48 The crash journal and the timeline: what is stored, and what is not retried

*Wave 2 (streams 14 and 16, `14-telemetry-pipeline.md`, `16-journal.md`).* A journal record is
`u32 length | u32 CRC-32 over length and payload | payload`; a length of 0 or above 512 is
corrupt at once, so a flipped high bit is corruption and not a plausible torn tail. *Rejected:*
the reference's 8-byte length and CRC (wasteful); varints (a reader that cannot be checked by
counting). The directory is fsynced at open, since a created file's name is not durable until its
entry is. **fsync failure is never retried** (only `EINTR` is): a second fsync after `EIO` can
succeed for pages already dropped. A failed append does not poison the writer (replay says
`kCorrupt` with the entries before it, and the controller, which already gets `kStorage`,
decides); a short write continues and zero progress is an error. The timeline is one vector of
points per series, *not* a ring (`TimelineView::points()` is a `std::span`, so a series is
contiguous) and not one preallocated block (a five-hour run's worth, over a gigabyte, paid by a
one-minute run); resident is at most 2 × capacity × `sizeof(Point)` and transient growth at most
one more capacity, stated as `memory_bound_bytes()`. A probe the timeline refuses is remembered,
and its first outcome is final. A source's readings are used all or none: a count that differs
from its present metrics makes each of its series `kMalformed`, naming the source as the defect.
The order of refusal is unknown series, out of order, kind mismatch, capacity, last, because every
other refusal is the caller's mistake and should be reported even on a full timeline.

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

### 4.9 Two parsers that look interchangeable and are not

The first draft of the cache reader parsed `cache/indexN/level` with `parse_cache_size` —
it was to hand, it reads digits, and `level` is digits. It also accepts a `K` suffix, so a
file containing `1K` would have read as **level 1024**: a plausible wrong number in the one
place this module exists to prevent them.

Caught on re-reading the code before it was ever run, which is the cheapest place, and
recorded because the mistake is *structural*: a parser for a superset format silently
accepts things the narrower field never contains. The fix was the narrower reader
(`Source::integer`) with an explicit bound. **When two parsers accept overlapping inputs,
the one that accepts *less* is the one to reach for**, and the comment at the call site now
says why.

### 4.10 A mutant that does not compile has proved nothing

Nine hand-applied mutations of the cache cross-checks, eight reporting "tests failed: 1"
or more. The ninth reported `tests failed:` — **blank**. Read quickly, a blank sits in the
column where a small number sits and looks like a small number.

It was not a survivor and not a kill. Replacing `if (!online)` with `if (false)` left
`online` unused, `-Werror` refused the build, and the `&&`-chained pipeline never reached
the count. The probe harness had no arm for "the build failed", so it said nothing — which
is the fail-open shape F20 is about, in a tool that only ever runs by hand.

Re-probed with a mutant that compiles (`if (!online && cpus.cpus().empty())`) and killed
by exactly the test written for it. Two things carried forward:

- **A mutation is evidence only if the mutant ran.** Under `-Werror`, disabling a check can
  strand a variable, and a mutant that will not build says nothing about the tests.
- **An empty result is not a result** — the `$?` lesson of §4.1 in a different costume. The
  harness now prints "MUTANT DID NOT COMPILE" as its own outcome, so the column can never
  be blank.

### 4.11 A test that hoped for an ordering instead of enforcing one

`RealStopAndContinueAreDistinguishedFromTermination` forked a child that did
`raise(SIGSTOP); _exit(7)`, then in the parent: wait for the stop, send `SIGCONT`, wait
with `WCONTINUED`, wait for the exit. It passed for nine pull requests and thirty
consecutive isolated runs, then failed under the coverage build, and again under ASan.

The mechanism: **"continued" is a state on the task, not an event the kernel queues.** The
child's exit replaces it. After `SIGCONT` there was nothing stopping the child from running
straight to `_exit` before the parent's `waitpid(WCONTINUED)` was issued — and once it had,
that wait reported the *exit*, the final reap got `ECHILD`, and three assertions failed for
one cause. Instrumented builds shifted the timing just enough.

The §6 note written after the first failure guessed at gcov and `_exit` (§1.9). That was
wrong; the code-0 exit was the test's own `wait_for` helper returning a zeroed status after
`ECHILD`. **The second failure, under a different instrumentation, was what made the
mechanism visible — which is what "a second failure is real" was for.**

The fix is a pipe: the child blocks on a read after resuming, and the parent writes to it
only after it has *seen* `CONTINUED`. The ordering is now a fact of the protocol.

*The general rule, which the T7 supervision tier will live by:* **a test of a lifecycle
transition must enforce every ordering it depends on with a synchronisation the test
controls — a pipe, a barrier, a wait — never with the child's own speed.** A test that
passes because the child is usually slow enough is a test that fails when the build gets
slower, which is exactly when it is needed.


### 4.12 A diagnosis in a report taken as a fact

On 2026-10-20 a stream's report said its pull request (#57) failed because `main` had
added pure virtual methods to `platform::Syscalls`, so every fake no longer compiled. The
pull request itself had added them. The report was plausible and wrong, and acting on it
would have meant "fixing" `main`. Reading the diff of the branch against its merge base
took a minute and settled it.

*The structural fix* was the seam rule in §5.7, which removes the failure mode. *The
general rule* is the corollary of §5.5, applied to reports as well as to CI: before acting
on anyone's account of a failure, a stream's, a reviewer's or one's own, reproduce it on
the branch and on the base.

### 4.13 A review finding is a claim, not a verdict

Owner's rule, 2026-10-20. Independent reviewers make mistakes too, and a finding accepted
without being reproduced costs a change that may be wrong in a direction no gate checks.
Every finding is therefore reproduced or disproved before any fix starts, and the reaction
plan records the verdict for each: **CONFIRMED** (reproduced; the evidence is named),
**REJECTED** (disproved; the evidence is named) or **PARTLY** (which part holds). The
earlier reaction plans (`reviews/reaction-plan-*`) predate the rule and are being given
their evidence after the fact.

### 4.14 A failed gate pushed because only its last lines were read

On 2026-10-20 a local gate run was judged green from the tail of its output, and the push
went out with a failing step earlier in the log. CI caught it. This is §4.1's mistake in a
new shape: the evidence was a summary, not the status. **The verdict of a gate is its exit
code, read directly, every time.**

### 4.15 Parallel worktrees filled the disk

At about 19:05 UTC on 2026-10-20 roughly thirty worktrees, each with one or more build
directories of 1 to 2 GB, exhausted the container's disk allowance. Builds and pushes in
every running stream failed at once with `No space left on device`, each looking like that
stream's own defect. Deleting the build directories of inactive worktrees freed 22 GB and
the streams re-ran their failed steps. *The fix is a habit:* a stream deletes its build
directories when it reports, and the manager removes the worktree of every merged branch at
each check-in.


### 4.16 A green check run on a base that no longer exists

On 2026-10-21 #87 showed every check green, but its run had merged it with `main` as it was
before #84, which split the contract headers that two of its tests included. Merged then, it
would have broken `main` with all checks green. A check run proves the merge of the head with
the base *at the time of the run*. Before merging, either the head contains the latest change
that could interact with it (a contract change above all), or the files it touches and the
files `main` changed since the run are disjoint (`comm -12` of the two name lists). The EXT
stack (#79 to #81) was merged on the second test; #87 was rebuilt on the first.

### 4.17 A trap recorded in every brief recurred anyway

Signed `__int128` multiplication under UBSan does not link on ARM64 (§1.30). It was in the
stream rules every worker received, and the stencil stream's test helper still multiplied
signed `__int128` (#70). The ARM64 ASan job caught it, which is what the gate is for; the
lesson is that a sentence in a brief is not a check. The evidence that the fix was right was
a cross-compile, since x86 provides `__muloti4`: `clang-18 --target=aarch64-linux-gnu
-fsanitize=signed-integer-overflow -c` of the signed form references it once and the
unsigned form not at all.

### 4.18 The same contract trap found independently by every stream in a wave

*Waves 1 to 4, from the fragments' "Contract questions".* Streams may not read each other's
work (ADR 0003), so a trap in a frozen header is met once per stream, each paying a build or a
CI run. Counted from the fragments: a header registering two typed suites, so that a binary
instantiating one fails GoogleTest's uninstantiated-suite check (streams 01, 02, 03, 04, 05,
07, 08, 09, 12, 13, 14 and 27); `for_each_index` tripping `bugprone-easily-swappable-parameters`
in the frozen executor header (05, 06, 20, 23, 31, among others); gcov counters going negative under threads
(04, 05, 06, 08, 20, 27); `INSTANTIATE_TYPED_TEST_SUITE_P` refusing a qualified name or a
comma (00, 04, 07, 14, 18, 26, 27). Wave 1 found each, the wave-1 contract corrections fixed
them (#84, #67), and wave 3's streams still met some of them
(stream 27 notes "as wave 2 found"). §4.17 is the same lesson from the manager's side: a
sentence in a brief is not a check. The structural fix is that a contract question which
costs every consumer a build is fixed by the manager before the next wave starts, not
folded at the wave's end, and that a stream's fragment is read by the manager while the wave
runs (§5.6). The things that cost nothing to say in a brief (where `INSTANTIATE` goes, a named
function for `Executor::run`) were not the problem; the ones nobody knew to say were.

### 4.19 A number stated in a header, a comment or a test name is a claim, and was wrong when measured

*Waves 1 to 4 (streams 04, 09, 22, 24, 26, 27, 31, 32).* Each was found by running something, not by
reading: the frozen dense-linear header said entries have "magnitude below 1" when
`entry_value(0)` is exactly -1.0; the adaptive-mesh header said a cell has "at most about 50"
leaves and 88 occurred in 200 units, and its "6913 leaves in all" is looser than the 4096 that
holds; the particles spec's own test is named "...DefaultIsOne" and asserts 2; a comment said a
temperature range caught a chip's `-128000` and a test showed it did not (§1.46); a claim that a
fault confined to a tile "meets a check" was true of 1 entry in 1,024 (§2.47); the FFT's
(1 ± 2^-20) boundary test cannot be met for Parseval (§1.48); a verification.md table says sparse
has a preconditioner that the specification rules out. A figure in a document is a
hypothesis until something has produced it. The rule: when a header states a measured-looking
number ("about", "at most"), record the measurement beside it with its command, or say it is
an estimate.

### 4.20 When a test disagreed with the code, the independent reference was wrong first

*Waves 3 and 4 (streams 22, 27, 31).* Three times the reference written by the test author was
the thing at fault, and the disagreement was the test doing its job: the FFT's fixture generator
lost the sign of every negative value (`mpf.man_exp` returns a magnitude); a `long double`
`cosl(2π m / n)` failed 23 of 8,192 entries near the function's zeros until the angle was
reduced by exact integer arithmetic first; a reference computing a hotspot's centre in
`uint32_t` wrapped at 0xFFFFFFFF where `spec.hpp`'s `uint64_t` does not. In each the code
followed the specification. What caught them: an input of all ones, and every entry of a table rather
than a sample. A disagreement is
a question about both sides; start with the one written most recently by hand.

### 4.21 Guessing what the instrumented builds cost

*Waves 1 to 3 (streams 06, 08, 11, 27).* The cost of a test in the debug, coverage or sanitizer
build was guessed and the guess was wrong, in both directions: the executor soak was cut to a
tenth under the sanitizers on the guess that they would be much slower, and measured 7.7 s, 9 s
and 8 s for the same hundred thousand runs (§1.50); one dense-linear test took 14 s because an
unoptimised build leaves the upper ymm halves dirty (§1.35); one graph check took 1.5 s under
the coverage preset against 0.1 s plain (atomic counters), and a four-iteration byte loop at
4.7 million uses ran the oracle tier past eleven CPU-minutes; the CLI over the shared fake
spent 25 s in an hwmon scan the real `/sys` does in milliseconds. Measure each tier's time the
first time it runs, in the builds that matter, and put the figure in the fragment so that the
next stream plans from it.

### 4.22 A brief that contradicts the repository's documents

*Waves 1 to 4 (streams 01, 02, 04, 10, 18, 19, 20, 21, 24).* Briefs named things the tree did
not have or disagreed with its documents: `tools/install-git-hooks.sh` did not exist (streams
18, 20, 24); the brief said to open a pull request where the stream's instructions said the
manager does (01, 02, 04); a brief asked for "a different cache domain" where verification.md
§7 needs the nearest core (§2.40); M3's exit criterion 6 says checks happen outside benchmark
windows where `runner.hpp` states the lower-duty-cycle fallback; verification.md §3 says sparse
has a preconditioner. The rule that worked, in every case: the repository's documents win, the
disagreement is written in the fragment (stream 10 did it in full), and the manager
corrects the brief or the document afterwards. A brief is generated from the documents at one
moment and goes stale; the stream's question to ask is which of the two the tree agrees with.

### 4.23 Twice a "timing flake" was a defect, found only once the failure could be read

*M2 corrections and M5 integration (#95, #96), 2026-10-21 and 2026-10-22.* Both failures passed
alone and failed under load, and both were first read as a margin too small:

- `cli_stress_test_dense_linear` failed in CI. The first diagnosis, a run window too short
  for the coverage build, was wrong, and widening the window did not hold. Once CI printed
  the failed test's output (the test steps now rerun failed tests with
  `--output-on-failure`, #95), the cause was plain: a worker sees a stop only between units,
  a dense-linear unit takes about 1.5 s in that build, and the stop grace was 1 s, so a
  healthy worker was killed. Verified runs now get a 10 s stop grace (`cli.cpp`).
- `ControlOverRealWorkers.PauseHolds…` failed once in a local full-suite run. With six CPU
  hogs beside it, it failed 2 in 12: the test assumed the controller always sends its
  deadline Stop, and `request_stop` sends nothing to a worker that left on its own duration
  and was already reaped. The test now admits either outcome.

What worked: get the failing output first (CI's rerun step, or a local run with
`--output-on-failure`), then reproduce under load (`timeout 300 sh -c 'while :; do :; done'`
six times), and read which assertion fails before changing anything. A wider margin is a
fix only when the output shows that the margin is the cause.

---

## 5. Process knowledge

### 5.1 Everything through pull requests, and why it is worth the friction

Every change goes through a pull request so that CI runs on it before it reaches `main`.
The friction is real — a fix that would be one commit becomes a branch, a push and a wait
— and it has already paid for itself three times: the ARM64 loader difference (§1.2), the
merge-commit sign-off failure (§1.5) and the arm64 kernel publishing no cache `id` (§1.14)
were all found by CI on a pull request, and none was findable locally. The third is the
sharpest: every local check was green, every x86 job was green, and the module would have
refused every arm64 machine it ever ran on.

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


### 5.6 What the first day of parallel streams showed

ADR 0004's waves ran for the first time on 2026-10-20. Between 12:16 and 22:19 UTC
thirty-three pull requests merged: all of wave 1 (streams 01 to 10), all of wave 2, the
mutation fixes and both wave-4 workloads, while wave 3 and the first integration streams
were in flight. What decided the pace was never the writing of a component:

- **A change to a shared gate is a wave event.** When #36 extended clang-tidy to `tests/`,
  every open wave-1 pull request turned red at once, each needing its own fix. Such a change
  should land before a wave starts, or be announced to every stream with the fix pattern.
- **A change to a shared contract breaks every consumer's mock.** See §4.12 and §5.7.
- **Merging is the bottleneck.** Pull requests that were green waited behind CI queues, and
  each merge to `main` made the next one stale. Merging main into a branch and re-running
  CI is the steady-state cost of parallel work, and per-branch CI concurrency with
  cancel-in-progress (#78) stops superseded runs from holding the queue.
- **Reports need reading, not forwarding.** A stream's report is a claim about its own
  work (§4.12, §4.13); its "Contract questions" are where the useful information is, and
  they are only useful if the manager folds them into a contract change.
- **Every stream pushing at each meaningful step paid for itself.** All work stopped for
  2.5 hours (13:05 to 15:40 UTC) on an external usage quota, and again briefly when the disk
  filled (§4.15); because each branch was pushed, nothing was lost and each stream resumed
  from its last commit when told to continue.

### 5.7 A method added to the syscall seam gets a default that fails with `ENOSYS`

`platform::Syscalls` is the one interface every component's fakes derive from: a known,
accepted exception to ADR 0003's "share only contracts". Adding a pure virtual method to it
breaks the compilation of every fake in the tree, in every open branch. So a new method is
added **non-pure, with a default that fails with `ENOSYS`**, defined in
`src/platform/syscalls.cpp` and exercised by `tests/unit/platform/syscalls_defaults_test.cpp`.
A fake that needs the call overrides it; a fake that does not is untouched, and a component
that reaches a call its fake does not model gets a clear refusal instead of a silent
success. (Introduced with #57. `Syscalls::has_exited` predates the rule and still has stubs
in two test fakes; giving it a default is an open follow-up.)

### 5.8 Numbered sections in a shared file collide across parallel pull requests

§2.31 was claimed twice on 2026-10-20, by the wave-4 specifications and by the EXT-02
fix, on branches that could not see each other. The collision shows up only as a merge
conflict, and the loser's internal references must then be renumbered by hand. This is why
streams write to their own fragments (§7). For pull requests outside the streams that must
add to this file, the rule is: take the next free number at the time of the merge, not at
the time of writing, and check cross-references when renumbering.

### 5.9 A stacked pull request must be retargeted by hand

A branch cut from an unmerged branch (§5.2) opens its pull request against that branch.
GitHub retargets such a pull request to `main` automatically only when its base branch is
deleted on merge, and the credential this work runs under cannot delete refs (§6). So after
the parent merges, the child is retargeted to `main` explicitly and `main` is merged into it
before its CI is believed. EXT-01, EXT-02 and EXT-03 (#79, #80, #81) are such a stack.


### 5.10 A container restart loses whatever a worker had not committed

At about 04:30 UTC on 2026-10-21 the container restarted. Three workers had merged `main` into
their branches and made their fixes, uncommitted; integration stream 11 was in its final
coverage run. The working trees survived, the processes did not, and every result that only
lived in a worker's head was gone: the work had to be read back from `git status` and
finished by new workers. Briefs written since then say to commit locally after each step that
changes files, and the manager's own long checks write their exit codes to files before
anything acts on them.

### 5.11 The runner queue, not the work, set the pace

Each pull request starts 24 jobs; with ten or more open, jobs waited over an hour for a
runner. Three changes cut the queue: clang-tidy in four shards with a process per core
(60 to 100 minutes down to 10 to 17, #85), a new push cancelling its branch's superseded run
(#78), and documentation-only pull requests skipping the build and test workflows (#89). The
credential this work runs under cannot cancel or re-run a job (403), so a push is the only
way to restart one, and an unneeded push costs a full run.


### 5.12 Two integrations of one controller: whichever lands second adapts to the first

The M2 and M3 integrations (#90, #87) each extended the same controller and command line, in
different directions, on branches that could not see each other. Each was right alone. Merged,
they conflicted in eleven files and, beyond the text, in meaning: both used exit codes 4 and 5
for different things, and both probed the telemetry sources. Then M3's contract corrections
(#92) removed the function M2's banner called, and a merge with no textual conflict did not
compile. The order did not change the work, only who did it: the second to land is adapted to
the first, with the full gate set, and the decisions recorded where both can be read (the
journal fragment of stream 11). What made it tractable was the rule that the second branch
re-earns its green on the merged tree (§4.16), never inherits it.

### 5.13 The runners' package mirror can fail a job before any of this project's code runs

On 2026-10-22 at about 11:00 UTC, the coverage job on #95 failed in `apt-get update`:
`packages.microsoft.com` answered 403 and apt treated the source as unsigned. The source
comes with the runner image and nothing here installs from it. Since a job cannot be re-run
from here (§5.11), the workflows now delete `/etc/apt/sources.list.d/microsoft-prod.*` before
every `apt-get update` (#95). An outside service that can fail a job is either removed from
the job's path or made unable to fail it; waiting for it to recover costs a push either way.

---

## 6. Open threads

Kept short and current; move an item to the relevant document once it is settled.

- **Awaiting the owner:** branch protection on `main` (required status checks, no required
  approvals — a solo maintainer would otherwise block themselves), and deletion of the
  merged branches. The credential this work runs under gets **403 on deleting a ref and on
  workflow dispatch**. The mutation-tool pin no longer waits: a push to a dedicated
  discovery branch ran the bootstrap job on GitHub's runners without a dispatch, and the
  pin is in `mutation.yml`.
- **Answered (#22):** whether CI should hard-require a specific committer identity. It
  admits identities from an allowlist, `.github/commit-identities`, which holds the owner
  today; a contributor's first PR adds their own line. That keeps the repository open while
  making a new identity a reviewed change, and it is the check `--no-verify` cannot skip.
- **Landed since this file was written:** the syscall seam, the runtime-dependency gate,
  `ExitStatus`, the coverage-completeness gate, the worker launcher and the pool that
  supervises a set of them (§2.1), the clock (§1.11, §1.12), the sysfs CPU-list parser, and
  the capability-classifying `Source` that decides what a *failed* reading means (§2.6,
  §2.7). Review was waived by the owner rather than performed, which is worth remembering
  when reading that history: the gates are the only thing that has inspected it.
- **Topology discovery is complete:** CPUs (§2.8, §2.9), caches (§1.14, §2.10) and NUMA
  (§1.15, §2.11), each with its cross-checks and its recorded list of checks deliberately
  *not* made. The module ledger's `topology/` row is satisfied.
- **The platform primitives M1 needs are complete:** `platform/memory` (§1.17, §2.13) joins
  affinity, the clock and process supervision. One prediction in the previous version of
  this entry was wrong and is worth keeping: `ENOMEM` from `mlock` under `RLIMIT_MEMLOCK` was
  called "the one CI can actually provoke" — it cannot, because CI is root and root ignores
  the limit. It is provoked from a child that drops privileges instead.
- **`config` is in (§1.18, §2.14):** parse, schema validation with every refusal named and
  line-numbered, round trip, and the memory-share arithmetic. `loadforge --check-config FILE`
  is the first user-facing command past `--version`. Not yet in: the *reader* of the memory
  budget (`MemAvailable` plus the cgroup limit under either hierarchy) — it lands with the
  controller, which is the first thing that needs the number — and config fields for the
  memory module's huge-page, lock, populate and NUMA choices, which wait for the workload
  that first sets them.
- **The controller/worker split is under way, in increments:** the heartbeat region
  (`worker/ipc`, §1.19, §2.15), the record channel (§1.20, §2.16) and the worker process
  itself (§1.21, §2.17: the internal `--worker` switch, the null workload loop, four exit
  codes) are in, with the seam calls each needed (memfd, shared map, close-on-exec
  handover; pipe, write, poll; ignore-signal, getpid; non-blocking reap). **The controller
  landed in #23** (§1.22, §2.18): `loadforge
  stress --config FILE` runs the null workload in N worker processes for the configured
  duration — M1 exit criterion 1 — and the supervision tier (`tests/supervision/`, T7)
  runs the shipped binary as the worker: two complete, one is killed from outside while
  the other completes, and the controller itself is killed and every orphan is found dead
  by the parent-death signal. The review of 2026-10-19 then found that it did not ignore
  `SIGPIPE` (R-02): on `main`, the real-binary stress test died by signal 13 in 2 runs of
  12, the shared fake having modelled the write as a plain `EPIPE` (ADR 0003). The
  fake-worker executable followed (§1.23, §2.19–§2.22): every reachable row of the T7
  misbehaviour catalogue, workers leading their own sessions so a dead worker's grandchildren
  are killed with it, and the two new gates (no test-hook symbol in the shipped binary; an
  `EOWNERDEAD` robustness test per process-shared lock, of which there are none). Under
  load it also found that both sides called one torn heartbeat read a death (§2.22). The
  memory-budget reader and OOM classification from the kernel's own record followed
  (§1.24, §2.23): `src/resources`, the cgroup's OOM-kill count consulted before the first
  fork and after the last reap, `kOomKilled` only when the count accounts for every
  SIGKILL the controller did not send. The M1 exit review followed
  (`reviews/m1-exit-review-2026-10-20-0444-UTC.md`): the OOM row end to end (§1.25, §2.24),
  the topology snapshot before the fork (§2.25), the stop-byte row without its race (§2.26),
  and `EPIPE` on a record pipe as the controller lost (§2.27). Every M1 exit criterion is
  met; the milestone waits only on the first mutation sweep over `src/`.
- **The first mutation sweep (§1.26, §2.28):** the run over the whole project as one job
  was cancelled at six hours with no report. The sweep now runs one job per module against
  the tests that create no process. Expected from its first run: five survivors in
  `src/platform/syscalls.cpp`, in `fork_process` and `reap_any_nonblocking`, which only a
  forking test reaches; they are not equivalent mutants, and the remedy is to move the
  decision on the returned pid above the seam, in its own change.
- **A known gap, from §2.20:** when the controller itself is `SIGKILL`ed, a grandchild of a
  worker survives: the death signal is cleared across fork and nobody is left to sweep the
  group. The real worker forks nothing, so nothing is exposed today; the first workload that
  forks must close it (a worker that kills its own group on the way out, or the grandchild
  arming its own death signal), and its T7 test is the deaf-worker test with a grandchild.
- **Deferred, from §2.23:** exact OOM attribution needs a cgroup per worker; the count of a
  shared cgroup attributes all or none. And `/proc/meminfo`'s and `nodeN/meminfo`'s
  `Key: value kB` parsing stays in two places, deliberately, until a third caller shows what
  is really common.
- **Resolved — a flake, with its fix:**
  `RealWaitStatus.RealStopAndContinueAreDistinguishedFromTermination` failed once under
  the coverage build and once under ASan, in different sessions, while passing 30/30 in
  isolation. §4.11 has the mechanism; the test now enforces the ordering it used to assume.
- **Wanted: one local entry point that runs what CI runs.** There is currently none, so
  every contributor — and every session — assembles the list from memory and gets a
  different subset. That is how §4.8 happened: eleven checks run, the twelfth not recalled
  because it belonged to the other language in the tree. A `tools/verify.sh` that runs
  exactly what the workflows run, and is itself checked against the workflow files so the
  two cannot drift, would close it. Deliberately not bundled into the change that
  motivated it.
- **Contradictions between the documents and the code, found by reading both and not yet
  resolved** — each is a decision or a one-line fix, recorded here rather than silently
  made:
  - *Resolved 2026-10-20:* PLAN §4.6's promised fallback for the worker executable without
    `/proc` is withdrawn; the refusal stands and an explicit `--worker-executable` override
    is the remedy, when someone first needs it (PLAN §4.6 records the reasoning).
  - *Resolved 2026-10-20:* the comments in `platform/exit_status.hpp` (OOM corroboration is
    M1 exit criterion 3, not M5), `platform/fs.cpp` (no `SIGCHLD` handler exists; other
    signals reach the bounded retry) and `platform/process.hpp` (an exec failure leaves only
    the status word; the record pipe does not exist before exec at any milestone) now say
    what the code does.
  - IMPLEMENTATION §3 listed `worker/ipc` as owing T5 before `tests/fault_injection` exists;
    the ledger row and the table now say T1 and why. The tier question returns at M2a.
- **Deferred, and worth not forgetting:** `Source` has no "counter wraps" state, which the
  P7 taxonomy lists. It is genuinely not needed yet — nothing here reads a monotonic
  counter — but the telemetry sources at M3 will, and the decision about where wrap
  detection lives (in `Source`, or in a counter type above it) should be made deliberately
  rather than by whoever first hits it.
- **Answered by ADR 0004:** wrap detection lives above the source. A source reports a
  counter's raw value with its modulus (`telemetry/sample.hpp`), `counter_delta()` defines
  the advance across one wrap, and the Load Signature never adds an advance across a gap in
  the readings, where the counter may have reset (`statistics/signature.hpp`).

- **State on 2026-10-20 at 22:22 UTC.** Waves 1, 2 and 4 are merged. Waves 3 (#69 to #74,
  waiting on the wave-3 specifications in #65) and the primitives (#63) are open. The
  external review's EXT-01 to EXT-03 are a stack of pull requests (#79 to #81, §5.9).
  Integration streams 11 (M2) and 21 (M3) are running; 29, 33 and 34 follow. Open
  follow-ups: the seam default for `has_exited` (§5.7); removing the per-target
  `-fprofile-update=atomic` copies (§1.31); the contract corrections collected from the
  wave-1 fragments (`chore/contract-corrections-wave1`), whose release build still has
  `-Werror=unused-result` failures to be checked against `main`; vector kernels for FFT and
  sparse; evidence for the earlier reaction plans (§4.13). The third outside review request
  is `reviews/review-request-2026-10-20-1841-UTC.md` (#68). Everything since supervision
  still has had no independent reviewer.

- **Contract questions from the stream fragments, folded 2026-10-21 18:20 UTC.** The fragments in
  `docs/journal/` keep the questions as the streams asked them; this is where each stands.
  *Resolved by a contract change:*
  - Wave 1's questions (one suite per header, the `for_each_index` NOLINT, the `oracle.hpp`
    visits, the dense-linear and compression specification wording, isolation's four readings)
    were decided in #84, and the shared coverage flag in #67:
    `docs/journal/wave-1-integration-2026-10-20-1840-UTC.md`.
  - The M3 integration's seven disagreements, with stream 13's question about the energy unit,
    were decided in #92: `docs/journal/m3-contract-corrections-2026-10-21-1145-UTC.md` (§2.42 to §2.44).
  - The M2 integration's five disagreements (stream 11: `select_path` against a workload's paths,
    the runner's lock policy, the decomposition missing from `Provenance`, one CPU for a
    multi-thread unit, `physical_core` as one number) are in progress. There is no decision
    file yet; the same change takes the stream-02 runner items the owner deferred.
  *Open:*
  - **Scratch.** One piece per unit against one per slot, and whether the caller's writes before
    `run()` are visible to the tasks (§2.38; streams 17, 20, 22 to 26, 31).
  - **Vector kernels.** Whether a workload stream may own files under `src/primitives/simd/`
    (§2.36; streams 22, 23, 24).
  - **Parseval.** The specification's (1 ± 2^-20) test cannot be met for that check (§1.48;
    streams 22, 27).
  - **Declared classes and versions.** Ray tracing (§2.31, stream 30) and the stencil (stream 23)
    give more than their class names; `Descriptor::version` against the specification's
    `kVersion` (streams 05, 25).
  - **Deferred by the owner, still open:** a `kMalformed` decode fault and a rule for over-long
    text (stream 01); `runs_here()` on a subject (streams 06, 18, 26); the floating-point
    re-assert policy (stream 07).
  - **`RunResult` gaps** (streams 12, 21): per-phase counter results are on the console and in no
    file; a worker's death leaves no telemetry field; `Observers` cannot say sampling stopped;
    the fingerprint carries no reason for an absent field and `topology::Unavailable` has no
    remediation; `scope` is printed as prose; nothing turns the two tick counters into a
    utilisation.
  - **Specification figures to correct:** adaptive mesh's "about 50 leaves in a cell" (88
    measured) and "6913 leaves in all" (4096 holds); the particles specification test named
    "...DefaultIsOne" that asserts 2; verification.md §3's preconditioner for sparse; the raytrace
    specification names neither which pixel of a tile is sampled nor whether the tile-sum slots
    past `tile_count` are checked (stream 32). Sparse's derivation 2 is no longer an assumption
    (§1.47).
  - **Captures wanted (F21):** hwmon, thermal, fan and cpufreq trees, EDAC, powercap and a laptop's
    fingerprint files; hybrid and big.LITTLE topologies; a NEON golden row for the particles.
    The VM offers none (§1.42), so T11 stays open.
  - **GCC's ThreadSanitizer preset** fails to build `src/worker/ipc/heartbeat.cpp`
    (`atomic_thread_fence`, streams 16 and 20); Clang's runtime does install and run
    (stream 21).

---

## 7. Stream journals

From M2a on, work runs as parallel streams (ADR 0004), and ten streams appending to this
file in one wave would conflict on every merge. So each stream keeps its own fragment,
`docs/journal/<NN>-<slug>.md`, with the same three kinds of entry as this file —
**Established by experiment** (a fact, and how it was established), **Decisions** (the
choice, the alternatives, the reason), and **Contract questions** (an ambiguity met in a
frozen contract, the reading taken, and the test that pins it). At the end of a wave the
manager folds each fragment into the sections above, keeps the file as a record of which
stream learned what, and resolves the contract questions in a contract change.

The first fragment is `docs/journal/00-contracts-and-streams.md`, from the change that
wrote the contracts.

**Folded 2026-10-21 18:20 UTC.** The fragments of waves 1 to 4 (00 to 10, 12 to 20, 22 to 28 and
30 to 32), the two integrations (11 and 21) and the two decision files beside them were folded
into the sections above: §1.33 to §1.50, §2.34 to §2.48, §4.18 to §4.22, and the state of every
contract question in §6. Streams 29, 33 and 34 have no fragment yet. Each file is kept unchanged
as the record of which stream learned what. The next fold is at the end of the next wave; the
numbering continues from §1.50, §2.48 and §4.22.
