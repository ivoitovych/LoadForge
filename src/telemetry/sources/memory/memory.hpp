// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_MEMORY_MEMORY_HPP
#define LOADFORGE_TELEMETRY_SOURCES_MEMORY_MEMORY_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {

/// System memory from /proc/meminfo, in bytes (the file's kB are 1024).
///
///   memory.total        MemTotal       memory.buffers     Buffers
///   memory.free         MemFree        memory.cached      Cached
///   memory.available    MemAvailable   memory.swap_total  SwapTotal
///                                      memory.swap_free   SwapFree
///
/// One read of the file serves every metric in a round, so the figures in one
/// round are from one instant. Each metric is its own state: a kernel without
/// MemAvailable (before Linux 3.14) reports that one absent and the rest
/// present, and a line that is not "<Key>: <number> kB" is malformed for its
/// metric alone. No estimate stands in for a missing line. There is no wrapped
/// state: these are gauges.
class MemorySource final : public Source {
 public:
  explicit MemorySource(platform::FileSystem& filesystem, const std::string& root = {});

  [[nodiscard]] std::string_view name() const noexcept override { return "memory"; }
  [[nodiscard]] std::vector<MetricStatus> probe() override;
  void sample(std::vector<Reading>& out) override;

 private:
  topology::Source meminfo_;
  std::vector<std::size_t> present_;  ///< Indices into the field table, in probe order.
};

}  // namespace loadforge::telemetry

#endif
