// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_STENCIL_STENCIL_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_STENCIL_STENCIL_ORACLE_HPP

// The independent oracle of the stencil workload (verification.md §3, §5),
// written from src/workloads/stencil/spec.hpp and from nothing else: four
// 16 x 16 windows of the grid are swept kSteps times by plain scalar code
// from the INPUT it is given, and compared cell by cell with the output it is
// given, each cell against the derived tolerance. Contract::kBounded.
//
// It allocates nothing: the input is read in place and the two buffers of a
// sweep are on the stack, so check() cannot fail to allocate scratch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/stencil/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::stencil {

/// The row and column of a window's first cell.
struct Origin {
  std::size_t row = 0;
  std::size_t column = 0;
};

/// The cells of one window, row-major.
using Window =
    std::array<double, workloads::stencil::kRegionEdge * workloads::stencil::kRegionEdge>;

/// The origins of the kRegions windows, in the order they are compared: the two
/// corners, then two that are a function of `index` alone. Every origin
/// coordinate is in [0, kEdge - kRegionEdge].
[[nodiscard]] std::array<Origin, workloads::stencil::kRegions> window_origins(
    std::uint64_t index) noexcept;

/// The window at `origin` after kSteps sweeps of the input grid (kEdge * kEdge
/// little-endian doubles), by sweeping the window grown by kSteps cells on
/// every side, clipped to the grid, with the ring held fixed.
[[nodiscard]] Window expected_window(std::span<const std::byte> input, Origin origin) noexcept;

/// T' for an input whose largest cell has magnitude `max_magnitude`:
/// 2 gamma_{4 kSteps} * max_magnitude * (1 + kEvaluationSlack).
[[nodiscard]] double cell_tolerance(double max_magnitude) noexcept;

/// The little-endian double at `element` of a span of doubles stored as bytes.
/// Precondition: (element + 1) * 8 <= bytes.size().
[[nodiscard]] double load_double(std::span<const std::byte> bytes, std::size_t element) noexcept;

class StencilOracle final : public Oracle {
 public:
  [[nodiscard]] std::string_view workload() const noexcept override;
  [[nodiscard]] Contract contract() const noexcept override;
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override;
};

/// The oracle for a plan the stencil workload resolved: kConfiguration when the
/// plan's sizes are not a unit of the specification or its decomposition is not
/// one of its tile edges. (The tile edge does not change what is checked; the
/// plan is still held to the specification.)
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle::stencil

#endif
