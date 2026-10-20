// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_TELEMETRY_SOURCES_THERMAL_OWNED_HPP
#define LOADFORGE_TESTS_UNIT_TELEMETRY_SOURCES_THERMAL_OWNED_HPP

// A source together with the machine it reads, so that the source contract
// (whose subjects are built by a static make() with no arguments) can be run
// once per state: `prepare` puts the machine in the state before the probe, and
// `after_probe` changes it between the probe and the first sample, which is
// how "vanishes mid-run" is made.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "unit/telemetry/sources/thermal/memfs.hpp"

namespace loadforge::telemetry::fixtures {

using Edit = std::function<void(MemFs&)>;

template <typename Inner>
class OwnedSource final : public Source {
 public:
  explicit OwnedSource(const Edit& prepare, Edit after_probe = {}, Edit after_sample = {})
      : files_(fs_),
        inner_(files_),
        after_probe_(std::move(after_probe)),
        after_sample_(std::move(after_sample)) {
    if (prepare) {
      prepare(fs_);
    }
  }

  [[nodiscard]] std::string_view name() const noexcept override { return inner_.name(); }

  [[nodiscard]] std::vector<MetricStatus> probe() override {
    std::vector<MetricStatus> statuses = inner_.probe();
    if (after_probe_) {
      after_probe_(fs_);
    }
    return statuses;
  }

  void sample(std::vector<Reading>& out) override {
    inner_.sample(out);
    if (after_sample_) {
      after_sample_(fs_);
    }
  }

  [[nodiscard]] MemFs& fs() { return fs_; }

 private:
  MemFs fs_;
  platform::FileSystem files_;
  Inner inner_;
  Edit after_probe_;
  Edit after_sample_;
};

/// A source over the machine this process runs on, with the real seam: the
/// real-against-real tier (ADR 0003), where no state is chosen and the contract
/// must hold on whatever the machine has.
template <typename Inner>
class RealMachineSource final : public Source {
 public:
  RealMachineSource() : files_(real_), inner_(files_) {}

  [[nodiscard]] std::string_view name() const noexcept override { return inner_.name(); }
  [[nodiscard]] std::vector<MetricStatus> probe() override { return inner_.probe(); }
  void sample(std::vector<Reading>& out) override { inner_.sample(out); }

 private:
  platform::RealSyscalls real_;
  platform::FileSystem files_;
  Inner inner_;
};

/// tests/fixtures/<relative>, found from the source tree the tests were built from.
inline std::filesystem::path fixture(const std::string& relative) {
  return std::filesystem::path(LOADFORGE_SOURCE_DIR) / "tests" / "fixtures" / relative;
}

}  // namespace loadforge::telemetry::fixtures

#endif
