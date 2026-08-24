// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_SELFTEST_HPP
#define LOADFORGE_MAIN_SELFTEST_HPP

#include <cstdint>
#include <string>
#include <string_view>

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
/// Not noexcept: it builds std::strings via to_string, so an allocation failure
/// should propagate rather than terminate. The earlier noexcept was accidental.
[[nodiscard]] std::uint64_t core_digest();

/// The digest rendered as fixed-width hex, for CI to diff.
[[nodiscard]] std::string core_digest_hex();

/// FNV-1a, 64-bit, over `text` starting from `basis`.
///
/// Exposed so it can be checked against *published* test vectors rather than
/// against itself. The first version of this file declared an offset basis of
/// 1469598103934665603 -- nineteen digits, not the correct twenty -- and every
/// test passed, because they were all self-consistency tests and one of them
/// embedded the same wrong constant. This project's own doctrine
/// (docs/verification.md §2) says never to verify an implementation by
/// re-running it; the digest did exactly that until an outside reviewer noticed.
[[nodiscard]] std::uint64_t fnv1a(std::string_view text, std::uint64_t basis) noexcept;

/// The canonical 64-bit FNV-1a parameters, 0xcbf29ce484222325 and
/// 0x100000001b3.
inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

}  // namespace loadforge::selftest

#endif
