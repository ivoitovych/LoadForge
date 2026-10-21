// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the compression workload: its output decodes, with a decoder written
// from the format document (lz4_decoder.hpp), and that decoder is itself
// checked against blocks the real lz4 tool wrote (F21). The checks run in
// both directions: the tool's blocks through this decoder, and this
// compressor's blocks through the tool, whose verdict is pinned by the
// fixtures (tests/fixtures/lz4-workload/README.md) and held to the code by
// asserting the compressor still writes exactly those bytes.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "oracle/workloads/compression/lz4_decoder.hpp"
#include "unit/workloads/compression/executors.hpp"
#include "unit/workloads/compression/support.hpp"
#include "workloads/compression/compression.hpp"
#include "workloads/compression/generator.hpp"
#include "workloads/compression/lz4_block.hpp"
#include "workloads/compression/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::compression {
namespace {

using testing::Breach;
using testing::Bytes;
using testing::fixture;
using testing::legacy_blocks;

const std::vector<std::string>& fixture_inputs() {
  static const std::vector<std::string> names = {"one-byte",       "four-bytes",  "twelve-bytes",
                                                 "thirteen-bytes", "zeros-65536", "random-65536",
                                                 "long-runs-900",  "text"};
  return names;
}

TEST(Lz4TestDecoder, DecodesEveryBlockTheRealToolWrote) {
  for (const std::string& name : fixture_inputs()) {
    const Bytes input = fixture(name + ".bin");
    ASSERT_FALSE(input.empty()) << name << ".bin is missing";
    for (const std::string mode : {".fast.lz4", ".hc.lz4"}) {
      const testing::Frame frame = legacy_blocks(fixture(name + mode));
      ASSERT_TRUE(frame.ok) << name << mode;
      ASSERT_EQ(frame.blocks.size(), 1U) << name << mode;
      const testing::Decoded decoded = testing::decode_block(frame.blocks[0], input.size());
      EXPECT_EQ(decoded.breach, Breach::kNone)
          << name << mode << ": " << describe(decoded.breach) << " at " << decoded.at;
      EXPECT_EQ(decoded.bytes, input) << name << mode;
    }
  }
}

TEST(Lz4TestDecoder, TheToolsBlocksReachEveryKindOfSequence) {
  // Evidence that the fixtures exercise what the decoder must handle: a
  // literal length in several 255 bytes, a match length in several, a match
  // overlapping itself, and two parsers that chose differently.
  const testing::Decoded random = testing::decode_block(
      legacy_blocks(fixture("random-65536.fast.lz4")).blocks.at(0), kInputBytes);
  ASSERT_EQ(random.sequences.size(), 1U);
  EXPECT_EQ(random.sequences[0].literals, kInputBytes);
  const testing::Decoded zeros = testing::decode_block(
      legacy_blocks(fixture("zeros-65536.fast.lz4")).blocks.at(0), kInputBytes);
  ASSERT_FALSE(zeros.sequences.empty());
  EXPECT_EQ(zeros.sequences[0].offset, 1U);
  EXPECT_GT(zeros.sequences[0].match, 255U * 2U);
  const testing::Decoded runs =
      testing::decode_block(legacy_blocks(fixture("long-runs-900.fast.lz4")).blocks.at(0), 900);
  ASSERT_FALSE(runs.sequences.empty());
  EXPECT_EQ(runs.sequences[0], (testing::Sequence{300, 300, 300}));
  EXPECT_NE(fixture("text.fast.lz4"), fixture("text.hc.lz4"));
}

TEST(Lz4TestDecoder, TheToolsEmptyFrameHasNoBlockAndTheFormatsEmptyBlockIsOneZero) {
  const testing::Frame frame = legacy_blocks(fixture("empty.fast.lz4"));
  ASSERT_TRUE(frame.ok);
  EXPECT_TRUE(frame.blocks.empty());
  const testing::Decoded decoded = testing::decode_block(testing::bytes_of({0x00}), 0);
  EXPECT_EQ(decoded.breach, Breach::kNone);
  EXPECT_TRUE(decoded.bytes.empty());
}

TEST(CompressionRoundTrip, TheToolDecodedTheseBlocksAndTheCompressorStillWritesThem) {
  std::vector<std::string> names = fixture_inputs();
  names.insert(names.end(), {"unit-seed1-index0", "unit-seed1-index1", "unit-seedmax-indexmax"});
  for (const std::string& name : names) {
    const Bytes input = fixture(name + ".bin");
    ASSERT_FALSE(input.empty()) << name;
    const testing::Frame frame = legacy_blocks(fixture(name + ".ours.lz4"));
    ASSERT_TRUE(frame.ok) << name;
    ASSERT_EQ(frame.blocks.size(), 1U) << name;
    EXPECT_EQ(testing::compressed(input), frame.blocks[0])
        << name << ": the compressor changed; the real tool's acceptance no longer covers it";
    const testing::Decoded decoded = testing::decode_block(frame.blocks[0], input.size());
    EXPECT_EQ(decoded.breach, Breach::kNone) << name;
    EXPECT_EQ(decoded.bytes, input) << name;
  }
}

TEST(CompressionRoundTrip, TheGeneratorStillWritesTheInputsTheToolDecodedTo) {
  struct Unit {
    std::uint64_t seed;
    std::uint64_t index;
    const char* name;
  };
  for (const Unit unit :
       {Unit{1, 0, "unit-seed1-index0.bin"}, Unit{1, 1, "unit-seed1-index1.bin"},
        Unit{~std::uint64_t{0}, ~std::uint64_t{0}, "unit-seedmax-indexmax.bin"}}) {
    Bytes input(kInputBytes);
    generate_input(unit.seed, unit.index, input);
    EXPECT_EQ(input, fixture(unit.name)) << unit.name;
  }
}

TEST(CompressionRoundTrip, EveryUnitOfSeveralSeedsDecodesToItsInput) {
  for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{42}, ~std::uint64_t{0}}) {
    auto created = create(WorkloadConfig{seed, std::uint64_t{1} << 30U, 0, {}});
    ASSERT_TRUE(created.has_value());
    const std::unique_ptr<Workload> workload = std::move(created).value();
    testing::SerialExecutor serial;
    for (std::uint64_t index = 0; index < 24; ++index) {
      Bytes input(workload->plan().input_bytes);
      workload->generate(index, input);
      Bytes output(workload->plan().output_bytes, std::byte{0xFF});
      Bytes scratch(workload->plan().scratch_bytes, std::byte{0xFF});
      workload->compute(index, input, output, scratch, serial);
      const testing::Judgement judgement = testing::judge_unit(input, output);
      EXPECT_EQ(judgement.verdict, testing::Verdict::kAgreement)
          << seed << "/" << index << ": check " << static_cast<int>(judgement.check) << " "
          << describe(judgement.breach) << " at " << judgement.at;
    }
  }
}

