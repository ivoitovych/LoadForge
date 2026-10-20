// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference implementation of the dispatch contract, run against its
// suites: proof the suites run and can be passed. Not the provider's code --
// the numerics stream writes that, and runs the same suites against it.

#include <memory>
#include <optional>
#include <string>

#include "contracts/simd/dispatch_contract.hpp"
#include "primitives/simd/dispatch.hpp"

namespace loadforge::contracts {
namespace {

using simd::DispatchError;
using simd::DispatchFault;
using simd::IsaPath;
using simd::IsaSet;

core::Result<IsaPath, DispatchError> reference_select(std::optional<IsaPath> requested,
                                                      IsaSet supported, IsaSet compiled) {
  if (requested.has_value()) {
    const IsaPath path = *requested;
    if (path == IsaPath::kScalar) {
      return path;
    }
    if (!compiled.contains(path)) {
      return DispatchError{DispatchFault::kNotCompiled, path,
                           std::string(simd::describe(path)) + " is not compiled into this build"};
    }
    if (!supported.contains(path)) {
      return DispatchError{DispatchFault::kNotSupported, path,
                           std::string(simd::describe(path)) + " is not supported here"};
    }
    return path;
  }
  for (const IsaPath path : simd::kPreference) {
    if (supported.intersect(compiled).contains(path)) {
      return path;
    }
  }
  return IsaPath::kScalar;
}

struct ReferenceSelect {
  static core::Result<IsaPath, DispatchError> select(std::optional<IsaPath> requested,
                                                     IsaSet supported, IsaSet compiled) {
    return reference_select(requested, supported, compiled);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, SelectPathContract, ::testing::Types<ReferenceSelect>);

/// A probe that reports what this build compiled: true of the scalar build
/// everywhere, and a fixed answer whatever the machine.
class CompiledPathsProbe final : public simd::IsaProbe {
 public:
  [[nodiscard]] IsaSet supported() const override { return simd::compiled_paths(); }
};

struct ReferenceProbe {
#if defined(LOADFORGE_SIMD_SCALAR)
  static constexpr const char* kArchitecture = "other";
#elif defined(__x86_64__)
  static constexpr const char* kArchitecture = "x86_64";
#elif defined(__aarch64__)
  static constexpr const char* kArchitecture = "aarch64";
#else
  static constexpr const char* kArchitecture = "other";
#endif
  static std::unique_ptr<simd::IsaProbe> make() { return std::make_unique<CompiledPathsProbe>(); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, IsaProbeContract, ::testing::Types<ReferenceProbe>);

}  // namespace
}  // namespace loadforge::contracts
