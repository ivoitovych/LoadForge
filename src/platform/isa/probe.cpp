// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/isa/probe.hpp"

namespace loadforge::isa {

SnapshotProbe::SnapshotProbe(const IsaSource& source, Architecture architecture)
    : set_(detect_paths(source, architecture)) {}

simd::IsaSet SnapshotProbe::supported() const { return set_; }

std::unique_ptr<simd::IsaProbe> make_native_probe() {
  const RealIsaSource source;
  return std::make_unique<SnapshotProbe>(source, native_architecture());
}

}  // namespace loadforge::isa
