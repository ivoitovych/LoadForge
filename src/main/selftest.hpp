// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_SELFTEST_HPP
#define LOADFORGE_MAIN_SELFTEST_HPP

#include <cstdint>
#include <string>

namespace loadforge::selftest {

/// A deterministic digest over a fixed corpus of core operations.
///
/// Purpose: cross-architecture determinism checking. The scalar build on
/// x86-64 and on ARM64 must produce byte-identical digests, which
/// docs/determinism.md requires of every class except BoundedResidual. CI
/// computes this on both architectures and compares; a mismatch means the two
/// disagree about something they are contractually obliged to agree on.
///
/// The corpus grows with the project. At M0 it covers configuration parsing;
/// as workloads land, their BitExact results join it.
[[nodiscard]] std::uint64_t core_digest() noexcept;

/// The digest rendered as fixed-width hex, for CI to diff.
[[nodiscard]] std::string core_digest_hex();

}  // namespace loadforge::selftest

#endif
