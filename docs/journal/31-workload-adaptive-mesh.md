# Journal fragment 31 — workload: adaptive mesh

**Written:** 2026-10-20 17:50 UTC
**Change:** `src/workloads/adaptive_mesh` (the workload) and its tests at T1, T3, T4, T5
and T6, with one golden-digest fixture (stream 31, IMPLEMENTATION M4).

## Established by experiment

### What a generated unit looks like

Measured over units 0..199 at seed 1 with the reference mesh in the tests (not the
workload): 3601 to 3778 leaves per unit, mean 3694.6; the most leaves in one coarse cell
is 88. spec.hpp says "about 3700" and "at most about 50 in a cell"; the first agrees, the
second does not (88 over 200 units, so the 50 is an underestimate of the tail, not a bound
anyone relies on: the hard bound is 1024 and the precondition of `cell_tolerance`).
`kTypicalLeaves = 3700` in the header is this mean, rounded.

### The residual is real, and small against the tolerance

Two dealt schedules of one generated unit (3 slots seed 1; 7 slots seed 2) give integrals
that differ in bits in some of the 256 cells (the T6 test requires at least one), each
within the tolerance of the leaf-order sum. The worst residual over tolerance across
three units on a 7-slot dealt schedule is recorded by the T3 test as a property
(`worst_residual_over_tolerance`); it stays below 1 with room, as the derivation says it
must (the tolerance covers the worst case, not the typical).

### A reference written in 32-bit arithmetic disagrees with the workload at u32 maximum

The tests' first reference computed a hotspot's centre as `2 * spot.x + 1` in `uint32_t`;
for `x = 0xFFFFFFFF` that wraps, while spec.hpp's `cell_centre` takes a `uint64_t` and does
not. The T1 test for an input that is all ones caught it. The workload follows the
specification; the reference was wrong and is fixed. (A corrupt coordinate is input, never
undefined behaviour, so the hotspot's "centre" is whatever 2^33 / 1024 says; the term is
then far below any term of a real hotspot and the sums remain finite and positive.)

### Every hotspot reaches every leaf

Removing one hotspot from a unit changes the value of every leaf in every cell, not just
the cells near it (the term is a smooth function of distance with no cutoff). So in the
T5 test of input corruption the *integral* check fails first in cell 0, whatever the
corruption, if the checker evaluates cells before counts; the specification's order, all
leaf counts and then the integrals, is what makes the failure name the cell whose count
changed. The tests' checker follows the specification's order for that reason.

## Decisions

### Where the non-determinism sits: per-slot partial sums, combined in slot order

The specification wants a coarse cell's leaves summed "in the order the threads finish",
by "a shared queue". Options:

1. A lock or an atomic compare-and-swap loop on a double per coarse cell. Faithful to a
   queue literally, but it brings an atomic into a workload whose only concurrency is the
   executor's, needs a memory-order argument and an ordering test (PLAN §4.5), and makes
   the result depend on the exact interleaving even for the final combine.
2. A task per coarse cell, summing its own leaves serially. Reproducible, and therefore
   not the dynamic load balancing the specification names; it would also make the class
   BoundedResidual for no reason.
3. A task per leaf, each added into a partial sum owned by the executor slot that
   claimed it, and the slots' partial sums added in slot order after `run()` returns.

Chosen: 3. The executor's contract does the claiming (`run(count, task, ctx)`: any index
on any thread, any order, a slot that no concurrent call shares), so the "shared queue" is
the executor's; the grouping of a cell's leaves into partial sums is the schedule's,
which is exactly the freedom the specification says the bound covers (any order, any
grouping of partial sums). No atomic, no lock, no word written by two threads: slot s
writes only its own piece of scratch, and `run()` returning makes those writes visible.
The scratch is `kCoarseCells * 8` bytes per slot (2048).

### The mesh is built on the calling thread, before the parallel phase

