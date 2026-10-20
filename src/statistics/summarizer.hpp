// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_STATISTICS_SUMMARIZER_HPP
#define LOADFORGE_STATISTICS_SUMMARIZER_HPP

// The provider of statistics/signature.hpp. Every sum is a Neumaier
// compensated sum taken in timestamp order, so one timeline gives one
// signature, bit for bit, on every machine (docs/determinism.md).

#include "core/result.hpp"
#include "statistics/signature.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::statistics {

class CompensatedSummarizer final : public Summarizer {
 public:
  [[nodiscard]] core::Result<LoadSignature, StatisticsError> summarize(
      const telemetry::TimelineView& view, const Window& window) const override;
};

}  // namespace loadforge::statistics

#endif
