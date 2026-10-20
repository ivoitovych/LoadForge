// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_RAYTRACE_GENERATOR_HPP
#define LOADFORGE_WORKLOADS_RAYTRACE_GENERATOR_HPP

// How a unit's input is drawn and read back (spec.hpp, "HOW THE INPUT IS
// GENERATED" and the input layout). Internal to the ray tracing workload.

#include <bit>
#include <cstdint>
#include <span>

#include "workloads/raytrace/scene.hpp"

namespace loadforge::workloads::raytrace {

// The input and the output are little-endian by the specification; the code
// copies doubles and words as they are held, which is that on every machine
// the project builds for.
static_assert(std::endian::native == std::endian::little);

/// Fills `input` with unit `index`'s scene under `seed`: draw e of the unit is
/// u64_of(block_at(seed, index, e)), decoded as spec.hpp says.
void generate_scene(std::uint64_t seed, std::uint64_t index,
                    std::span<std::byte, kInputBytes> input) noexcept;

/// Reads the spheres and triangles an input holds into `scene`. Any bytes are
/// an input: a NaN or an enormous value is a number like another.
void decode_scene(std::span<const std::byte, kInputBytes> input, SceneStorage& scene) noexcept;

}  // namespace loadforge::workloads::raytrace

#endif
