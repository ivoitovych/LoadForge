// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/services.hpp"

#include <memory>

#include "platform/fpenv/real_control.hpp"
#include "platform/isa/probe.hpp"
#include "verification/oracle/registry/registry.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::worker {

struct ProductionServices::Owned {
  workloads::BuiltinRegistry workloads;
  verification::BuiltinOracleRegistry oracles;
  fpenv::RealFpControl fp;
  PosixThreads threads;
  std::unique_ptr<simd::IsaProbe> isa = isa::make_native_probe();
};

ProductionServices::ProductionServices() : owned_(std::make_unique<Owned>()) {
  services_.workloads = &owned_->workloads;
  services_.oracles = &owned_->oracles;
  services_.fp = &owned_->fp;
  services_.threads = &owned_->threads;
  services_.isa = owned_->isa.get();
  services_.make_runner = &verification::make_slice_runner;
}

ProductionServices::~ProductionServices() = default;

}  // namespace loadforge::worker
