// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for the adaptive mesh workload: corruption injected where memory would
// corrupt it, and what reaches the output. The workload performs no
// verification (the oracle stream's does); what it owes the doctrine
// (docs/verification.md §4a, §6) is that it neither hides a fault nor spreads
// one beyond where the arithmetic carries it, so that the bounded check sees
// the fault and the error record can name where it is. Each test injects one
// fault and asserts what reaches the output and whether the specification's
// checks (leaf count exact, integral within the derived tolerance) see it:
//
//   * a hotspot's coordinate corrupted to one outside the lattice removes the
//     hotspot: the leaf count of its cell falls, which the exact count check
//     sees against the regenerated input -- while the same output checked
//     against the CORRUPTED input passes: the corruption has redefined the
//     problem, which is why the input bracket exists (§4a);
//   * a low-order bit of a coordinate moves the hotspot to a neighbouring
//     finest cell, which changes the integrals around it (and the counts only
//     if it crosses a split boundary);
//   * a count flipped in the output after compute is seen as any difference;
//   * an integral with a high bit flipped, a NaN or an infinity is seen; one
//     with the lowest mantissa bit flipped is below the tolerance of a cell
//     of many leaves, where a bounded contract cannot see a flip and an exact
//     check of the input is what catches the faults it misses.
//   * compute() is total: inputs of all zero, all ones, noise and the
//     alignment-hostile bit patterns give the mesh of the hotspots they
//     decode to, checked against the reference, without a crash.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "primitives/rng/philox.hpp"
#include "unit/workloads/adaptive_mesh/support.hpp"

namespace loadforge::workloads::adaptive_mesh::testing {
namespace {

struct Fixture {
  std::unique_ptr<Workload> workload = make(config_for(4, 8));
  Serial serial;

  Bytes run(const Bytes& input) { return computed(*workload, 1, input, serial); }
};

/// One hotspot in the lattice, in coarse cell 9 (cx 9, cy 0), and the rest outside it.
Hotspots lone_hotspot() {
  Hotspots spots(kHotspots, Hotspot{kLattice, kLattice});
  spots[100] = {(9 * 32) + 7, 11};
  return spots;
}

TEST(AdaptiveMeshFault, ACoordinateCorruptedOutOfTheLatticeLowersItsCellsCount) {
  Fixture f;
  ASSERT_NE(f.workload, nullptr);
  const Hotspots spots = lone_hotspot();
  const Bytes clean_input = input_of(spots);
  const Bytes clean = f.run(clean_input);
  ASSERT_EQ(count_of(clean, 9), 16U);

  Bytes corrupt_input = clean_input;
  corrupt_input.at((100 * kHotspotBytes) + 1) ^= std::byte{0x80};  // bit 15 of x
  const Bytes corrupt = f.run(corrupt_input);
  EXPECT_EQ(count_of(corrupt, 9), 1U);
  EXPECT_EQ(first_disagreement(corrupt, spots), "leaf count of cell 9")
      << "against the regenerated input the exact count check names the cell";
  EXPECT_EQ(first_disagreement(corrupt, hotspots_of(corrupt_input)), "")
      << "against the corrupted input the output is correct: the input bracket's job";
  for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
    if (cell != 9) {
      EXPECT_EQ(count_of(corrupt, cell), count_of(clean, cell));
    }
  }
}

TEST(AdaptiveMeshFault, ALowBitOfACoordinateMovesTheHotspotAndChangesNearbyIntegrals) {
  Fixture f;
  ASSERT_NE(f.workload, nullptr);
  const Hotspots spots = lone_hotspot();
  const Bytes clean_input = input_of(spots);
  const Bytes clean = f.run(clean_input);
  Bytes corrupt_input = clean_input;
  corrupt_input.at(100 * kHotspotBytes) ^= std::byte{0x01};  // x = 295 -> 294
  const Bytes corrupt = f.run(corrupt_input);
  // Same cell, same path: the count is unchanged and the integral is not.
  EXPECT_EQ(count_of(corrupt, 9), count_of(clean, 9));
  EXPECT_NE(integral_of(corrupt, 9), integral_of(clean, 9));
  const CellReference r = reference_cell(9, spots);
  EXPECT_FALSE(integral_within(corrupt, 9, r)) << "the bounded check sees a moved hotspot";
}

TEST(AdaptiveMeshFault, CorruptionOfTheOutputAfterComputeIsSeenWhereTheChecksCanSeeIt) {
  Fixture f;
  ASSERT_NE(f.workload, nullptr);
  // Many leaves in cell 0: the cell where the tolerance is widest.
  Hotspots spots;
  for (std::uint32_t y = 0; y < 32; y += 2) {
    for (std::uint32_t x = 0; x < 32; x += 2) {
      spots.push_back({x, y});
    }
  }
  const Bytes output = f.run(input_of(spots));
  ASSERT_EQ(first_disagreement(output, spots), "");
  const CellReference r = reference_cell(0, spots);

  const auto flip = [&](unsigned bit) {
    Bytes corrupt = output;
    put_double(corrupt, integral_offset(0),
               std::bit_cast<double>(std::bit_cast<std::uint64_t>(integral_of(output, 0)) ^
                                     (std::uint64_t{1} << bit)));
    return corrupt;
  };
  EXPECT_TRUE(integral_within(flip(0), 0, r)) << "the lowest mantissa bit: below the tolerance";
  for (const unsigned bit : {30U, 45U, 52U, 62U, 63U}) {
    EXPECT_FALSE(integral_within(flip(bit), 0, r)) << "bit " << bit;
  }
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    Bytes corrupt = output;
    put_double(corrupt, integral_offset(0), bad);
    EXPECT_FALSE(integral_within(corrupt, 0, r));
  }
  Bytes counted = output;
  put_u32(counted, count_offset(0), count_of(output, 0) ^ 1U);
  EXPECT_EQ(first_disagreement(counted, spots), "leaf count of cell 0");
}

TEST(AdaptiveMeshFault, ComputeIsTotalAndGivesTheMeshOfTheHotspotsTheBytesDecodeTo) {
  Fixture f;
  ASSERT_NE(f.workload, nullptr);
  Bytes noise(kInputBytes);
  rng::fill_stream(0xBAD, 1, noise);
  Bytes mixed = generated(*f.workload, 3);
  for (std::size_t at = 0; at < mixed.size(); at += 5) {
    mixed[at] ^= std::byte{0xFF};
  }
  const Bytes inputs[] = {Bytes(kInputBytes, std::byte{0x00}), Bytes(kInputBytes, std::byte{0xFF}),
                          Bytes(kInputBytes, std::byte{0x7F}), noise, mixed};
  for (const Bytes& input : inputs) {
    const Bytes output = f.run(input);
    EXPECT_EQ(first_disagreement(output, hotspots_of(input)), "");
  }
}

}  // namespace
}  // namespace loadforge::workloads::adaptive_mesh::testing
