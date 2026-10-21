// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_ISA_PROBE_HPP
#define LOADFORGE_PLATFORM_ISA_PROBE_HPP

#include <memory>

#include "platform/isa/detect.hpp"
#include "primitives/simd/dispatch.hpp"

namespace loadforge::isa {

/// An IsaProbe whose answer is read once, from `source`, when it is made, and
/// is then a constant: the same on every call, from any thread, and independent
/// of `source`'s lifetime.
class SnapshotProbe final : public simd::IsaProbe {
 public:
  SnapshotProbe(const IsaSource& source, Architecture architecture);

  [[nodiscard]] simd::IsaSet supported() const override;

 private:
  simd::IsaSet set_;
};

/// The probe for the running machine.
[[nodiscard]] std::unique_ptr<simd::IsaProbe> make_native_probe();

}  // namespace loadforge::isa

#endif
