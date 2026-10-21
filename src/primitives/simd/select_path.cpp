// SPDX-License-Identifier: GPL-3.0-or-later
#include <optional>
#include <string>

#include "primitives/simd/dispatch.hpp"

namespace loadforge::simd {
namespace {

/// The paths in a set, widest first, for a person: "avx2, scalar".
std::string list_paths(IsaSet set) {
  std::string text;
  for (const IsaPath path : kPreference) {
    if (set.contains(path)) {
      if (!text.empty()) {
        text += ", ";
      }
      text += describe(path);
    }
  }
  if (text.empty()) {
    text = "none";
  }
  return text;
}

}  // namespace

core::Result<IsaPath, DispatchError> select_path(std::optional<IsaPath> requested, IsaSet supported,
                                                 IsaSet compiled) {
  if (requested.has_value()) {
    const IsaPath path = *requested;
    if (path == IsaPath::kScalar) {
      return path;
    }
    if (!compiled.contains(path)) {
      return DispatchError{DispatchFault::kNotCompiled, path,
                           std::string(describe(path)) +
                               " was requested but this build holds no kernels for it (compiled: " +
                               list_paths(compiled) + ")"};
    }
    if (!supported.contains(path)) {
      return DispatchError{DispatchFault::kNotSupported, path,
                           std::string(describe(path)) +
                               " was requested but this machine cannot run it (supported: " +
                               list_paths(supported) + ")"};
    }
    return path;
  }
  const IsaSet usable = supported.intersect(compiled);
  for (const IsaPath path : kPreference) {
    if (usable.contains(path)) {
      return path;
    }
  }
  return IsaPath::kScalar;
}

}  // namespace loadforge::simd
