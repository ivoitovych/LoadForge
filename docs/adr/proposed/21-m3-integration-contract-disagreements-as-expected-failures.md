# ADR (proposed) 21 — A disagreement between two components is a test that is expected to fail

**Status:** proposed (stream 21; the manager numbers it when it lands)
**Date:** 2026-10-20 21:41 UTC

> **Outcome, 2026-10-21 (branch `fix/m3-contract-disagreements`):** all seven disagreements
> were corrected by a contract change (`docs/journal/m3-contract-corrections-*.md`), the
> `WILL_FAIL` executable was removed, and its tests now pass in
> `tests/integration/m3/corrections_test.cpp`. The mechanism worked as designed: each test
> stayed red-by-design until its contract was fixed, and then moved. Nothing is registered
> `WILL_FAIL` now.

## Context

ADR 0003 has each component read a contract for itself and compares the readings by running
the provider's suite against the consumer's mock. That finds disagreements either suite
thought to test. The real-against-real tier finds the rest, and when it does, the finding is
about two components neither of which this stream may edit, against a contract it may not
edit either. Three bad ways to keep the finding exist: a note nobody reruns, a test disabled
so the build stays green and nobody remembers it, or a failing test that turns every pull
request red.

## Decision

A disagreement is a **test that asserts what the contract's clause says, fails against
today's components, and is registered with `WILL_FAIL`**. It is named
`<Component>_<ContractFile>_<Clause>`, after the component that is wrong and the clause it is
wrong against, and sits in its own executable (`loadforge_integration_m3_disagreements`) so
that the passing suite's sanitizers and coverage are untouched. The journal fragment says,
for each, what was observed, on what, and who can fix it.

When the component is corrected the test passes, `WILL_FAIL` inverts it, CI goes red, and the
test moves into the passing suite in the same change that corrects the component.

## Consequences

- A disagreement cannot be forgotten, and cannot block anyone while it stands.
- A test that fails for the wrong reason (a sanitizer report, a crash) is also "expected".
  The same code runs in the passing suite under the sanitizers, and each disagreement test
  states its observed value in the assertion's message so a reader can tell.
- A real-kernel disagreement is not made a `WILL_FAIL` test when whether it fails depends on
  the machine; it is asserted on what the component asks of the kernel through the seam,
  where the failure is the same everywhere, and the real-kernel evidence goes in the journal.
