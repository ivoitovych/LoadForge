// SPDX-License-Identifier: GPL-3.0-or-later
// Self-containment and product warnings for one contract header (see ../CMakeLists.txt).
#include <cstdint>

#include "verification/oracle.hpp"

namespace loadforge::contracts_headers {

// is_valid() and blocks_of() inlined into a consumer over a verdict whose
// alternative the compiler cannot see: the static release build once failed on
// exactly this (journal fragment 02, question 9). Compiled, never run.
bool verdict_keeps_contract(const verification::Verdict& verdict, verification::Contract contract,
                            std::uint64_t& blocks) noexcept {
  blocks = verification::blocks_of(verdict);
  return verification::is_valid(verdict, contract);
}

}  // namespace loadforge::contracts_headers
