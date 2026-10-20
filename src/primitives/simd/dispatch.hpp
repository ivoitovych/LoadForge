// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_SIMD_DISPATCH_HPP
#define LOADFORGE_PRIMITIVES_SIMD_DISPATCH_HPP

// CONTRACT (ADR 0004). The one seam through which a vectorised kernel is
// chosen (docs/PLAN.md §4.5, docs/determinism.md §3, IMPLEMENTATION M2b).
// Frozen: changed only by a contract change, never inside a stream.
//
//   Provider of select_path() and of an IsaProbe:  the numerics stream
//                                                   (src/platform/isa/).
//   Consumers: every workload with vector kernels (through KernelTable and
//   the IsaPath its WorkloadConfig names), the worker that selects the path,
//   and the results and error records that report it.
//
// THE RULES THIS SEAM EXISTS TO KEEP
// ----------------------------------
//   * The scalar path is always present: in every build, in every table, and
//     selectable on every machine. It is the reference every other path is
//     compared against, and the one path that is bit-identical across x86-64
//     and ARM64 (determinism.md §3).
//   * The path is chosen ONCE, before a workload is created, and recorded --
//     in the run fingerprint and in every error record -- because a
//     floating-point result is only interpretable with the path that made it.
//     A workload never re-selects mid-run.
//   * A path the user asked for is used or refused, never silently replaced:
//     a run that asked for AVX2 and ran scalar would report numbers for a
//     configuration nobody chose.
//   * Kernels that use an ISA's intrinsics live under src/primitives/simd/x86/
//     or src/primitives/simd/arm/; the portability workflow refuses x86
//     intrinsics anywhere else.
//
// THREAD-SAFETY: every type here is a value type; every function is pure
// except IsaProbe::supported(), whose thread-safety is stated on it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "core/result.hpp"

namespace loadforge::simd {

/// The instruction-set paths a kernel can be built for. The numeric values are
/// the wire and file encoding (error records, run.json) and never change; a new
/// path takes a new number.
///
///   kScalar  portable C++, no intrinsics. Always present.
///   kAvx2    x86-64 AVX2 with FMA3 (Haswell and later, Zen and later).
///   kAvx512  x86-64 AVX-512 Foundation.
///   kNeon    AArch64 Advanced SIMD. SVE is deliberately absent (PLAN §4.5).
enum class IsaPath : std::uint8_t { kScalar = 0, kAvx2 = 1, kAvx512 = 2, kNeon = 3 };

inline constexpr std::size_t kIsaPathCount = 4;

/// The name of a path, as configuration, reports and records spell it.
[[nodiscard]] constexpr std::string_view describe(IsaPath path) noexcept {
  switch (path) {
    case IsaPath::kScalar:
      return "scalar";
    case IsaPath::kAvx2:
      return "avx2";
    case IsaPath::kAvx512:
      return "avx512";
    case IsaPath::kNeon:
      return "neon";
  }
  return "unrecognised ISA path";
}

/// The inverse of describe(): exactly those four words, nothing else.
[[nodiscard]] constexpr std::optional<IsaPath> parse_isa_path(std::string_view text) noexcept {
  for (const IsaPath path : {IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512, IsaPath::kNeon}) {
    if (describe(path) == text) {
      return path;
    }
  }
  return std::nullopt;
}

/// A set of paths. A value type: nothing here allocates or fails.
class IsaSet {
 public:
  constexpr IsaSet() noexcept = default;
  constexpr IsaSet(std::initializer_list<IsaPath> paths) noexcept {
    for (const IsaPath path : paths) {
      bits_ = static_cast<std::uint8_t>(bits_ | bit(path));
    }
  }

  /// Whether `path` is in the set. A value outside the enumeration is in no set.
  [[nodiscard]] constexpr bool contains(IsaPath path) const noexcept {
    return (bits_ & bit(path)) != 0U;
  }
  [[nodiscard]] constexpr IsaSet with(IsaPath path) const noexcept {
    return IsaSet(static_cast<std::uint8_t>(bits_ | bit(path)));
  }
  [[nodiscard]] constexpr IsaSet intersect(IsaSet other) const noexcept {
    return IsaSet(static_cast<std::uint8_t>(bits_ & other.bits_));
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0U; }

  [[nodiscard]] friend constexpr bool operator==(IsaSet, IsaSet) noexcept = default;

