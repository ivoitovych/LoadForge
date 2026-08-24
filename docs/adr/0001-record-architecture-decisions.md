# ADR 0001 — Record architecture decisions

**Status:** accepted
**Date:** 2026-08-24

## Context

The design phase produced nineteen findings and five review rounds before a line
of code existed. Several were corrections to earlier decisions — floating-point
contraction, supervision as a process rather than a thread, the scope of
cross-architecture determinism, the strength of the ARM/TSan evidence claim.

`docs/PLAN.md` records those in a revision history, which works while one person
holds the whole design in their head. It will not scale once contributors arrive
and decisions accumulate: a reader wanting to know *why* a constraint exists
should not have to read a nine-revision changelog to find out.

## Decision

Architecture decisions are recorded as numbered ADRs in `docs/adr/`.

An ADR is written when a decision:

- constrains future work in a way that is expensive to reverse, or
- will look arbitrary to someone who was not present for the discussion, or
- reverses a previous decision.

Routine choices do not get an ADR. The test is whether a competent contributor
might reasonably undo the decision because the reason is not visible.

Each ADR states **context**, **decision**, **consequences**, and — where a
decision replaces an earlier one — what changed and why. ADRs are immutable once
accepted; a superseded ADR is marked as such and points at its replacement,
rather than being edited. The history of a decision is part of its value.

The existing normative documents (`determinism.md`, `verification.md`,
`platforms.md`) remain the place for *rules*. ADRs are the place for *reasons*.

## Consequences

- One more file per significant decision, written while the reasoning is fresh.
- `PLAN.md`'s revision history stops being the only record of why something is
  the way it is.
- Reversals become explicit and auditable instead of silent edits, which matters
  for a project whose credibility rests on being accurate about what it knows.
