# Journal fragment 14 — the timeline and the sampler

**Written:** 2026-10-20 12:52 UTC
**Change:** `src/telemetry/timeline/` and `src/telemetry/sampler/`, their tests and ledger
fragments.

## Established by experiment

### What a point costs, and what five hours cost

`sizeof(Point)` is fixed by the frozen header: an `int64_t` and a `std::variant` whose
largest alternative is an `Unavailable` (an enum and three `std::string`s). The figure is
recorded by the memory test (`RecordProperty("point_bytes")`) so it appears in the test
report on each architecture rather than being asserted from memory. Five hours at 10 Hz over
64 series is 18,000 s × 10 × 64 = 11,520,000 points; the stated bound is
`3 × capacity × sizeof(Point)` (see Decisions), tested for its arithmetic
(`MemoryBound.AFiveHourRun...`) and, on a run one hundredth the size, against the room the
buffers actually hold (`MemoryBound.TheRoomHeld...`). Property tests hold the premise of the
bound — room ≤ 2 × points — over random appends.

### A `Types<...>` list with commas cannot be written inside `INSTANTIATE_TYPED_TEST_SUITE_P`

The macro splits its arguments at the commas of a template argument list. The list is
named by an alias outside the macro.

## Decisions

### The timeline is a vector of points per series, not a ring and not one preallocated block

`TimelineView::points()` returns a `std::span`, so a series must be contiguous, which rules
out a ring or a chunked store. Options: (a) reserve `capacity` points up front — a five-hour
run's worth, over a gigabyte, paid by a one-minute run; (b) one vector per series growing
geometrically — memory follows the run. Chose (b). Its cost is the growth slack, bounded as
follows: resident ≤ 2 × capacity × sizeof(Point) (a vector's capacity is never more than
twice its size); transient, while one series grows, its old buffer is also live, at most
capacity × sizeof(Point) more (one writer, so one series grows at a time). The stated bound is
the sum, 3 × capacity × sizeof(Point), by `memory_bound_bytes()`. Not done: compacting an
`Unavailable` out of line, which would need a different `Point` (a contract change).

### Order in which a refusal is chosen

The contract lists the faults as unknown series, out of order, kind mismatch, capacity. The
timeline tests them in that order, so a point that is both stale and of the wrong kind is
reported stale, and a point that would pass everything but arrives at capacity is reported
capacity. Capacity is last because a refusal for any other reason is the caller's mistake and
should be reported even on a full timeline.

### A probe the timeline refuses is remembered

The contract says a series the timeline refuses is an error and nothing more is registered.
It does not say what a second `probe()` does afterwards. Probing again would probe the
sources a second time (the contract says once) and register series already registered.
So the first outcome, success or the refusal, is stored and returned on every later call, and
`round()` reads nothing after a refused probe. Registered series of the sources before the
refusal stay on the timeline (a sink has no remove).

### A source's readings are used all or none

A source that appends a different number of readings than it has present metrics has none of
them used: each of its series gets a `kMalformed` `Unavailable` whose detail reads
"<source> returned N readings for M present metrics" and whose remediation says it is a
defect in that source. The path is empty, because the sampler does not know which file the
source read. Other sources are unaffected. Readings of the wrong *kind* (a counter for a
gauge series) from a source with the right count are not checked by the sampler: the timeline
refuses them with `kKindMismatch`, which ends the round with that error, as the contract
says of any refusal.

### No thread, no P5

The sampler reads no clock and starts no thread, as the contract recommends. Neither module
owes a P5 argument.

## Contract questions

### Two suites in one header

`telemetry_contract.hpp` registers `SourceContract` and `TimelineContract`; a binary
instantiating one fails unless it allows the other to stay uninstantiated. The timeline's
test allows `SourceContract` and the sampler's allows `TimelineContract`
(`GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST`).

### What does `probe()` do after a refused probe?

Not stated (see Decisions). Reading chosen: the first outcome is final.

### Does the sampler validate a source's `probe()` against source.hpp?

The source contract promises at least one status, valid names, and so on; the sampler
trusts it. A source that returns an empty list yields an empty entry in the report and no
series. Not defended further, because the contract assigns that check to the source's own
provider suite.

### `round()` and a first-point timestamp

A round at a time not later than the series' last is refused by the timeline; the sampler
passes the refusal through. The first refused point of a round ends the round, so the
earlier sources of that round have appended and the later ones have not been appended
(they have been sampled: a source's `sample()` is called before its appends are tried, and
the sources after the refusal are not sampled at all).
