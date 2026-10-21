// SPDX-License-Identifier: GPL-3.0-or-later
//
// RecordCodecContract against the codec this module provides (ADR 0003): the
// suite's own samples, through verification::encode and verification::decode.

#include <cstddef>
#include <span>
#include <vector>

#include "contracts/verification/record_contract.hpp"
#include "core/result.hpp"
#include "verification/record.hpp"

namespace loadforge::contracts {
namespace {

struct VerificationCoreCodec {
  static std::vector<std::byte> encode(const verification::ErrorRecord& record) {
    return verification::encode(record);
  }
  static core::Result<verification::ErrorRecord, verification::DecodeError> decode(
      std::span<const std::byte> bytes) {
    return verification::decode(bytes);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(VerificationCore, RecordCodecContract,
                               ::testing::Types<VerificationCoreCodec>);

}  // namespace
}  // namespace loadforge::contracts
