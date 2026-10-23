# ADR 0002 — Continuous development: decide, record, keep moving

**Status:** accepted
**Date:** 2026-10-20 01:22 UTC

## Context

Between 2026-08-24 and 2026-10-19 the repository saw commits on ten days out of
fifty-seven. M0 is complete; M1 has been code-complete on a branch for weeks
without landing. The gaps were not spent on hard problems. They were spent
waiting: for permission to commit, for a decision that common sense could have
made, for a review nobody had been asked to give. At that pace 1.0 is more than
a year away for a project whose design is already written down in full.

Twenty pull requests have been merged and none has received an outside review.
The pull-request workflow still earns its place, because every PR runs the
complete gate set on the exact merge candidate, but waiting on a review that
does not exist does not.

## Decision

1. **Work proceeds without intermediate stops.** Each increment is built,
   verified against the full local gate set, committed, pushed, opened as a PR,
   and merged once every required check is green. The next increment starts
   immediately.
2. **Decisions are made, recorded, and acted on in the same sitting.** When a
   choice has more than one defensible answer, the options are researched to the
   depth the choice deserves, the most defensible one is taken, and the options
   and the reasons are written down: as an ADR when ADR 0001's test applies,
   otherwise in the journal or the PR description. The record exists so the
   decision can be reviewed and reversed later; it is not a request for
   permission now.
3. **Reviews are asynchronous.** A review that arrives on an open PR is
   answered on that PR. A review that arrives after merge becomes a follow-up
   change. No PR waits for one.
4. **Work stops only for** an action that destroys something that cannot be
   recovered, or a change to the project's scope. Everything else is a decision
   to make and record.
5. **The plan lives in the repository.** `docs/ROADMAP.md` is the ordered
   queue of work from here to 1.0. It is read at the start of every working
   session and updated as items land, so that no session has to reconstruct
   where the project is.

## Consequences

- The quality bar does not move. Every gate that ran before still runs, locally
  before the push and in CI before the merge.
- More decisions are taken without a second opinion. The mitigation is that
  each one is written down where a reviewer can find it, and that the project
  still needs, and is still looking for, an outside reviewer.
- `docs/ROADMAP.md` becomes a document that must stay true, like the status
  sections it summarises.

> **Amended, 2026-10-23 00:15 UTC:** the dates in the Context carry no time. The period begins
> at the first commit, 2026-08-24 20:35 UTC (the time of ADR 0001), and ends at the review
> of 2026-10-19 04:49 UTC. The decision is unchanged.