TEST(CompressionRoundTrip, InputsTheGeneratorWouldNeverWriteStillDecode) {
  // compute() is total over input bytes, and its output is still a block of
  // exactly that input: a corrupted input is compressed faithfully, which is
  // why the round trip alone cannot see it (verification.md §4a).
  std::vector<Bytes> inputs = {Bytes(kInputBytes, std::byte{0x00}),
                               Bytes(kInputBytes, std::byte{0xFF}),
                               testing::noise(kInputBytes, 77)};
  Bytes ramp(kInputBytes);
  for (std::size_t k = 0; k < ramp.size(); ++k) {
    ramp[k] = static_cast<std::byte>(k % 251);
  }
  inputs.push_back(ramp);
  Bytes flipped(kInputBytes);
  generate_input(8, 8, flipped);
  for (std::size_t k = 0; k < flipped.size(); k += 7) {
    flipped[k] ^= std::byte{0x80};
  }
  inputs.push_back(flipped);
  for (const Bytes& input : inputs) {
    Bytes output(output_bytes_for(kInputBytes));
    Bytes table(kTableBytes);
    write_unit(input, output, table);
    EXPECT_EQ(testing::judge_unit(input, output).verdict, testing::Verdict::kAgreement);
  }
}

}  // namespace
}  // namespace loadforge::workloads::compression
