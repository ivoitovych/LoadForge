// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ISOLATION_LADDER_HPP
#define LOADFORGE_VERIFICATION_ISOLATION_LADDER_HPP

#include "verification/isolation.hpp"

namespace loadforge::verification::isolation {

/// What one re-run's outcome answers: kYes for reproduced, kNo for clean, and
/// kUnknown for anything else -- a re-run not performed, and an outcome
/// outside the enumeration, which a re-runner that sent one has not answered.
/// Never kNo for something that was not a clean re-run: the ladder's most
/// alarming finding is reached through a kNo, and only a check that was made
/// and passed may produce one.
[[nodiscard]] Answer answer_of(RerunOutcome outcome) noexcept;

/// The provider of Isolator: verification.md §7's ladder, run through a
/// Rerunner.
///
/// THE ORDER, AND WHERE IT STOPS
/// -----------------------------
/// Same core first, with the original thread count. Anything but a
/// reproduction stops there: clean is a non-reproducible transient, and a
/// re-run that could not be made leaves nothing to compare the later rungs
/// against. A reproduction climbs on: another core, when the placement
/// offers one, then one thread on the original core, unless the original ran
/// one thread, in which case step 1 was that step and is not made twice.
///
/// A later rung that cannot be made does not stop the ladder. Each rung asks
/// its own question, and a failure to start a worker on core 9 says nothing
/// about what one thread on core 5 would find.
///
/// THE SINGLE-THREAD ANSWER OF A ONE-THREAD ORIGINAL
/// -------------------------------------------------
/// When the original ran one thread, the same-core re-run WAS the
/// single-thread re-run, so its answer is the single-thread answer whatever
/// it was -- kNo after a clean step 1 as well as kYes after a reproduction.
/// The finding does not depend on it (the same-core answer decides first),
/// but the report states what was observed, and "unknown" would understate
/// it (docs/journal/10-isolation.md, contract question 1).
///
/// Stateless: one isolator may serve any number of calls, one at a time as
/// the contract asks.
class LadderIsolator final : public Isolator {
 public:
  [[nodiscard]] IsolationReport isolate(const WrongResult& finding, const Placement& placement,
                                        Rerunner& rerunner) override;
};

}  // namespace loadforge::verification::isolation

#endif
