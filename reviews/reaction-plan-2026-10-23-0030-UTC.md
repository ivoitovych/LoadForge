# Reaction plan: internal review of 2026-10-22 23:25 UTC

**Written:** 2026-10-23 00:30 UTC

## What this review was, and was not

On 2026-10-22 at about 23:25 UTC, six read-only reviews of the tree at `main` 720fdbb were run from inside the project's own working session. Their reports:

- `internal-review-2026-10-22-2325-UTC-a-worker-control.md`: worker control and the mixed workload
- `internal-review-2026-10-22-2325-UTC-b-explore.md`: `explore`, the trial runner, the statistics
- `internal-review-2026-10-22-2325-UTC-c-journal-orchestration.md`: the crash journal and the controller's run loop
- `internal-review-2026-10-22-2325-UTC-d-docs.md`: the user documents against the code
- `internal-review-2026-10-22-2325-UTC-e-root-md-adr.md`: the root files and the ADRs
- `internal-review-2026-10-22-2325-UTC-f-design-docs.md`: the design documents against the tree

**These reviews are not an outside review.** They come from the same working session that built the code, and they count as nothing towards the standing outside-review item in `docs/ROADMAP.md`.

## How each finding was decided

Every finding was treated as a claim. Each one was reproduced or disproved by a separate verification pass before any fix started. That pass used:

- a test that fails on the current code;
- an experiment against the real binary or the kernel;
- a seeded simulation; or
- a derivation with file and line.

The verdicts and their evidence are in three files:

- `internal-review-verdicts-2026-10-23-0030-UTC-a-c.md`: reviews A and C, with 12 reproducing tests, all failing on 720fdbb.
- `internal-review-verdicts-2026-10-23-0030-UTC-b.md`: review B, with 17 failing tests and two seeded simulations.
- `internal-review-verdicts-2026-10-23-0030-UTC-d-e-f.md`: reviews D, E and F.

**Verdicts:**

- **CONFIRMED:** the claim holds as stated.
- **PARTLY:** part holds. The verdict file says which part.
- **REJECTED:** the evidence contradicts the claim.

Severities are the verification pass's, not the reviewer's.

## Code findings

### Review A: worker control and the mixed workload

| # | Finding | Verdict | Sev. | Work item |
|---|---|---|---|---|
| A1 | A framing fault in one read drops the good records before it: a Stop on the control pipe, findings on the record pipe | CONFIRMED | medium | R1 |
| A2 | A worker that leaves because the controller is gone reports a zero tally | CONFIRMED | medium | R1 |
| A3 | The memory budget is checked against the start footprints only. Later Switches exceed it (seeds 1, 5, 6: 384, 288 and 384 MiB checked, against 480, 448 and 416 MiB held) | CONFIRMED | medium | R2 |
| A4 | A command written to a closed pipe is in no counter | PARTLY: only the label "not sent" over-promises | low | R2 |
| A5 | No heartbeat during a runner replacement | CONFIRMED (mechanism) | low | R2 |
| A6 | `notes` and `told` grow without bound | PARTLY: bounded in practice by the acknowledgements and the 64-message cap | low | open thread |
| A7 | A Switch can revive a runner that halted itself | PARTLY: the not-performed record is still sent | low | open thread |
| A8 | The schedule's `problem()` accepts an empty Start and repeated labels | CONFIRMED | low | R1 |
| A9 | Overflow in `next_slice_units` (above about 9.8 h, not 4.9 h), the `int` cast, and `roster_of` | PARTLY | low | R1 |

### Review C: the crash journal and the run loop

| # | Finding | Verdict | Sev. | Work item |
|---|---|---|---|---|
| C1 | `run.duration` overflows int64 on the `--config` path. A huge value ends the run at once | CONFIRMED | low | R2 |
| C2 | An inherited ignored SIGCHLD makes the run exit 1 with no report | CONFIRMED (real binary under such a launcher) | medium | R2 |
| C3 | Result files are renamed without fsync, and a close error goes unread | CONFIRMED (strace; full-disk test) | medium | R2 |
| C4 | A run that died of a controller error leaves a journal reading "ended cleanly", which the next start deletes | PARTLY: the ENOSPC sub-claim is rejected, because the terminator write fails too and the journal reads abnormal | medium | R2 |
| C5 | A journal fsync inside the beat loop can starve the beat (`--rotation 1ms --journal`: workers report the controller lost, exit 3) | CONFIRMED (mechanism) | low | R2 |
| C6 | `--journal PATH` renames any non-journal file. Two runs on one path interfere | CONFIRMED | low | R2 |
| C7 | Replay is looser than its comment: an overshooting length reads as a torn tail, a negative time is accepted, and labels are printed raw | CONFIRMED (a, b, c) | low | R1 (a, b); R2 (c) |

