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
  SIGKILL the controller did not send. Still to come: the T7 row that OOM-kills a real fake
  worker and asserts `kOomKilled` end to end, now that both halves exist. The M1 exit
  criteria in IMPLEMENTATION.md §4 are the checklist.
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
