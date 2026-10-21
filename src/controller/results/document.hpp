// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_RESULTS_DOCUMENT_HPP
#define LOADFORGE_CONTROLLER_RESULTS_DOCUMENT_HPP

// A RunResult as a tree of plain values: the one place that decides what a
// result is made of, so that every format renders the same facts and none can
// leave one out. A reporter serialises the tree and does nothing else; it
// never reads a RunResult itself. The schema in docs/result-schema.md is this
// tree's shape.
//
// ABSENCE
// -------
// A value that is not there is one of two nodes, never a zero or an empty
// string:
//
//   Null     the value was not recorded: an optional the result leaves empty.
//   Absence  a reading that was unavailable, with the kind's words, the path,
//            the detail and the remediation the source gave.
//
// REAL NUMBERS
// ------------
// A finite double is a real node. A NaN or an infinity -- a residual can be
// either, and that is the finding -- is a text node "nan", "inf" or "-inf":
// neither JSON nor a spreadsheet has a number for them, and a null would say
// "not recorded" about a value that was recorded and was not a number.

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "controller/results/result.hpp"
#include "topology/source.hpp"

namespace loadforge::results {

struct Absence {
  std::string kind;   ///< The availability's code: "absent", "vanished", "denied", "malformed",
                      ///< "unreadable".
  std::string words;  ///< topology::describe of it: "not permitted", ...
  std::string path;
  std::string detail;
  std::string remediation;
};

class Node;

// NOLINTNEXTLINE(misc-no-recursion): a tree holds trees.
struct Array {
  std::vector<Node> items;
};

// NOLINTNEXTLINE(misc-no-recursion): a tree holds trees.
struct Object {
  std::vector<std::string> keys;
  std::vector<Node> values;
};

enum class NodeKind : std::uint8_t {
  kNull,
  kBool,
  kInt,
  kUnsigned,
  kReal,
  kText,
  kArray,
  kObject,
  kAbsence,
};

// NOLINTNEXTLINE(misc-no-recursion): a tree holds trees.
class Node {
 public:
  Node() = default;  ///< Null.

  [[nodiscard]] static Node null() { return Node{}; }
  [[nodiscard]] static Node boolean(bool value);
  [[nodiscard]] static Node integer(std::int64_t value);
  [[nodiscard]] static Node unsigned_integer(std::uint64_t value);
  /// Finite: a real. Otherwise: the text "nan", "inf" or "-inf".
  [[nodiscard]] static Node real(double value);
  [[nodiscard]] static Node text(std::string value);
  [[nodiscard]] static Node array();
  [[nodiscard]] static Node object();
  [[nodiscard]] static Node absence(Absence value);

  /// Appends a member to an object, or an item to an array.
  Node& add(std::string key, Node value);
  Node& push(Node value);

  [[nodiscard]] NodeKind kind() const noexcept;
  [[nodiscard]] bool as_bool() const;
  [[nodiscard]] std::int64_t as_integer() const;
  [[nodiscard]] std::uint64_t as_unsigned() const;
  [[nodiscard]] double as_real() const;
  [[nodiscard]] const std::string& as_text() const;
  [[nodiscard]] const Array& as_array() const;
  [[nodiscard]] const Object& as_object() const;
  [[nodiscard]] const Absence& as_absence() const;

  /// The member of an object, or nullptr.
  [[nodiscard]] const Node* find(std::string_view key) const;

 private:
  std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Array,
               Object, Absence>
      value_;
};

/// A real in the shortest decimal that reads back as the same double, in the
/// plain form both JSON and a spreadsheet read ("0.1", "1e+20", "-0"). Finite
/// values only.
[[nodiscard]] std::string format_real(double value);

/// The availability's code, stable across releases: "present", "absent",
/// "vanished", "denied", "malformed", "unreadable".
[[nodiscard]] std::string_view availability_code(topology::Availability availability) noexcept;

/// The verdict's code: "clean", "corrected_errors", "incomplete", "findings".
[[nodiscard]] std::string_view verdict_code(Verdict verdict) noexcept;

/// The result as its tree. Deterministic: equal results give equal trees.
[[nodiscard]] Node to_document(const RunResult& result);

}  // namespace loadforge::results

#endif