### Review B: explore and its statistics

| # | Finding | Verdict | Sev. | Work item |
|---|---|---|---|---|
| B1 | Corrected hardware errors are scored, unflagged, and exit 0. The README says 5 | CONFIRMED | high | R3 |
| B2 | A trial that did not settle leaves no trace in the report | CONFIRMED | medium | R3 |
| B3 | A settle window longer than the settle maximum can never settle and is not refused | CONFIRMED | medium | R3 |
| B4 | The load window includes worker start-up | PARTLY: a late deadline tick shifts the window the other way, and the size is unmeasured | low | open thread |
| B5 | `delta` loses the start of the window | CONFIRMED: always one sample interval, not 0 to 1 | medium | R3 |
| B6 | "Separated" is claimed with a rival never examined | PARTLY: 4 in 20000 searches by this mechanism. The wrong winner is named 38% of the time at `--replicates 1` | low | R3 (wording), open thread |
| B7 | The winner's interval is computed on the samples that selected it | CONFIRMED: 87.3% coverage against 95.1%, a bias of +0.61 sd | medium | R3 |
| B8 | The reference's degrees of freedom are overstated | CONFIRMED with corrected numbers: effective df 1.0, 1.4, 1.9, 2.3 and 3.4 for 3, 4, 5, 6 and 8 readings | medium | R3 |
| B9 | Tied samples give a zero-width interval | PARTLY: a limit of plug-in variance | low | open thread |
| B10 | A finding in a reference trial exits 3 and drops the measured results | PARTLY: documented (JOURNAL §2.56), but at odds with the README | medium | R3 |
| B11 | A duration equal to the sample interval fails only after a full trial, with exit 6 | CONFIRMED | low | R3 |
| B12 | The elimination wrap; the settle-scope mismatch; a false "none" sentence; the out-of-order test | CONFIRMED (a, c, d), PARTLY (b) | low | R3 |

## Documentation findings

All 13 of review D, 8 of review E and 51 of review F were checked. None was rejected. A few hold only in part, as the verdict file says.

- **D1 (high, code):** an empty milestone gave the version `0.0.0-`, so a `v1.0.0` tag could never pass the release workflow.
- **Every other finding:** the document is the side that is wrong.

## Work items

| Item | Content | Where | State |
|---|---|---|---|
| R0 | Release version without a trailing hyphen (D1) | #103 | open |
| R0 | Validation guide, packaging notes and CONTRIBUTING (D5, D6, D8 part, D9 part, D10, D11, D13) | #104 | open |
| R0 | ADRs numbered 0005 to 0010, amendments, stream brief states (E F1 to F8) | #105 | open |
| R0 | Design documents reconciled with the tree (F A1 to E9, except B4); carries 34c | #106 | open |
| R1 | Code fixes outside the files the temperature ceiling edits: A1, A2, A8, A9, C7a, C7b | `fix/review-ipc-worker-journal` | in progress |
| R2 | Code fixes in the controller and command line, after #107 merges: A3, A4 label, A5, C1, C2, C3, C4, C5, C6, C7c | after #107 | queued |
| R3 | Explore fixes, after #107 merges: B1, B2, B3, B5, B6 wording, B7, B8, B10, B11, B12 | after #107 | queued |
| R4 | README, `docs/safety.md`, `docs/architecture.md`: D2, D3, D4, D7, D8 and D9 README parts, D12, and F's B4 | after #107 | queued |

## Decisions taken while answering

- **`loadforge selftest` is a 1.0 item (roadmap 34e).**
  - PLAN F2 calls it "required, not optional".
  - It is built from what exists: the golden digest, plus one short verified unit per workload with its oracle.
  - Until it lands, the documents say it is not built.
- **Benchmark mode is not in 1.0.** The configuration parses it, and `stress --config` refuses it.
- **`tools/gen-golden/` will not be built.** The per-fixture `make_golden.py` scripts are the route for regenerating golden vectors.
- **Accepted ADRs change only by dated amendment.** ADR 0004's table row stays as written.

## Open threads, not fixed

These are design limitations, documented here and to be folded into `docs/JOURNAL.md` §6. Each needs a design decision, not a patch.

- A6 and A7.
- B4: the start-up portion of the load window, unmeasured.
- B6: how often successive halving with one replicate picks the wrong winner.
- B9: the plug-in variance on quantised sensors.
