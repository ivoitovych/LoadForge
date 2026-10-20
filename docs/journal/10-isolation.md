# Journal fragment 10 — fault isolation: the re-run ladder

**Written:** 2026-10-20 11:04 UTC
**Change:** stream 10 (`feature/isolation`): `src/verification/isolation/`, the provider
of `verification::Isolator`, and the choice of the core the ladder's second rung moves a
unit to.

## Established by experiment

### The machine this was written on, as its `/sys` describes it

x86-64, 4 logical CPUs, read with `cat /sys/devices/system/cpu/online`,
`cat /sys/devices/system/cpu/cpu*/topology/{physical_package_id,core_id,thread_siblings_list}`
and `cat /sys/devices/system/cpu/cpu*/cache/index*/{level,type,shared_cpu_list,size}`:

```text
online 0-3
cpu0  package 0  core 0  siblings 0    L1d 48K (0)  L1i 32K (0)  L2 2048K (0)  L3 266240K (0-3)
cpu1  package 0  core 1  siblings 1    L1d 48K (1)  L1i 32K (1)  L2 2048K (1)  L3 266240K (0-3)
(cpu2 and cpu3 alike)
node  node0 only
```

The symmetric layout of the tests (`tests/unit/verification/isolation/layouts.hpp`) is
this tree, and `OtherCoresOnThisMachine` reads the real one through the real discovery
and checks every CPU's list against the CPUs on other cores, each once, the first on the
same node whenever the node holds another core. That is the model checked against the
kernel at least once (F21). This machine has no SMT and one node, so it corroborates the
symmetric case only; the other layouts are modelled (see Decisions, 4).

### Each rule of the order has a test that fails without it

Mutations made by hand to `placement.cpp` and `ladder.cpp`, each built and run against
the module's tests, and reverted:

| Mutation | Tests failing |
|---|---|
| the node rule inverted (prefer another node) | 3 |
| the shape rule inverted | 4 |
| the shape ignores the thread count | 1 (`AShapeCountsTheCoresThreads`) |
| the shape counts shared caches too (`&&` to `\|\|`) | 1 (`AShapeIsTheCoresOwnCachesNotItsClusters`) |
| every shared cache reported as level 1 | 1 |
| SMT siblings not excluded | 18 |
| the ranked list not sorted | 8 |
| a one-thread original's single-thread answer left unknown | 3 |
| `kNotPerformed` answered as `kNo` | 9 |

The thread-count and shared-cache rows survived the first test set: no layout then had two
cores alike in caches but not in threads, or alike in kind but in clusters with caches of
different sizes. The two layouts that kill them (`uneven_smt`, `clusters_of_two_sizes`)
were added for that reason.

### Coverage

`src/verification/isolation/ladder.cpp` 17 of 17 branches, `placement.cpp` 39 of 39, and
the whole tree at 100% line, function and branch with every source file in the report
(`tools/coverage.sh build/coverage`, exit 0). Nothing needed restructuring for §1.13 or
§1.22: no aggregate is built from two allocating members, and no comparison is defined
in a header of this module. `answer_of` is `noexcept` and a free function so that its
out-of-enumeration arm is reached by a direct call rather than through the ladder only.

## Decisions

### 1. The provider is a stateless final class, and `answer_of` is public

`LadderIsolator final : public Isolator`, holding nothing, so one instance may serve any
number of isolations and the contract's "one call at a time" holds trivially. A factory
returning `unique_ptr<Isolator>` was considered and not added: the integration stream can
construct the class directly, and a factory would be a second way to do one thing.
`answer_of(RerunOutcome)` is in the header because it is where a judgement lives — an
outcome that is not a clean re-run is never `kNo` — and that judgement has an arm (an
outcome outside the enumeration) that deserves its own test.

### 2. The other core is the NEAREST other physical core, not the farthest

**This departs from the brief**, which asks for "a different physical core, preferring a
different cache domain and NUMA node where the topology offers one". The brief says that
where it and the repository's documents disagree, the documents win and the
disagreement is written here. They disagree in substance:

- verification.md §7 reads the second rung as: clean elsewhere is *consistent with a
  core-local fault*; reproduced elsewhere is *consistent with memory, uncore, or power
  delivery*. Both readings hold only if the other core shares memory and uncore with the
  original — that is, the same node and as much of the cache hierarchy as possible. A
  reproduction on a core in another node and cache domain is barely consistent with the
  original node's memory or the original cache slice, since neither was used.
- §7's own caveat list names "different NUMA placement, and therefore different memory,
  on the two cores" and "different shared-cache or CCX/cluster topology" among the things
  that produce "fails on core 5, passes on core 11" without a core-local fault. Choosing
  a far core adds exactly those confounds.
- Concretely: a fault in the L2 an efficiency cluster shares, or in the L3 of one CCX.
  Re-run on a core of the same cluster, it recurs: not core-local, correctly. Re-run
  across the cluster, it comes out clean, and the ladder reports a core-local fault on
  hardware whose fault is in the cache. `InjectedFault.ASharedClusterCacheIsFoundNotCoreLocal`
  is this case.