 private:
  constexpr explicit IsaSet(std::uint8_t bits) noexcept : bits_(bits) {}
  [[nodiscard]] static constexpr std::uint8_t bit(IsaPath path) noexcept {
    const auto index = static_cast<std::uint8_t>(path);
    return index < kIsaPathCount ? static_cast<std::uint8_t>(1U << index) : std::uint8_t{0};
  }
  std::uint8_t bits_ = 0;
};

/// The preference order when no path is requested: the widest first. Scalar
/// is last and always eligible.
inline constexpr std::array<IsaPath, kIsaPathCount> kPreference = {
    IsaPath::kAvx512, IsaPath::kAvx2, IsaPath::kNeon, IsaPath::kScalar};

/// The paths this build compiled kernels for: scalar always; the native
/// architecture's paths unless the build is LOADFORGE_SIMD=scalar, which
/// exists to prove portable code needs none of them (PLAN §4.5).
[[nodiscard]] constexpr IsaSet compiled_paths() noexcept {
#if defined(LOADFORGE_SIMD_SCALAR)
  return IsaSet{IsaPath::kScalar};
#elif defined(__x86_64__)
  return IsaSet{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512};
#elif defined(__aarch64__)
  return IsaSet{IsaPath::kScalar, IsaPath::kNeon};
#else
  return IsaSet{IsaPath::kScalar};
#endif
}

/// Why a path could not be selected.
///
///   kNotCompiled   this build holds no kernels for the requested path: an
///                  ARM binary asked for AVX2, or a scalar-only build for
///                  anything but scalar.
///   kNotSupported  the build has it and this machine (CPU or kernel) does
///                  not: AVX-512 on a CPU without it, or with the OS not
///                  saving its registers.
enum class DispatchFault : std::uint8_t { kNotCompiled, kNotSupported };

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct DispatchError {
  DispatchFault fault = DispatchFault::kNotCompiled;
  IsaPath requested = IsaPath::kScalar;
  std::string detail;  ///< For a person: which path, and what this build or machine has.

  [[nodiscard]] friend bool operator==(const DispatchError&, const DispatchError&) = default;
};

/// What the running CPU and kernel can execute.
///
/// supported() always contains kScalar, never contains a path of another
/// architecture, and gives the same answer on every call in a process. On
/// x86-64 kAvx2 requires the CPU to report AVX2 and FMA3 AND the OS to save
/// the YMM state (XCR0 bits 1 and 2); kAvx512 requires AVX512F AND the OS to
/// save opmask and ZMM state (XCR0 bits 5 to 7). On AArch64 kNeon requires the
/// kernel to report Advanced SIMD (HWCAP_ASIMD). A feature the probe cannot
/// read is not supported: the probe never guesses upward.
///
/// Thread-safety: supported() may be called from any thread.
class IsaProbe {
 public:
  IsaProbe() = default;
  IsaProbe(const IsaProbe&) = delete;
  IsaProbe(IsaProbe&&) = delete;
  IsaProbe& operator=(const IsaProbe&) = delete;
  IsaProbe& operator=(IsaProbe&&) = delete;
  virtual ~IsaProbe() = default;

  [[nodiscard]] virtual IsaSet supported() const = 0;
};

/// Chooses the path a run uses. Provided by the numerics stream.
///
/// With a request: the requested path if `compiled` holds it and `supported`
/// holds it; kNotCompiled if `compiled` does not; kNotSupported if `compiled`
/// does and `supported` does not. kScalar is never refused, whatever the sets
/// say. Without a request: the first path in kPreference that both sets hold,
/// and kScalar when none does -- so the unrequested call never fails.
[[nodiscard]] core::Result<IsaPath, DispatchError> select_path(std::optional<IsaPath> requested,
                                                               IsaSet supported, IsaSet compiled);

/// One kernel per path, for one operation. `Fn` is a function-pointer type.
///
/// The scalar kernel is required -- a table without one breaks this contract,
/// and kernel_paths() omits kScalar for it so that the breach is visible -- and
/// the others are optional. A workload checks, when it is created, that its
/// table has a kernel for the path its configuration names, and refuses the
/// configuration otherwise (WorkloadFault::kConfiguration).
template <typename Fn>
struct KernelTable {
  static_assert(std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>,
                "a kernel table holds function pointers");

  Fn scalar = nullptr;
  Fn avx2 = nullptr;
  Fn avx512 = nullptr;
  Fn neon = nullptr;
};

/// The kernel for `path`, or nullptr when the table has none.
template <typename Fn>
[[nodiscard]] constexpr Fn kernel_at(const KernelTable<Fn>& table, IsaPath path) noexcept {
  switch (path) {
    case IsaPath::kScalar:
      return table.scalar;
    case IsaPath::kAvx2:
      return table.avx2;
    case IsaPath::kAvx512:
      return table.avx512;
    case IsaPath::kNeon:
      return table.neon;
  }
  return nullptr;
}

/// The paths the table has a kernel for.
template <typename Fn>
[[nodiscard]] constexpr IsaSet kernel_paths(const KernelTable<Fn>& table) noexcept {
  IsaSet set;
  for (const IsaPath path : {IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512, IsaPath::kNeon}) {
    if (kernel_at(table, path) != nullptr) {
      set = set.with(path);
    }
  }
  return set;
}

}  // namespace loadforge::simd

#endif
