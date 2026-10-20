# Journal fragment 08 — the executor

**Written:** 2026-10-20 11:05 UTC
**Change:** `src/worker/executor/`, the thread pool behind `worker/executor.hpp`
(stream 08, wave 1).

Measured on an x86-64 container: Linux 6.18.44, glibc 2.39, GCC 13.3, four vCPUs of an
Intel Xeon at 2.10 GHz, running as root. The ARM64 half of each claim below is CI's.

## Established by experiment

### A thread started after the registers change inherits them, rounding and flags alike

`fesetround(FE_UPWARD)` with MXCSR.FTZ and MXCSR.DAZ set on the creating thread, then a
pool of four threads: every task, on every pool thread, read rounding upward, FTZ on and
DAZ on, through `<cfenv>` and through the registers directly. In the required state, every
task read the required state. This is the property `fpenv.hpp` says the provider must
confirm against the real platform, and the pool's start-up check rests on it: a thread is
expected to hold exactly its creator's state, so a difference means something changed it.
The test is `tests/integration/worker/executor/real_pool_test.cpp`, and on AArch64 it reads
FPCR.FZ and FPCR.DN instead; CI's ARM64 runner is where that half is established.

### `sched_setaffinity(0, …)` from a pool thread moves that thread alone

Each of four threads pinned itself with pid 0; each then read its own mask back
(`sched_getaffinity(0)`) as exactly its CPU, and `sched_getcpu()` agreed on every one of
160 tasks, with no thread seen on two CPUs. The calling thread's mask was unchanged
afterwards. Linux reads pid 0 as the calling *thread*, not the process — which is what lets
each thread place itself and no thread place another.

### The kernel refuses a thread's placement exactly as the shared fake models it

A pool whose second thread is placed on CPU 1023 (absent from this machine), and one whose
second thread is placed on no CPU at all, were each refused with `EINVAL` at thread 1,
classified `kNoUsableCpu`, with both started threads joined — the same report the unit tier
gets from `FakeSyscalls` for a cpuset that withholds the CPU. The model is checked against
the kernel in a test that can fail.

### The hand-off costs about 75 µs a run here, nearly all of it in the kernel

Two million runs of one to thirteen trivial tasks on a pool of four: 151.5 s wall, 21 s
user, 162 s system — the futex calls of the condition variables, four threads woken per
run. No index was lost or repeated, the process had five threads before and after, and
the counting allocator saw no allocation in two million runs. A hundred thousand runs take
7.7 s. That is the cost of the simplest protocol (Decisions, below); it matters only to a
workload whose units are a few microseconds, and none of the specified ones is.

### The ordering tests fail when the protocol is broken, and pass when it is merely changed

Each mutation was made to `thread_pool.cpp`, the ordering tests run, and the file restored:

| Mutation | Debug build | TSan |
|---|---|---|
| `run` returns without waiting for every thread to finish | 4 of 5 ordering tests fail | — |
| a thread counts itself finished before running its tasks | 4 of 5 fail | 4 of 5 fail, 4 data races reported |
| the slot passed is `index % threads()`, not the thread's | the contract's `NoTwoConcurrentCallsShareASlot` fails for 4 and 7 threads, and `ASlotIsAlwaysTheSameThreadAndNeverTheCaller` | — |
| a thread reads the task, context and count after unlocking | all pass | all pass, no race |
| the claim counter's `fetch_add` made `relaxed` | all 50 pass | — |

The last two are not bugs, and the tests agreeing is the argument confirmed rather than a gap:
the published values cannot change until every thread has counted itself finished (claim 4),
so reading them unlocked is still ordered; and claim 2 holds whatever the counter's memory
order, because uniqueness comes from the read-modify-write, and visibility from the mutex.
The counter stays `seq_cst` because PLAN §4.5 makes that the default and nothing measured
asks for less.

### Concurrent code needs atomic coverage counters, or gcovr refuses the report

The first coverage run failed in gcovr: `src/platform/syscalls.cpp:194 Got negative hit value
… branch 1 taken -13473` (GCC bug 68080). gcov's counters are plain increments; four pool
threads calling `RealSyscalls::get_affinity` at once lost updates, and gcov's derived arc
counts went negative. The same can happen to any code the pool's threads run concurrently —
the pool's own claim loop, and header code such as `for_each_index`'s lambda, counted in
whichever test binary compiled it. The executor's library and its three test binaries are
compiled with `-fprofile-update=atomic` under the coverage preset, from their own build
fragments, and the integration test asks the kernel for a task's affinity directly rather
than through the platform library, whose counters are not atomic. Proposed for
`cmake/Coverage.cmake`, which is not this stream's: every later stream with threads meets
the same failure, and atomic counters cost only the coverage build's speed.

