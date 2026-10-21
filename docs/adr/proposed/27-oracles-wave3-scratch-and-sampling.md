# ADR — How an oracle gets its working memory, and how it samples

**Status:** proposed
**Date:** 2026-10-20 17:40 UTC
**Stream:** 27, the wave-3 oracles (FFT, stencil, sparse, graph, particles)

## Context

An oracle's `check()` is `const`, may run on any thread for any unit, and must
return `NotChecked` — never an agreement — when it cannot obtain what it needs,
including memory for its reference computation (`verification.md` §5,
`oracle.hpp`). Three of the five wave-3 oracles judge a sample of a unit, and
the contract requires that "which blocks is a function of `evidence.index`
alone".

Two questions came up five times and will come up in every later oracle:
where does the working memory live, and how is a sample chosen.

## Decision

**Working memory.** An oracle allocates nothing it can avoid. The FFT, stencil,
sparse and particles oracles read the evidence in place and keep their small
buffers (at most 98 KiB) on the stack, so there is no allocation to fail and no
`NotChecked` path for one. An oracle that must recompute the whole output — the
graph oracle needs about 4 MiB — takes the memory from a `ScratchFactory` it is
constructed with (a plain function pointer, defaulting to
`new (std::nothrow)`), asks once per `check()` and not for evidence that is
refused before any work, and answers `NotChecked` with nothing compared when the
factory returns nothing. A test passes a factory that returns nothing; that is
how the failure is injected (`docs/IMPLEMENTATION.md` §2.2, P2).

**Sampling.** A sample is a pure function of the unit index built from the
splitmix64 finaliser (a bijection on 64 bits, unsigned arithmetic only) and a
fixed stride coprime to the range sampled, so that the chosen values are
distinct by construction. The pinned members (bin 0 and n/2; particle 0 and
n−1; the two corner windows) are written in, not drawn. There is no rejection
loop.

## Alternatives rejected

- **Catch `std::bad_alloc` in `check()` and return `NotChecked`.** The handler
  can be reached only by making the global allocator fail, which a test cannot do
  without a hook in the allocator; the branch would be a line the coverage gate
  counts and no test enters (journal §1.13), and the project's rule is to inject
  every failure, not to hope for it.
- **Scratch as a member of the oracle, or thread-local.** `check()` is `const`
  and concurrent; shared scratch needs a lock, and thread-local scratch is hidden
  state with a lifetime nobody tests.
- **The graph oracle's scratch on the stack.** 4 MiB exceeds a worker thread's
  default stack under the sanitizers.
- **Drawing the sample from Philox with rejection of repeats.** A repeat occurs
  for about one unit in forty, so the rejection branch needs a unit found by
  search to cover it, and the pinned members need a second rejection rule. The
  arithmetic-progression sample has neither branch.

## Consequences

- A later oracle that fits in a few tens of kilobytes copies the first pattern
  and has no allocation path to test; one that does not copies the second, and
  its ledger row declares P2 with the injected factory as the test.
- The factory is a public constructor parameter of the oracle class. It is a
  seam in product code, like `platform::Syscalls`, and not a test hook: the
  release gate looks for test-only symbols, and this is not one.
- Two units share a sample only when the number that fixes it collides: for the
  particles oracle, one chance in 4094 per pair of indices. That is a property of
  the sample, not a flaw in the check; the contract only asks that a re-run of the
  same unit checks the same blocks.
