// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SAMPLER_HPP
#define LOADFORGE_TELEMETRY_SAMPLER_HPP

// CONTRACT (ADR 0004). The sampler: every source, read in rounds, onto the
// timeline (IMPLEMENTATION §3, telemetry/sampler). Frozen: changed only by a
// contract change, never inside a stream.
//
//   Provider: the telemetry-pipeline stream (src/telemetry/sampler/).
//   Consumers: the controller, which probes at the start of a run, states the
//     capability report, and calls a round every sampling interval.
//
// THE SAMPLER IS THE ONLY PROBER
// ------------------------------
// probe() is the one place Source::probe() is called (source.hpp: once), because
// it must also register the present metrics as series on the timeline, in the
// order the sources report them. The capability report (CapabilityReport) is
// probe()'s answer; telemetry/capability renders it and probes nothing. A
// controller that wants the report calls probe() -- as often as it likes, for a
// repeat answers the same report and probes nothing -- and never probes the
// sources some other way before or beside it.
//
// ROUNDS, DRIVEN BY THE CALLER
// ----------------------------
// The sampler starts no thread and reads no clock: the caller says when a
// round happens and what time it is. The controller already has one loop with
// one blocking call (controller/orchestration.hpp) and a clock it reads every
// tick; sampling in that loop adds no concurrency to be wrong about (PLAN
// §4.5: prefer not to have the path at all), and makes every round a test
// with a stated time. A provider that needs a thread for a slow source is
// free to have one behind this interface, and owes the P5 argument and
// ordering test that come with it.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/result.hpp"
#include "telemetry/source.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::telemetry {

/// Reads its sources onto a timeline.
///
/// probe() calls each source's probe() once, in the order the sources were
/// given, registers each PRESENT metric as a series on the timeline in that
/// same order, and answers the capability report: one SourceStatus per
/// source, in order, named by the source's name(), holding the statuses its
/// probe returned. A second call answers the same report and probes nothing.
/// A series the timeline refuses is an error, and nothing more is registered.
///
/// round(now_ns) reads every source once, in order, and appends each reading
/// to its metric's series at now_ns; it answers how many points it appended.
/// Before probe() it reads nothing and answers 0. A source that appends a
/// number of readings other than its present metrics' count has broken its
/// contract (source.hpp): none of its readings is used, and each of its
/// series gets an Unavailable of kind kMalformed whose detail names the
/// source and both counts, so one faulty source never shifts another's
/// readings onto the wrong series. A timeline refusal (now_ns not later than
/// the last round's, capacity) ends the round with that error; points
/// appended before it stay.
///
/// Thread-safety: one thread at a time, which is also the timeline's writer.
class Sampler {
 public:
  Sampler() = default;
  Sampler(const Sampler&) = delete;
  Sampler(Sampler&&) = delete;
  Sampler& operator=(const Sampler&) = delete;
  Sampler& operator=(Sampler&&) = delete;
  virtual ~Sampler() = default;

  [[nodiscard]] virtual core::Result<CapabilityReport, TimelineError> probe() = 0;
  [[nodiscard]] virtual core::Result<std::size_t, TimelineError> round(std::int64_t now_ns) = 0;
};

/// A sampler over `sources`, writing to `timeline`, which it borrows and
/// which must outlive it. Provided by the telemetry-pipeline stream.
[[nodiscard]] std::unique_ptr<Sampler> make_sampler(std::vector<std::unique_ptr<Source>> sources,
                                                    TimelineSink& timeline);

}  // namespace loadforge::telemetry

#endif
