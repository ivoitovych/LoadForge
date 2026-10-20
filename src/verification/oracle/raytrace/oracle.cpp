// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/raytrace/oracle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "verification/record.hpp"
#include "workloads/raytrace/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::raytrace {
namespace {

namespace spec = workloads::raytrace;

constexpr std::string_view kWhat = "the rendered image against a brute-force trace";
constexpr std::size_t kSceneDoubles =
    spec::kSpheres * spec::kSphereDoubles + spec::kTriangles * spec::kTriangleDoubles;
constexpr std::size_t kSphereValues = spec::kSpheres * spec::kSphereDoubles;
constexpr std::size_t kSampleEdge = spec::kWidth / kSampleTiling;

using Item = std::array<std::byte, spec::kPixelBytes>;

struct Scene {
  std::array<spec::Sphere, spec::kSpheres> spheres;
  std::array<spec::Triangle, spec::kTriangles> triangles;
};

std::uint64_t load_le64(std::span<const std::byte> bytes, std::size_t at) noexcept {
  std::uint64_t value = 0;
  for (std::size_t k = 0; k < sizeof(value); ++k) {
    value |= std::to_integer<std::uint64_t>(bytes[at + k]) << (8U * k);
  }
  return value;
}

std::uint32_t load_le32(std::span<const std::byte> bytes, std::size_t at) noexcept {
  std::uint32_t value = 0;
  for (std::size_t k = 0; k < sizeof(value); ++k) {
    value |= std::to_integer<std::uint32_t>(bytes[at + k]) << (8U * k);
  }
  return value;
}

void store_le64(std::span<std::byte> bytes, std::size_t at, std::uint64_t value) noexcept {
  for (std::size_t k = 0; k < sizeof(value); ++k) {
    bytes[at + k] = static_cast<std::byte>((value >> (8U * k)) & 0xFFU);
  }
}

void store_le32(std::span<std::byte> bytes, std::size_t at, std::uint32_t value) noexcept {
  for (std::size_t k = 0; k < sizeof(value); ++k) {
    bytes[at + k] = static_cast<std::byte>((value >> (8U * k)) & 0xFFU);
  }
}

/// Where scene value `which` (0 .. kSceneDoubles - 1) of the input is allowed
/// to lie, by its place in the layout: spheres first, then triangles.
spec::Interval range_of(std::size_t which) noexcept {
  if (which < kSphereValues) {
    const std::size_t component = which % spec::kSphereDoubles;
    if (component < 2) {
      return spec::kSphereXy;
    }
    return component == 2 ? spec::kSphereZ : spec::kRadius;
  }
  const std::size_t component = (which - kSphereValues) % 3;
  return component < 2 ? spec::kVertexXy : spec::kVertexZ;
}

/// A description of scene value `which`, for the reason a check was not made.
std::string describe_value(std::size_t which) {
  if (which < kSphereValues) {
    constexpr std::array<std::string_view, spec::kSphereDoubles> kNames = {"centre x", "centre y",
                                                                           "centre z", "radius"};
    return "sphere " + std::to_string(which / spec::kSphereDoubles) + " " +
           std::string(kNames[which % spec::kSphereDoubles]);
  }
  const std::size_t offset = which - kSphereValues;
  constexpr std::array<std::string_view, 3> kAxes = {"x", "y", "z"};
  return "triangle " + std::to_string(offset / spec::kTriangleDoubles) + " vertex " +
         std::to_string(offset % spec::kTriangleDoubles / 3) + " " + std::string(kAxes[offset % 3]);
}

/// Reads the scene out of the input. The index of the first value that is not
/// finite or outside its range, if there is one (the scene is then partial).
std::optional<std::size_t> read_scene(std::span<const std::byte> input, Scene& scene) noexcept {
  std::array<double, kSceneDoubles> values{};
  for (std::size_t which = 0; which < kSceneDoubles; ++which) {
    values[which] = std::bit_cast<double>(load_le64(input, which * sizeof(double)));
    if (!spec::contains(range_of(which), values[which])) {
      return which;
    }
  }
  for (std::size_t i = 0; i < spec::kSpheres; ++i) {
    const std::size_t at = i * spec::kSphereDoubles;
    scene.spheres[i] =
        spec::Sphere{spec::Vec3{values[at], values[at + 1], values[at + 2]}, values[at + 3]};
  }
  for (std::size_t j = 0; j < spec::kTriangles; ++j) {
    const std::size_t at = kSphereValues + j * spec::kTriangleDoubles;
    scene.triangles[j] = spec::Triangle{spec::Vec3{values[at], values[at + 1], values[at + 2]},
                                        spec::Vec3{values[at + 3], values[at + 4], values[at + 5]},
                                        spec::Vec3{values[at + 6], values[at + 7], values[at + 8]}};
  }
  return std::nullopt;
}

/// The record of pixel p by brute force: every primitive tried in index order,
/// the nearest kept, a tie to the lower index (a later one must be STRICTLY
/// nearer to replace it).
Item brute_force_record(const Scene& scene, std::size_t p) noexcept {
  const spec::Ray ray = spec::pixel_ray(static_cast<std::uint32_t>(p % spec::kWidth),
                                        static_cast<std::uint32_t>(p / spec::kWidth));
  double nearest = spec::kNoHitDistance;
  std::uint32_t which = spec::kNoHit;
  for (std::size_t i = 0; i < spec::kSpheres; ++i) {
    const double t = spec::ray_sphere(ray, scene.spheres[i]);
    if (t < nearest) {
      nearest = t;
      which = static_cast<std::uint32_t>(i);
    }
  }
  for (std::size_t j = 0; j < spec::kTriangles; ++j) {
    const double t = spec::ray_triangle(ray, scene.triangles[j]);
    if (t < nearest) {
      nearest = t;
      which = static_cast<std::uint32_t>(spec::kSpheres + j);
    }
  }
  Item record{};
  store_le64(record, 0, std::bit_cast<std::uint64_t>(nearest));
  store_le32(record, 8, which);
  return record;
}

/// Whether the stored record of pixel p is one the specification allows.
bool well_formed(std::span<const std::byte> output, std::size_t p) noexcept {
  const std::size_t at = spec::pixel_offset(p);
  if (load_le32(output, at + 12) != 0) {
    return false;
  }
  const std::uint64_t bits = load_le64(output, at);
  const std::uint32_t which = load_le32(output, at + 8);
  if (which == spec::kNoHit) {
    return bits == spec::kNoHitDistanceBits;
  }
  const double t = std::bit_cast<double>(bits);
  return which < spec::kPrimitives && std::isfinite(t) && t > 0.0;
}

/// The sum of tile k's stored records, or the first malformed pixel of it.
struct TileSum {
  double sum = 0.0;
  std::optional<std::size_t> malformed;
};

TileSum sum_of_tile(std::span<const std::byte> output, std::uint32_t edge, std::size_t k) noexcept {
  const std::size_t per_row = spec::kWidth / edge;
  const std::size_t x0 = k % per_row * edge;
  const std::size_t y0 = k / per_row * edge;
  TileSum tile;
  for (std::size_t y = y0; y < y0 + edge; ++y) {
    for (std::size_t x = x0; x < x0 + edge; ++x) {
      const std::size_t p = y * spec::kWidth + x;
      if (!well_formed(output, p)) {
        tile.malformed = p;
        return tile;
      }
      if (load_le32(output, spec::pixel_offset(p) + 8) != spec::kNoHit) {
        tile.sum = tile.sum + std::bit_cast<double>(load_le64(output, spec::pixel_offset(p)));
      }
    }
  }
  return tile;
}

/// The difference between an item's expected bytes and the stored ones, with
/// offsets from the start of the output; none when they are equal.
/// `offset` is the item's place in the output, a multiple of kWordBytes.
std::optional<Difference> item_difference(std::span<const std::byte> expected,
                                          std::span<const std::byte> output, std::size_t offset) {
  const std::size_t size = expected.size();
  const std::span<const std::byte> actual = output.subspan(offset, size);
  std::size_t first = size;
  std::size_t last = 0;
  std::uint64_t popcount = 0;
  for (std::size_t at = 0; at < size; ++at) {
    const auto delta = std::to_integer<unsigned>(expected[at] ^ actual[at]);
    if (delta != 0) {
      first = std::min(first, at);
      last = at;
      popcount += static_cast<std::uint64_t>(std::popcount(delta));
    }
  }
  if (popcount == 0) {
    return std::nullopt;
  }
  Difference difference;
  difference.domain = Domain::kStored;
  difference.first_offset = offset + first;
  difference.last_offset = offset + last;
  difference.popcount = popcount;
  difference.window_offset = difference.first_offset / kWordBytes * kWordBytes;
  const auto from = static_cast<std::ptrdiff_t>(difference.window_offset - offset);
  difference.expected.assign(expected.begin() + from, expected.end());
  difference.actual.assign(actual.begin() + from, actual.end());
  return difference;
}

/// splitmix64's finaliser: a fixed, well-mixed function of one word.
std::uint64_t mix(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

std::string numbered(std::string_view check, std::size_t number) {
  std::string text(check);
  text += ' ';
  text += std::to_string(number);
  return text;
}

OracleError refused(std::string detail) {
  return OracleError{OracleFault::kConfiguration, std::move(detail)};
}

}  // namespace

std::size_t sampled_pixel(std::uint64_t index, std::size_t j) noexcept {
  const std::uint64_t bits = mix(mix(index) + j);
  const std::uint64_t dx = bits % kSampleEdge;
  const std::uint64_t dy = bits / kSampleEdge % kSampleEdge;
  const std::size_t x = j % kSampleTiling * kSampleEdge + dx;
  const std::size_t y = j / kSampleTiling * kSampleEdge + dy;
  return y * spec::kWidth + x;
}

RaytraceOracle::RaytraceOracle(std::uint32_t tile_edge) noexcept : tile_edge_(tile_edge) {}

std::string_view RaytraceOracle::workload() const noexcept { return kWorkload; }

Contract RaytraceOracle::contract() const noexcept { return Contract::kExact; }

Verdict RaytraceOracle::check(const UnitEvidence& evidence) const {
  if (evidence.input.size() != spec::kInputBytes || evidence.output.size() != spec::kOutputBytes) {
    std::string why = "the evidence is ";
    why += std::to_string(evidence.input.size());
    why += " bytes of input and ";
    why += std::to_string(evidence.output.size());
    why += " of output; a ray tracing unit is ";
    why += std::to_string(spec::kInputBytes);
    why += " and ";
    why += std::to_string(spec::kOutputBytes);
    return NotChecked{std::string(kWhat), std::move(why), 0};
  }
  Scene scene;
  if (const auto bad = read_scene(evidence.input, scene)) {
    std::string why = "the input's ";
    why += describe_value(*bad);
    why += " is not finite or is outside the range the specification generates";
    return NotChecked{std::string(kWhat), std::move(why), 0};
  }
  const std::span<const std::byte> output = evidence.output;
  std::uint64_t compared = 0;
  for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
    ++compared;
    const std::size_t p = sampled_pixel(evidence.index, j);
    const Item expected = brute_force_record(scene, p);
    if (auto difference = item_difference(expected, output, spec::pixel_offset(p))) {
      return ExactMismatch{numbered(spec::kCheckPixel, p), std::move(difference), compared};
    }
  }
  for (std::size_t k = 0; k < spec::tile_count(tile_edge_); ++k) {
    ++compared;
    const TileSum tile = sum_of_tile(output, tile_edge_, k);
    if (tile.malformed.has_value()) {
      std::string check = numbered(spec::kCheckTileSum, k);
      check += ": the stored record of pixel ";
      check += std::to_string(*tile.malformed);
      check += " is not well-formed";
      return ExactMismatch{std::move(check), std::nullopt, compared};
    }
    std::array<std::byte, sizeof(double)> expected{};
    store_le64(expected, 0, std::bit_cast<std::uint64_t>(tile.sum));
    if (auto difference = item_difference(expected, output, spec::tile_sum_offset(k))) {
      return ExactMismatch{numbered(spec::kCheckTileSum, k), std::move(difference), compared};
    }
  }
  return Agreement{compared};
}

core::Result<std::unique_ptr<Oracle>, OracleError> create(const workloads::WorkloadConfig& config,
                                                          const workloads::Plan& plan) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    return refused("a ray tracing unit is " + std::to_string(spec::kInputBytes) + " bytes in and " +
                   std::to_string(spec::kOutputBytes) + " out; the plan says " +
                   std::to_string(plan.input_bytes) + " and " + std::to_string(plan.output_bytes));
  }
  const std::uint32_t resolved =
      config.decomposition == 0 ? spec::kDefaultTileEdge : config.decomposition;
  if (!spec::is_tile_edge(resolved) || plan.decomposition != resolved) {
    return refused("the configured decomposition " + std::to_string(config.decomposition) +
                   " resolves to " + std::to_string(resolved) +
                   ", which must be a tile edge (8, 16 or 32) and the plan's decomposition, " +
                   std::to_string(plan.decomposition));
  }
  return std::unique_ptr<Oracle>(std::make_unique<RaytraceOracle>(resolved));
}

}  // namespace loadforge::verification::oracle::raytrace
