// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR 0003, rule 3: the provider's suite for the crash journal, run against the
// controller's own model of the contract (journal_model.hpp).

#include "unit/controller/journal_model.hpp"

#include <gtest/gtest.h>

#include <memory>

#include "contracts/journal/journal_contract.hpp"

namespace loadforge::contracts {
namespace {

struct ModelSubject {
  static core::Result<std::unique_ptr<journal::Writer>, journal::JournalError> open_writer(
      journal::Storage& storage) {
    return controller_testing::ModelJournal::open_writer(storage);
  }
  static core::Result<journal::Replay, journal::JournalError> replay(
      const journal::Storage& storage) {
    return controller_testing::ModelJournal::replay(storage);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(ControllerModel, JournalContract, ::testing::Types<ModelSubject>);

}  // namespace
}  // namespace loadforge::contracts