The far choice has one argument for it: a reproduction on a core that shares nothing with
the original is stronger evidence that the fault is not in anything local. But the
ladder's finding for any reproduction elsewhere is already `kNotCoreLocal` with high
confidence; distance adds nothing to that finding and costs the correctness of the other
one. So the order holds constant everything the snapshot lets it hold constant and
changes only the core. The function is one place and one sort key; if the manager reads
it the other way, the change is the sign of two keys and the expected lists of the tests.

### 3. The order: node, then shared cache, then shape, then id

`other_cores(snapshot, cpu)` lists every online CPU on another `(package, core)` — never an
SMT sibling, which shares the core under test — ranked by: the same NUMA node first; the
lowest cache level shared with `cpu`, lowest first (no shared cache ranks last); a core of
the same shape first; the CPU id. Node above cache because a different node changes the
memory the re-run uses, which is the larger confound, and on every layout seen a shared
cache implies a shared node anyway.

*Shape* is the snapshot's stand-in for core kind: the core's thread count and its private
caches (those serving no CPU outside the core) by level, type and size. The snapshot does
not name kinds — no capacity, no frequency — and on a heterogeneous package (x86 hybrid,
Arm big.LITTLE / DynamIQ) a re-run on a core of another kind changes the
microarchitecture as well as the core. Private caches and thread count are what the
kernel does publish that differs between kinds on every such part looked at. Shared
caches are excluded from the shape on purpose: two cores of one kind in clusters with
different-sized L2s are still one kind. Rejected: a shape that includes frequency or
`cpu_capacity` — not in the snapshot, and the snapshot is the contract this stream builds
against.

### 4. Heterogeneous layouts are modelled, not captured

No hybrid or big.LITTLE machine was available. The hybrid and big.LITTLE layouts follow
the numbering and sharing the kernel publishes for those parts (performance threads
first with adjacent siblings, efficiency cores in clusters of four sharing an L2; LITTLE
cores numbered first on DynamIQ), and every layout is a tree the real topology discovery
must accept — its cross-checks reject a tree that could not be a machine — but they are
not a capture of a real `/sys`. Open: capture both from real machines (an Alder Lake or
later x86, an Arm SoC) as committed fixtures with the command and machine, and replace
the models. The ARM64 CI runner reads its own `/sys` through `OtherCoresOnThisMachine`
on every run, which is a second real layout already.

### 5. A CPU the snapshot does not hold has no other core

`other_cores` returns an empty list for a CPU the snapshot does not hold: nothing can be
said to be on a different core from an unknown one, and an empty list is the placement
offering no other core, which the ladder already handles (the rung is skipped, its answer
unknown). Rejected: every CPU, which would offer the unknown CPU's own sibling.

### 6. Classes and tiers, against the brief's table

The brief lists P1, P4 and T1, T5. The ledger fragment declares P1, P3, P4 and T1, T5,
T5b. P3 is present: a machine of one core with one and two threads, a CPU the snapshot
does not hold, a snapshot without caches or NUMA, a placement of zero threads. T5b is
present because the unrun checks are this module's sharpest obligation: a re-run that
could not be made must never land on "transient". No P2 (nothing here can fail) and no P6
or T7 (re-running in real workers is the integration stream's `Rerunner`).
`consumes = []`: `Rerunner` has no suite (ADR 0004 says why), and the topology snapshot is
an M1 module, not a contract with a suite.

### 7. The layout builder lives in this module's unit tier and is shared with its T5

`tests/unit/verification/isolation/layouts.hpp` is included by the fault-injection tests
as well, so a fault is injected into the same layouts the order is tested on. Both
directories are this stream's; nothing outside it uses the header.

## Contract questions

### 1. The single-thread answer of a one-thread original that stopped at step 1

`IsolationReport::answers` says a rung not run is `kUnknown`, "except that a single-thread
rung skipped because the original ran one thread takes the same-core answer, which it
is". When the original ran one thread and step 1 came out clean, the single-thread rung
was not run for two reasons at once. The reference isolator in `tests/contracts/` leaves
it `kUnknown`; the suite does not test the case. **Reading taken:** the same-core answer,
always, when the original ran one thread — the same-core re-run *was* the single-thread
re-run, so "unknown" understates what was observed. The finding is the same either way
(`same_core` decides first). Tested in
`Ladder.AOneThreadOriginalThatComesOutCleanAnswersNoSingleThreadedToo`. Proposed: the
header states it, and the reference follows.

### 2. A placement of zero threads

`Placement::threads` is documented "at least 1", without saying what `isolate` does with
0. **Reading taken:** passed through as given — the same-core request carries 0 threads,
the re-runner is the party that can refuse it, and a refusal is an answer the ladder
already turns into inconclusive. The single-thread rung would be climbed (0 is not 1) if
a re-runner ever reproduced with 0 threads. Proposed: either make it a precondition the
`Rerunner` contract refuses with `kNotPerformed`, or let `Isolator` treat it as one.

### 3. Which other core

`Placement::other_cores` is "logical CPUs on physical cores other than cpu's, in the order
to try them"; the contract does not say what the order is for, and the brief and §7
point in opposite directions (Decisions, 2). Proposed: the contract or verification.md
§7 states that the second rung changes the core and holds node and cache sharing
constant where the topology allows.

### 4. An outcome outside the enumeration

A `Rerunner` returning a value outside `RerunOutcome` is recorded in the step as given and
answered `kUnknown`, never `kNo`. The contract does not say; this is the reading under
which no unrecognised value can produce the transient finding.
