// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_RESIDUAL_ORDERS_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_RESIDUAL_ORDERS_HPP

// The evaluation orders a correct kernel may use, written here from the
// specification's words ("each C entry accumulates panel by panel in
// increasing k; within a panel, the order is the kernel's for the ISA path";
// "FMA is used where the kernel names it"), never from a kernel: stand-ins for
// what a scalar, a two-lane (NEON) and a four- or eight-lane (AVX2, AVX-512)
// kernel do, so the derived tolerance is tested against every shape of
// summation tree it claims to cover. None of these is the dense-linear
// workload's code, and none could be: this stream does not see it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace loadforge::verification::orders {

enum class Shape { kForward, kReverse, kPairwise, kPanel };

/// One evaluation order. For kPanel: k-panels of depth `kc`, each summed into
/// `lanes` accumulators (k mod lanes) that are then combined pairwise, the
/// panel's partial added to the running entry; `fused` forms each product in
/// a fused multiply-add with its accumulator.
struct Order {
  Shape shape = Shape::kForward;
  std::size_t kc = 0;
  std::size_t lanes = 1;
  bool fused = false;
};

inline std::string name_of(const Order& order) {
  switch (order.shape) {
    case Shape::kForward:
      return "forward";
    case Shape::kReverse:
      return "reverse";
    case Shape::kPairwise:
      return "pairwise";
    case Shape::kPanel:
      break;
  }
  return "panel kc=" + std::to_string(order.kc) + " lanes=" + std::to_string(order.lanes) +
         (order.fused ? " fma" : "");
}

inline double pairwise(std::span<const double> x, std::span<const double> y) {
  if (x.size() == 1) {
    return x[0] * y[0];
  }
  const std::size_t half = x.size() / 2;
  return pairwise(x.first(half), y.first(half)) + pairwise(x.subspan(half), y.subspan(half));
}

inline double evaluate(const Order& order, std::span<const double> x, std::span<const double> y) {
  const std::size_t n = x.size();
  double c = 0.0;
  switch (order.shape) {
    case Shape::kForward:
      for (std::size_t k = 0; k < n; ++k) {
        c += x[k] * y[k];
      }
      return c;
    case Shape::kReverse:
      for (std::size_t k = n; k > 0; --k) {
        c += x[k - 1] * y[k - 1];
      }
      return c;
    case Shape::kPairwise:
      return n == 0 ? 0.0 : pairwise(x, y);
    case Shape::kPanel:
      break;
  }
  for (std::size_t p = 0; p < n; p += order.kc) {
    std::array<double, 8> acc{};
    for (std::size_t k = p; k < std::min(p + order.kc, n); ++k) {
      double& lane = acc.at((k - p) % order.lanes);
      lane = order.fused ? std::fma(x[k], y[k], lane) : lane + x[k] * y[k];
    }
    for (std::size_t width = order.lanes / 2; width >= 1; width /= 2) {
      for (std::size_t l = 0; l < width; ++l) {
        acc.at(l) += acc.at(l + width);
      }
    }
    c += acc[0];
  }
  return c;
}

/// Every order the tests try: the three plain shapes, then each panel depth
/// of the specification with one, two, four and eight lanes, plain and fused.
inline std::vector<Order> all(std::span<const std::uint32_t> depths) {
  std::vector<Order> result{{Shape::kForward}, {Shape::kReverse}, {Shape::kPairwise}};
  for (const std::uint32_t kc : depths) {
    for (const std::size_t lanes : {1U, 2U, 4U, 8U}) {
      for (const bool fused : {false, true}) {
        result.push_back(Order{Shape::kPanel, kc, lanes, fused});
      }
    }
  }
  return result;
}

}  // namespace loadforge::verification::orders

#endif
