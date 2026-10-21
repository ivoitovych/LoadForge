// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_FFT_FFT_HPP
#define LOADFORGE_WORKLOADS_FFT_FFT_HPP

// The FFT workload (spec.hpp): a unit is the forward DFT of kPoints complex
// points. Created by create(), which the registry stream links by name.
//
//   decomposition  the radix: 2 or 4; 0 asks for kDefaultRadix.
//   isa            kScalar. The stage kernels are behind simd::KernelTable and
//                  the workload refuses a path its table has no kernel for.
//
// Class: BitExactFixedDecomposition. The same bits for one radix at any
// thread count and on any run: a stage's butterflies are independent, each
// writes its own outputs, and the stages are separated by the executor's
// barrier, so the split over threads cannot reach a result.

#include <memory>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::fft {

[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

}  // namespace loadforge::workloads::fft

#endif
