// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_RAYTRACE_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_RAYTRACE_ORACLE_HPP

// The ray tracing workload's independent oracle (verification.md §3): the
// unit's output, as workloads/raytrace/spec.hpp lays it out, judged against
// the scene in the input by trying EVERY primitive, in index order, for each
// sampled pixel, and by recomputing each tile sum from the stored pixel
// records (oracle.hpp). There is no hierarchy here, and nothing of the
// workload: the arithmetic comes from the specification's own helpers and the
// algorithm is the brute force the specification names.
//
// WHAT IT ANSWERS, IN ORDER
// -------------------------
//   1. evidence that is not kInputBytes in and kOutputBytes out: NotChecked.
//   2. a scene value that is not finite or lies outside its range: NotChecked.
//   3. the kSampledPixels sampled pixels, in order of sample, each against the
//      brute-force record; then the tile sums of tiles 0 .. tile_count(edge)
//      - 1, each against the sum of the stored records. The first item that
//      fails is an ExactMismatch (Domain::kStored, offsets from the start of
//      the output) and ends the check; blocks_compared counts the items
//      evaluated, the failing one included. All items right: an Agreement of
//      kSampledPixels + tile_count(edge).
//
// WHICH PIXEL SAMPLE j IS
// -----------------------
// The specification fixes the tile of sample j (tile j of the 8 x 8 tiling)
// and leaves the pixel inside it to the oracle: sampled_pixel() picks it by a
// mix of the unit index and j, so a re-run checks the same pixels and
// different units check different ones (journal 32, "Contract questions").
//
// The slots of the tile sums at and after tile_count(edge) are not items of
// the specification and are not looked at.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/raytrace/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::raytrace {

/// The name of the workload this oracle checks.
inline constexpr std::string_view kWorkload = "raytrace";

/// The edge, in pixels, of the tiling the samples are spread over: 8 x 8 tiles.
inline constexpr std::size_t kSampleTiling = 8;

/// The pixel (y * kWidth + x) sample `j` of unit `index` checks, j below
/// kSampledPixels: a pixel inside tile j of the 8 x 8 tiling.
[[nodiscard]] std::size_t sampled_pixel(std::uint64_t index, std::size_t j) noexcept;

class RaytraceOracle final : public Oracle {
 public:
  // Defined out of line, every one: an inline member used from several
  // translation units leaves one copy's counters at zero (journal §1.22).
  /// Precondition: workloads::raytrace::is_tile_edge(tile_edge); create()
  /// is the checked way to get one.
  explicit RaytraceOracle(std::uint32_t tile_edge) noexcept;

  [[nodiscard]] std::string_view workload() const noexcept override;
  [[nodiscard]] Contract contract() const noexcept override;
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override;

 private:
  std::uint32_t tile_edge_;
};

/// The oracle for units of `plan`, the plan the ray tracing workload resolved
/// from `config`; kConfiguration when the two do not describe a ray tracing
/// unit: input or output sizes other than the specification's, a resolved
/// decomposition that is not a tile edge, or one that is not what the
/// configured one resolves to (a configured 0 resolves to kDefaultTileEdge;
/// any other value must be a tile edge and is its own resolution). The
/// registry's create() for "raytrace" calls this.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle::raytrace

#endif
