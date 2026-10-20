// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_EDAC_EDAC_HPP
#define LOADFORGE_TELEMETRY_SOURCES_EDAC_EDAC_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {

/// What the user can do about an EDAC attribute that could not be read, for
/// the state it is in -- or, for kAbsent, that nothing can be done.
///
/// EDAC is the one source whose absence is not a failure to be fixed (PLAN F3's
/// table): the driver for a memory controller is missing on most consumer
/// boards, and no setting loads it. So kAbsent advises nothing, and no other
/// state is ever reported for a controller the kernel does not have. Advising
/// "run privileged" for a machine without ECC would be a fix that cannot work.
[[nodiscard]] std::string edac_remediation(topology::Availability kind, std::string_view path);

/// Corrected and uncorrected memory errors per memory controller (PLAN F13),
/// read from /sys/devices/system/edac/mc/mc<N>/{ce,ue}_count.
///
/// Reports `edac.corrected` and `edac.uncorrected`, counters of events with
/// scope kMemoryController and the controller's number as index -- the names
/// result.hpp's verdict reads. The kernel keeps each count in 32 bits and
/// prints it unsigned, so both are counters with modulus 2^32 and a larger
/// value is refused as malformed rather than passed on.
///
/// Controllers are found by number, mc0 upwards, stopping at the first whose
/// ce_count is absent: the kernel numbers them contiguously as they register.
/// A machine with none gets one status per metric, kAbsent, at machine scope.
///
/// Thread-safety: none (source.hpp). Holds one topology::Source per attribute,
/// which is where "was here, now gone" is remembered.
class EdacSource final : public Source {
 public:
  static constexpr std::string_view kDefaultSysfsRoot = "/sys";
  /// The width of the kernel's counters: readings are below this.
  static constexpr std::uint64_t kModulus = std::uint64_t{1} << 32U;

  explicit EdacSource(platform::FileSystem& filesystem,
                      std::string_view sysfs_root = kDefaultSysfsRoot);

  [[nodiscard]] std::string_view name() const noexcept override { return "edac"; }
  [[nodiscard]] std::vector<MetricStatus> probe() override;
  void sample(std::vector<Reading>& out) override;

 private:
  struct Channel {
    topology::Source file;
  };

  platform::FileSystem* filesystem_;
  std::string root_;
  std::vector<Channel> channels_;
};

}  // namespace loadforge::telemetry

#endif