### ThreadSanitizer's runtime starts a thread of its own, lazily

The soak's first version asserted that a pool of four made the process five threads. Under
TSan it had six, and still six when measured before the pool was made — the runtime's
background thread starts once the process has made a thread. The exact count of what the
pool added is asserted only outside the sanitizers; that the count does not change across a
million runs is asserted everywhere.

### Two details of GoogleTest and of the ledger gate

- `contracts/fpenv/fpenv_contract.hpp` declares `FpCheckContract` beside
  `FpControlContract`. A consumer of only the second that includes the header fails
  GoogleTest's own check, `UninstantiatedTypeParameterizedTestSuite<FpCheckContract>`,
  until it says `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(FpCheckContract)` in the
  suite's namespace.
- The ledger gate finds an instantiation by the regular expression
  `INSTANTIATE_TYPED_TEST_SUITE_P(\w+, (\w+),`: a suite named with its namespace
  (`::loadforge::contracts::ExecutorContract`) is not found. The instantiations are written
  inside `namespace loadforge::contracts`, where the suite's name is unqualified, as the
  contracts' reference tests do.

## Decisions

### Threads are started through a seam of the pool's own, over pthreads

`platform::Syscalls` has no thread calls, and is not this stream's to grow. The pool has
`Threads`: `pthread_create(3)` and `pthread_join(3)` and nothing else, the real one a
translation with no branch. *Rejected:* `std::thread`, which reports a failed start by
throwing `std::system_error` — the catch arm would sit below the seam, and no test on a
root CI runner can make a real start fail (`RLIMIT_NPROC` does not bind root; a cgroup's
`pids.max` would, at the cost of moving the test into a cgroup for one errno). With
`pthread_create` the error is a return value, and `make_thread_pool` decides what it means
where `ScriptedThreads` can force `EAGAIN`, `EINVAL` and `EPERM`. `pthread_join`'s result is
dropped by contract, as `start_session`'s is: it fails only for a handle never started,
joined twice, or the thread itself, and the pool joins each thread it started once, from
the thread that started it.

### Threads start one at a time, each placing and checking itself

Affinity with pid 0 and the floating-point registers both belong to the calling thread, so
each thread places itself and reads its own registers, then reports under the mutex; the
factory holds the mutex across the start, so the report cannot come before the wait for it,
and starts the next thread only after it. That makes the seams single-threaded — the shared
`FakeSyscalls` is not thread-safe, and need not be — and makes the unwind simple: on any
refusal, the pool's destructor stops and joins exactly the threads started. *Rejected:*
starting all and collecting reports, which saves a few hundred microseconds once per worker
and makes every seam concurrent.

### One thread per placement; the caller runs nothing

A pool of *n* placements has *n* threads; slot *s* is thread *s*, placed by placement *s*,
for its whole life. The calling thread waits and never runs a task. A load test pins work
to CPUs, and a placement means something only if the thread that was placed is the one that
does the work; a caller that joined in would run tasks on whichever CPU it happened to be
on. `one_thread_per_cpu(mask)` is the placement for a worker given a set of CPUs: one
thread per CPU, each pinned to that CPU alone. A placement may also be absent — a thread
left on what its creator may run on — for a worker that was not placed.

### Indices are claimed one at a time from one seq_cst counter, and every thread wakes for every run

The protocol is the simplest that meets the contract: everything a run publishes is
written and read under one mutex; indices are claimed with `fetch_add` on one atomic, every
operation `seq_cst`; every thread wakes for every run and reports when its claims run out;
the caller returns when all have. Nothing is weaker than `seq_cst`, so PLAN §4.5's ordering
test for weaker orders is not owed — the protocol's own argument and tests are in
`thread_pool.cpp` and `ordering_test.cpp`. *Rejected:*

- **A static partition** (slot *s* takes *s*, *s + n*, …) — no shared counter at all, but a
  thread descheduled or a unit dearer than its neighbours holds up the run, and a load tool
  should keep every CPU busy.
