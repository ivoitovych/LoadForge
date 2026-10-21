// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporters/csv/csv_text.hpp"

#include <gtest/gtest.h>

#include <string>

#include "unit/reporters/csv/mini_csv.hpp"

namespace loadforge::reporters {
namespace {

using testing::CsvRecord;
using testing::parse_csv;

TEST(MiniCsvTest, ReadsRfc4180AndRefusesWhatItIsNot) {
  const auto records = parse_csv("a,b\r\n\"x,\"\"y\"\"\r\nz\",\r\n");
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[1][0], "x,\"y\"\r\nz");
  EXPECT_EQ(records[1][1], "");
  EXPECT_EQ(parse_csv("a\r\n\r\n").size(), 2U) << "an empty field is a record of one empty field";
  for (const char* bad : {"", "a,b\r\nc\r\n", "a\n", "a\rb\r\n", "a,b", "a\"b\r\n", "\"a\"b\r\n",
                          "\"a\r\n", "a\r\n\"b"}) {
    EXPECT_ANY_THROW((void)parse_csv(bad)) << bad;
  }
}

TEST(CsvFieldTest, QuotedWhenItHoldsACommaAQuoteACrOrAnLf) {
  EXPECT_EQ(csv_field(""), "");
  EXPECT_EQ(csv_field("plain 1.5 text"), "plain 1.5 text");
  EXPECT_EQ(csv_field("a,b"), "\"a,b\"");
  EXPECT_EQ(csv_field("say \"hi\""), "\"say \"\"hi\"\"\"");
  EXPECT_EQ(csv_field("a\nb"), "\"a\nb\"");
  EXPECT_EQ(csv_field("a\rb"), "\"a\rb\"");
  EXPECT_EQ(csv_field("\""), "\"\"\"\"");
  EXPECT_EQ(csv_field(" padded "), " padded ") << "RFC 4180 keeps spaces; so does this";
}

TEST(CsvFieldTest, EveryFieldReadsBackAsItself) {
  for (const char* text : {"", ",", "\"", "\"\"", "a,\"b\",c", "\r\n", "\n", "x\r", "\r"}) {
    const auto records = parse_csv(csv_field(text) + ",z\r\n");
    ASSERT_EQ(records.size(), 1U) << text;
    EXPECT_EQ(records[0][0], text);
  }
}

results::Node sample() {
  results::Node root = results::Node::object();
  root.add("schema_version", results::Node::unsigned_integer(7));
  root.add("flag", results::Node::boolean(true));
  root.add("off", results::Node::boolean(false));
  root.add("signed", results::Node::integer(-3));
  root.add("real", results::Node::real(0.1));
  root.add("nan", results::Node::real(std::nan("")));
  root.add("text", results::Node::text("a,b"));
  root.add("none", results::Node::null());
  root.add("no_items", results::Node::array());
  root.add("no_members", results::Node::object());
  results::Node list = results::Node::array();
  list.push(results::Node::unsigned_integer(1));
  results::Node inner = results::Node::object();
  inner.add("k", results::Node::text("v"));
  list.push(std::move(inner));
  root.add("list", std::move(list));
  root.add("gone", results::Node::absence({"denied", "not permitted", "/sys/x", "mode 0400",
                                           "grant, \"read\" access"}));
  return root;
}

TEST(ToCsvTest, OneRowPerLeafWithTheHeaderFirst) {
  const auto records = parse_csv(to_csv(sample()));
  const CsvRecord header = {"schema_version", "field",       "value",  "status",
                            "reason",         "source_path", "detail", "remediation"};
  ASSERT_EQ(records.size(), 14U);
  EXPECT_EQ(records[0], header);
  const auto row = [&records](const std::string& field) -> const CsvRecord& {
    for (std::size_t i = 1; i < records.size(); ++i) {
      if (records[i][1] == field) {
        return records[i];
      }
    }
    throw std::out_of_range(field);
  };
  EXPECT_EQ(row("schema_version"),
            (CsvRecord{"7", "schema_version", "7", "present", "", "", "", ""}));
  EXPECT_EQ(row("flag")[2], "true");
  EXPECT_EQ(row("off")[2], "false");
  EXPECT_EQ(row("signed")[2], "-3");
  EXPECT_EQ(row("real")[2], "0.1");
  EXPECT_EQ(row("nan")[2], "nan");
  EXPECT_EQ(row("text")[2], "a,b");
  EXPECT_EQ(row("list[0]")[2], "1");
  EXPECT_EQ(row("list[1].k")[2], "v");
  for (const CsvRecord& each : std::vector<CsvRecord>(records.begin() + 1, records.end())) {
    EXPECT_EQ(each[0], "7") << "the version is on every row";
  }
}

TEST(ToCsvTest, AbsenceIsAnEmptyCellWithItsReasonInItsOwnColumns) {
  const auto records = parse_csv(to_csv(sample()));
  const auto find = [&records](const std::string& field) {
    for (const CsvRecord& each : records) {
      if (each[1] == field) {
        return each;
      }
    }
    throw std::out_of_range(field);
  };
  EXPECT_EQ(find("none"), (CsvRecord{"7", "none", "", "absent", "not recorded", "", "", ""}));
  EXPECT_EQ(find("gone"), (CsvRecord{"7", "gone", "", "denied", "not permitted", "/sys/x",
                                     "mode 0400", "grant, \"read\" access"}));
  EXPECT_EQ(find("no_items"), (CsvRecord{"7", "no_items", "", "empty", "no items", "", "", ""}));
  EXPECT_EQ(find("no_members"),
            (CsvRecord{"7", "no_members", "", "empty", "no members", "", "", ""}));
}

TEST(ToCsvTest, RecordsEndInCrlfAndTheTableEndsWithOne) {
  const std::string text = to_csv(sample());
  EXPECT_EQ(text.substr(text.size() - 2), "\r\n");
  EXPECT_EQ(text.find('\n'), text.find("\r\n") + 1) << "no bare line feed outside a quoted field";
}

TEST(ToCsvTest, ANestedRootFieldIsJoinedByDotsAndIndexes) {
  results::Node root = results::Node::object();
  root.add("schema_version", results::Node::unsigned_integer(1));
  results::Node outer = results::Node::array();
  results::Node inner = results::Node::array();
  inner.push(results::Node::text("deep"));
  outer.push(std::move(inner));
  root.add("a", std::move(outer));
  const auto records = parse_csv(to_csv(root));
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[2][1], "a[0][0]");
  EXPECT_EQ(records[2][2], "deep");
}

}  // namespace
}  // namespace loadforge::reporters