The structure is integer logic over 256 hotspots, about 4000 cells, each tested against
all 256 hotspots, a million cheap comparisons against the roughly one million
divisions of the leaf values. Parallelising it would need per-cell leaf offsets (a prefix
sum) and buys less than 10% of the unit. Serial, it also makes the leaf list a pure
function of the input: the same list, in the same order, whatever the schedule. The list
lives on the stack: `kMaxLeaves = 256 + 3 * 5 * 256 = 4096` leaves of eight bytes (32 KB),
a bound that holds for any input, since a hotspot is in one cell per level and so splits
at most the five cells of levels 4..8 (spec.hpp's own "at most 9 cells split per
hotspot" counts levels 0..3 as well, which are not split cells here; its 6913 is a looser
bound for the same fact). One hotspot per coarse cell fills the table exactly, and a T1
test does that.

### Descriptor and plan

`Descriptor::version` is `kWorkloadVersion = 1`, separate from spec.hpp's `kVersion` as
the specification says. `paths` is scalar only: the leaf value is a fixed sequence in which
nothing is lane-parallel without changing the sequence, and an ISA other than scalar is
refused as `kConfiguration`, naming the path. `decomposition` is accepted as 0 or 1 and the
plan reports 1; anything else is refused naming the value. `logical_bytes` is the input
read once and the output written once (5120). `operations` is a typical unit's terms
(3700 leaves x 256 hotspots x 8 operations) plus one addition each; it is a typical figure,
not a bound, because the true count depends on the input.

### P4 and T5b removed from the ledger row

The brief lists P4 and T5b. This module performs no verification: it computes. It has
no outcome to classify and no check that can fail to run. The oracle stream owns both
(`src/verification/oracle/adaptive_mesh`). T5 here is kept, because the corruption tier
shows what that oracle will be handed (where the specification's checks do and do not see
a fault). P5 is added: the leaf-claiming and per-slot accumulation is the concurrency of
the module, though it carries no atomic of its own. The same decision dense_linear took
(docs/journal/06-workload-dense-linear.md).

### The tests' reference is deliberately not the workload's code

The reference mesh is a plain recursion with integer division for the containment test,
the leaf value is computed with `ldexp` and written out from the prose, and a coarse cell
is summed leaf by leaf in leaf order, which is the oracle of spec.hpp. Only the constants
and `cell_tolerance` are taken from spec.hpp. A fault in a shared helper (`cell_contains`,
`hotspot_term`) would therefore be found, not copied.

## Contract questions

1. spec.hpp says a cell has "at most about 50" leaves in a generated unit (measured). It
   measured 88 here over 200 units. Not a contract defect (the bound used is 1024), but the
   sentence is wrong and the oracle's tests for the wide-tolerance cells may rely on it.
2. spec.hpp's bound "at most 6913 leaves in all" is looser than the 4096 that holds (the
   cells of levels 0..3 are not part of any coarse cell). Harmless; a tighter figure would
   let a runner size a buffer exactly.
3. WorkloadContract skips `TheThreadCountNeverChangesAReproducibleOutput` for a
   BoundedResidual workload, with the reason that the workload's own T6 owns it. Read that
   way: the T6 tier here holds counts exact and integrals within the derived tolerance
   across 1, 2, 3, 4 and 7 slots.
4. `Executor::for_each_index` trips `bugprone-easily-swappable-parameters` in the frozen
   header (known from dense_linear); this workload calls `run` with a named task function.

## What could not be done here

No AArch64 toolchain or emulator was available in this container (`g++-aarch64-linux-gnu`
and `qemu-user` have no installation candidate), so the committed digests were produced on
x86-64 only. The argument that they hold on AArch64 is that every leaf value is a fixed
sequence of binary64 operations with contraction disabled for these targets
(`loadforge_set_fp_contract`) and every integer step is unsigned fixed-width; CI's ARM64
job is the check. If it disagrees, the fixture's `serial` lines are what to look at first.
