// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_FFT_TWIDDLE_TABLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_FFT_TWIDDLE_TABLE_HPP

// The oracle's own copy of the specification's twiddle table (src/workloads/fft/
// spec.hpp, "THE TWIDDLE TABLE"). The specification defines the table as a
// function of n alone and says it is generated offline with arbitrary-precision
// arithmetic, so the oracle holds a table of its own, generated from the
// definition by tests/fixtures/verification/oracle/fft/generate_twiddles.py and
// checked in tests/unit against the C library's long double sine and cosine.
// Nothing here is read from the workload's table or kernels.

#include <array>
#include <cstddef>
#include <cstdint>

namespace loadforge::verification::oracle::fft {

/// For m in [0, 4096): element 2 m holds the bits of Re W[m], element 2 m + 1
/// the bits of Im W[m].
inline constexpr std::size_t kTwiddleWords = 8192;
extern const std::array<std::uint64_t, kTwiddleWords> kTwiddleBits;

}  // namespace loadforge::verification::oracle::fft

#endif
