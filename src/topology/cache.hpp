// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TOPOLOGY_CACHE_HPP
#define LOADFORGE_TOPOLOGY_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cpu_list.hpp"
#include "topology/cpu_topology.hpp"

namespace loadforge::topology {

/// What a cache holds. The three values `cache/indexN/type` ever contains.
enum class CacheType : std::uint8_t { kData, kInstruction, kUnified };

/// The word for a CacheType, and the exact text sysfs writes for it.
[[nodiscard]] std::string_view describe(CacheType type) noexcept;

/// Which cache this is, across the whole machine.
///
/// All three parts are needed. `id` is unique only within a level -- every
/// level numbers its caches from zero -- and a level holds both a data and an
/// instruction cache with the SAME id on nearly every machine. Verified: this
/// kernel reports L1 Data id=0 and L1 Instruction id=0 for cpu0, two different
/// caches that an identity of (level, id) alone would merge into one.
struct CacheId {
  std::uint32_t level = 0;
  CacheType type = CacheType::kUnified;
  std::uint32_t id = 0;

  [[nodiscard]] friend bool operator==(const CacheId&, const CacheId&) = default;
};

/// One cache, as the kernel describes it.
struct Cache {
  CacheId identity;
  std::uint64_t size_bytes = 0;
  std::uint32_t line_bytes = 0;
  /// Every CPU this cache serves. Includes the CPU whose directory it was read
  /// from, and is the same set no matter which of them you read it through.
  CpuList shared_with;

  [[nodiscard]] friend bool operator==(const Cache&, const Cache&) = default;
};

/// The machine's caches, deduplicated and cross-checked.
///
/// WHY THE SAME CACHE IS READ SEVERAL TIMES
/// ----------------------------------------
/// sysfs publishes a cache once per CPU that can see it: a shared L3 appears in
/// four CPUs' `cache/` directories on a four-CPU machine, describing ONE piece
/// of silicon. Deduplicating on `(level, type, id)` is what turns four readings
/// back into one cache -- and reading all four first, rather than taking the
/// first and moving on, is what makes them CHECKABLE against each other.
///
/// Verified on the machine this was written against: `cache/index3/id` reads 0
/// from all four CPUs, while `index0`..`index2` (L1d, L1i, L2) each report an
/// id equal to the CPU's own number.
///
/// WHAT IS CHECKED, AND WHAT DELIBERATELY IS NOT
/// --------------------------------------------
/// Checked, because each is certain of any machine:
///
///   * a cache's shared set contains the CPU it was read through;
///   * every CPU in that set is one the CPU topology found online;
///   * every CPU sharing a cache publishes an IDENTICAL record for it --
///     same size, same line size, same shared set. Sharing a cache is an
///     equivalence relation, exactly as sharing a core is;
///   * sharing only ever widens with level: the CPUs sharing a CPU's L1 are a
///     subset of those sharing its L2, and so on. That is the defining
///     structural property of a cache hierarchy.
///
/// NOT checked, and the omissions are deliberate rather than oversights:
///
///   * that every CPU reports the same set of levels. False on a heterogeneous
///     machine -- an arm64 big.LITTLE package can genuinely give its big and
///     little cores different cache depths -- and a check that refuses a real
///     machine is worse than no check at all.
///   * that sizes grow with level. Nearly always true and not guaranteed:
///     L1 instruction and L1 data caches differ in size at the SAME level
///     (32K and 48K here), and nothing forbids an unusual arrangement further
///     up. The nesting check above already catches the corruption this would.
class CacheHierarchy {
 public:
  /// Highest `cache/indexN` examined before giving up.
  ///
  /// The scan walks index0, index1, ... and stops at the first one that is
  /// absent, because sysfs numbers these contiguously from zero. The bound
  /// exists so that a tree which is NOT contiguous -- a directory of thousands
  /// of stray entries, a path that resolves somewhere unexpected -- cannot turn
  /// discovery into an unbounded walk. Real machines use four or five.
  static constexpr std::uint32_t kMaxCacheIndex = 16;

  /// Read and cross-check the machine's caches.
  ///
  /// Takes the CPU topology rather than re-reading `online`, so that the two
  /// halves of discovery cannot disagree about which CPUs exist -- and so that
  /// a cache naming a CPU the topology never saw is a contradiction this can
  /// actually detect.
  [[nodiscard]] static core::Result<CacheHierarchy, DiscoveryFailure> discover(
      platform::FileSystem& filesystem, const CpuTopology& cpus,
      std::string_view sysfs_root = kDefaultSysfsRoot);

  /// Distinct caches, ordered by level and then by id.
  [[nodiscard]] const std::vector<Cache>& caches() const noexcept { return caches_; }

  [[nodiscard]] std::size_t count() const noexcept { return caches_.size(); }

  /// How many distinct caches exist at a level. Zero if the level is absent,
  /// which is a real answer: not every machine has an L3.
  [[nodiscard]] std::size_t count_at_level(std::uint32_t level) const noexcept;

  /// The deepest level any cache reports, or zero when there are none.
  [[nodiscard]] std::uint32_t deepest_level() const noexcept;

  /// Total bytes across every distinct cache at a level, counted once each
  /// however many CPUs share them.
  [[nodiscard]] std::uint64_t total_bytes_at_level(std::uint32_t level) const noexcept;

 private:
  explicit CacheHierarchy(std::vector<Cache> caches) : caches_(std::move(caches)) {}

  std::vector<Cache> caches_;
};

/// Parses the size format sysfs writes for a cache, in bytes.
///
/// Exposed because it is where a judgement lives, and a format decision that
/// can only be reached through a directory walk is one nobody will keep correct.
///
/// This is NOT core::parse_byte_size, and the difference is not an oversight.
/// That function parses a value a USER wrote in a configuration file: it accepts
/// `KiB`/`MiB`/`GiB` and, deliberately, a percentage like `70%`. sysfs writes
/// neither -- it writes a bare `K`, which that parser rejects as an unknown unit
/// (verified: "48K" returns "unit suffix is not recognised"). Pointing the
/// config parser at a cache size would therefore fail on every real machine, and
/// teaching it `K` would additionally make `70%` a parseable cache size, which
/// is not a thing.
///
/// `K` means 1024, as everywhere in the kernel. A bare number is bytes, and `B`
/// is accepted because the kernel's own formatter switches to it below 1 KiB;
/// every level on every machine probed so far reports `K`.
[[nodiscard]] core::Result<std::uint64_t, std::string_view> parse_cache_size(std::string_view text);

}  // namespace loadforge::topology

#endif