- **A claim under the mutex** — no atomic at all, and a lock per index.
- **Waking only as many threads as there are indices, or spinning before sleeping** — both
  cut the 75 µs, both make which arm of a condition a test takes depend on the scheduler,
  and the second is lock-free cleverness PLAN §4.5 asks to be shown necessary first.

The counter ends at most `count + threads()` and would wrap only for a count within
`threads()` of `SIZE_MAX`, which is more tasks than any run could execute in a lifetime;
no code guards it, since a guard would be a branch no test could take.

### Every wait's first test is fixed by the protocol, never by the scheduler

Each condition is first tested by a thread that has held the mutex since the condition last
changed: the factory waits for a report it started the thread for while holding the lock; a
new thread reports and goes straight into its wait without releasing it; the caller waits
for completion in the same critical section that published the run. So the "must wait" arm
of every condition is taken deterministically, and coverage of the pool is not a matter of
timing (journal §4.11).

### A thread checks its registers against its creator's, not against `check()`

The worker establishes and checks the required state before it makes the pool (fpenv.hpp);
each thread asserts that it inherited exactly the creator's state, which the factory reads
on the calling thread. That needs no provider of `fpenv::check()`, which is the numerics
stream's and does not exist in wave 1, and it says the right thing either way: a difference
means something changed a thread's registers between the two reads. So this stream consumes
`FpControlContract` — run against its own model in `fakes.hpp` and, as the F21 check of that
model, against the real registers — and not `FpCheckContract`.

### The soak counts allocations and threads, and is a million runs by default

`tests/soak/worker/executor/` replaces the global allocation functions with counting ones —
in that binary only — and asserts that a million runs allocated nothing and left the
process's thread count, read from `/proc/self/status`, unchanged, with every index of every
run checked. A counter that could not move would pass for the wrong reason, so a second test
shows it counting. Under ASan and TSan the replacement is left out, since their runtimes
own the allocator. The million holds under every build: a first version ran a tenth of it
under the sanitizers on the guess that they would be much slower, and measurement said
otherwise (a hundred thousand runs: 7.7 s debug, 9 s TSan, 8 s ASan — the time is the
kernel's). `LOADFORGE_SOAK_RUNS` sets the count by hand: two million were run for the
figures above, and the 30-of-30 repetition below ran a hundred thousand thirty times. *Rejected:* `mallinfo2()`,
which a sanitizer runtime answers with its own figures or zeros, and resident set size,
which moves with the allocator's caching and the page cache rather than with the code.

### Repetition: 30 of 30

Every test here starts real threads, so each binary was run thirty times over
(`--gtest_repeat=30`): the unit and integration binaries under the debug build and under
TSan, 30 of 30 each with no race reported; the soak thirty times at a hundred thousand runs
in the debug build, 30 of 30 in 225 s.

### The classes the module has

P1, P2, P3 and P5, owing T1, T2, T8 and T10. P6 is not claimed: the threads start and stop,
but they have no process lifecycle — a thread cannot die alone — and the brief's table did
not list it either. T8 is added to the brief's tiers, because the real kernel and registers
are where the fakes' models are checked.

## Contract questions

1. **"Pinned where the worker was placed."** The brief does not say whether each thread is
   pinned to one CPU of the worker's set or all to the whole set. The factory takes one
   placement per thread, so either can be built; `one_thread_per_cpu` gives the first, which
   is the reading a load tool needs (Decisions). The integration stream chooses.
2. **"The calls happen on the executor's own threads or on the calling thread."** This pool
   uses only its own; the caller waits. A consumer must not assume that the calling thread
   takes part, nor that it does not — which is what the contract says.
3. **The floating-point check.** `fpenv.hpp` says "the executor's threads assert it on
   start"; read here as "assert they inherited the creator's state", not "call `check()`",
   for the reasons under Decisions. If the contract means the second, the change is one
   call in `prepare()` and a link to the numerics stream's library.
4. **One header, two suites.** A consumer of `FpControl` alone must allow `FpCheckContract`
   to go uninstantiated. Harmless, but each contract consumed separately could have its
   suite in a header of its own.
5. **A count within `threads()` of `SIZE_MAX`.** The contract allows any count; this pool's
   claim counter would wrap past `SIZE_MAX`. Such a run cannot complete in any lifetime, so
   this is recorded rather than guarded.
