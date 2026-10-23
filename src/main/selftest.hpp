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
/// It is `workloads_digest(corpus_digest())`: the two halves are separate functions so that
/// `loadforge selftest` can compare each with its own expected value and say which differed.
///
/// The corpus grows with the project. At M0 it covers configuration parsing;
/// as workloads land, their BitExact results join it.
/// Not noexcept: it builds std::strings via to_string, so an allocation failure
/// should propagate rather than terminate. The earlier noexcept was accidental.
[[nodiscard]] std::uint64_t core_digest();

/// The FNV-1a digest over the configuration-parsing corpus alone: the first half of
/// core_digest(), before any workload is folded in.
[[nodiscard]] std::uint64_t corpus_digest();

/// `basis` folded with a digest over the SCALAR paths of the compression and dense-linear
/// workloads: two units of each, generated and computed from a fixed seed on
/// the calling thread, their outputs folded in. The scalar path is the one that
/// determinism.md §3 holds bit-identical on x86-64 and ARM64, whatever the
/// build, so this is the digest that catches a vector kernel, a contraction or
/// a library that moved a bit between the two. A path other than scalar is
/// never used here, even where the machine has one.
[[nodiscard]] std::uint64_t workloads_digest(std::uint64_t basis);

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

/// The digests this source tree is expected to produce, committed here so that they are
/// independent of the code that computes them: a build that computes something else is
/// broken, or the machine running it is (`loadforge selftest`, docs/verification.md §9).
/// They cannot drift silently: tests/unit/main/selftest_test.cpp recomputes both and fails,
/// naming the new values, when the corpus or a scalar kernel changes on purpose. Regenerate
/// with `loadforge --selftest-digest` (the final value) and the failing test's message (the
/// corpus value). CI compares the final value across x86-64 and ARM64 as before.
///
///   kExpectedCorpusDigest    corpus_digest().
///   kExpectedDigest          core_digest(), which is workloads_digest(kExpectedCorpusDigest).
inline constexpr std::uint64_t kExpectedCorpusDigest = 0xa154e111368c537bULL;
inline constexpr std::uint64_t kExpectedDigest = 0xe0ff849ff34906deULL;

/// The canonical 64-bit FNV-1a parameters, 0xcbf29ce484222325 and
/// 0x100000001b3.
inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

}  // namespace loadforge::selftest

#endif
