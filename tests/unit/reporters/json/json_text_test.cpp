// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporters/json/json_text.hpp"

#include <gtest/gtest.h>

#include <string>

#include "unit/reporters/json/mini_json.hpp"

// NOLINTBEGIN(modernize-raw-string-literal): escapes are the point of these tests.
namespace loadforge::reporters {
namespace {

using testing::Json;
using testing::JsonReader;

// --- the reader the other tests trust is itself checked first ------------------------

TEST(MiniJsonTest, AcceptsWhatJsonIsAndRefusesWhatItIsNot) {
  for (const char* good : {"null", "true", "false", "0", "-0", "1.5e+10", "\"\"", "[]", "{}",
                           "[1, [2]]", " {\"a\": {\"b\": []}} ", "\"\\u00e9\\n\"", "1E5", "12"}) {
    EXPECT_NO_THROW((void)JsonReader::parse(good)) << good;
  }
  for (const char* bad : {"",
                          "nul",
                          "01",
                          "1.",
                          "1e",
                          "+1",
                          ".5",
                          "[1,]",
                          "{\"a\":1,}",
                          "[1 2]",
                          "{\"a\" 1}",
                          "{1: 2}",
                          "\"\\x\"",
                          "\"\\ud800\"",
                          "\"\n\"",
                          "\"a",
                          "[1] 2",
                          "\"\\u12\"",
                          "\"\\u12g4\"",
                          "tru",
                          "{\"a\":}",
                          "\"\xff\"",
                          "\"\xc0\x80\"",
                          "\"\xed\xa0\x80\"",
                          "\"\xf4\x90\x80\x80\"",
                          "\"\xe2\x82\"",
                          "\"\xc3(\""}) {
    EXPECT_ANY_THROW((void)JsonReader::parse(bad)) << bad;
  }
}

TEST(MiniJsonTest, ReadsValuesBack) {
  const Json value = JsonReader::parse("{\"a\": [1, \"x\", null, true, false], \"b\": {}}");
  EXPECT_EQ(value.at("a").at(1).text, "x");
  EXPECT_EQ(value.at("a").at(0).text, "1");
  EXPECT_EQ(value.at("a").at(2).kind, Json::Kind::kNull);
  EXPECT_TRUE(value.at("a").at(3).boolean);
  EXPECT_FALSE(value.at("a").at(4).boolean);
  EXPECT_TRUE(value.has("b"));
  EXPECT_FALSE(value.has("c"));
  EXPECT_THROW((void)value.at("c"), std::out_of_range);
  EXPECT_EQ(JsonReader::parse("\"\\u00e9\\u20ac\\/\\b\\f\"").text, "\xc3\xa9\xe2\x82\xac/\b\f");
  EXPECT_EQ(JsonReader::parse("\"\xf0\x9f\x98\x80\"").text, "\xf0\x9f\x98\x80");
}

// --- strings ----------------------------------------------------------------------------

TEST(JsonStringTest, QuotesBackslashesAndControlCharactersAreEscaped) {
  EXPECT_EQ(json_string(""), "\"\"");
  EXPECT_EQ(json_string("plain text, 1.5"), "\"plain text, 1.5\"");
  EXPECT_EQ(json_string("a\"b\\c"), "\"a\\\"b\\\\c\"");
  EXPECT_EQ(json_string("\n\r\t"), "\"\\n\\r\\t\"");
  EXPECT_EQ(json_string(std::string("\x01\x1f\x7f", 3)), "\"\\u0001\\u001f\\u007f\"");
  EXPECT_EQ(json_string(std::string("a\0b", 3)), "\"a\\u0000b\"");
  EXPECT_EQ(json_string("/"), "\"/\"");
}

TEST(JsonStringTest, EveryByteStringReadsBackAsValidJson) {
  for (int byte = 0; byte < 256; ++byte) {
    const std::string text = std::string("x") + static_cast<char>(byte) + "y";
    EXPECT_NO_THROW((void)JsonReader::parse(json_string(text))) << byte;
  }
}

TEST(JsonStringTest, ValidUtf8PassesThroughUnchanged) {
  for (const char* text : {"\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\xc2\x80", "\xdf\xbf",
                           "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbf",
                           "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf", "\xf1\x80\x80\x80"}) {
    EXPECT_EQ(json_string(text), std::string("\"") + text + "\"") << text;
  }
}

TEST(JsonStringTest, InvalidUtf8IsReplacedOneReplacementPerOffendingByte) {
  const std::string replacement = "\\ufffd";
  for (const char* text : {"\x80", "\xbf", "\xc0\xaf", "\xc1\xbf", "\xf5\x80\x80\x80", "\xff"}) {
    std::string expected = "\"";
    for (const char* p = text; *p != '\0'; ++p) {
      expected += replacement;
    }
    EXPECT_EQ(json_string(text), expected + "\"");
  }
  // Truncated sequences: the lead is replaced, what follows is read afresh.
  EXPECT_EQ(json_string("\xe2\x82"), "\"\\ufffd\\ufffd\"");
  EXPECT_EQ(json_string("\xc3"), "\"\\ufffd\"");
  EXPECT_EQ(json_string("\xf0\x9f\x98"), "\"\\ufffd\\ufffd\\ufffd\"");
  EXPECT_EQ(json_string("\xc3("), "\"\\ufffd(\"");
  // Surrogates, overlong forms and the range's end.
  EXPECT_EQ(json_string("\xed\xa0\x80"), "\"\\ufffd\\ufffd\\ufffd\"");
  EXPECT_EQ(json_string("\xe0\x80\x80"), "\"\\ufffd\\ufffd\\ufffd\"");
  EXPECT_EQ(json_string("\xf0\x80\x80\x80"), "\"\\ufffd\\ufffd\\ufffd\\ufffd\"");
  EXPECT_EQ(json_string("\xf4\x90\x80\x80"), "\"\\ufffd\\ufffd\\ufffd\\ufffd\"");
  // A bad continuation in each position.
  EXPECT_EQ(json_string("\xe2\x82("), "\"\\ufffd\\ufffd(\"");
  EXPECT_EQ(json_string("\xf0\x9f(\x80"), "\"\\ufffd\\ufffd(\\ufffd\"");
  EXPECT_EQ(json_string("\xf0\x9f\x98("), "\"\\ufffd\\ufffd\\ufffd(\"");
}

TEST(Utf8LengthTest, TheLengthOfTheSequenceAtTheStartOrZero) {
  EXPECT_EQ(utf8_sequence_length("a"), 1U);
  EXPECT_EQ(utf8_sequence_length("\xc3\xa9z"), 2U);
  EXPECT_EQ(utf8_sequence_length("\xe2\x82\xac"), 3U);
  EXPECT_EQ(utf8_sequence_length("\xf0\x9f\x98\x80"), 4U);
  EXPECT_EQ(utf8_sequence_length("\x80"), 0U);
  EXPECT_EQ(utf8_sequence_length("\xf8\x88\x80\x80\x80"), 0U);
}

// --- trees --------------------------------------------------------------------------------

TEST(ToJsonTest, ScalarsAndEmptiesAndNesting) {
  results::Node object = results::Node::object();
  object.add("null", results::Node::null());
  object.add("yes", results::Node::boolean(true));
  object.add("no", results::Node::boolean(false));
  object.add("int", results::Node::integer(-9223372036854775807LL - 1));
  object.add("big", results::Node::unsigned_integer(18446744073709551615ULL));
  object.add("real", results::Node::real(0.1));
  object.add("nan", results::Node::real(std::nan("")));
  object.add("text", results::Node::text("a\"b"));
  object.add("empty_array", results::Node::array());
  object.add("empty_object", results::Node::object());
  results::Node items = results::Node::array();
  items.push(results::Node::integer(1)).push(results::Node::object());
  object.add("items", std::move(items));
  const std::string expected =
      "{\n"
      "  \"null\": null,\n"
      "  \"yes\": true,\n"
      "  \"no\": false,\n"
      "  \"int\": -9223372036854775808,\n"
      "  \"big\": 18446744073709551615,\n"
      "  \"real\": 0.1,\n"
      "  \"nan\": \"nan\",\n"
      "  \"text\": \"a\\\"b\",\n"
      "  \"empty_array\": [],\n"
      "  \"empty_object\": {},\n"
      "  \"items\": [\n"
      "    1,\n"
      "    {}\n"
      "  ]\n"
      "}\n";
  EXPECT_EQ(to_json(object), expected);
}

TEST(ToJsonTest, AnAbsenceMemberIsNullWithItsReasonBesideIt) {
  results::Node object = results::Node::object();
  object.add("value", results::Node::absence(
                          {"denied", "not permitted", "/sys/x", "mode 0400", "grant \"read\""}));
  object.add("after", results::Node::integer(1));
  const std::string text = to_json(object);
  const Json parsed = JsonReader::parse(text);
  EXPECT_EQ(parsed.at("value").kind, Json::Kind::kNull);
  const Json& reason = parsed.at("value_unavailable");
  EXPECT_EQ(reason.at("kind").text, "denied");
  EXPECT_EQ(reason.at("words").text, "not permitted");
  EXPECT_EQ(reason.at("path").text, "/sys/x");
  EXPECT_EQ(reason.at("detail").text, "mode 0400");
  EXPECT_EQ(reason.at("remediation").text, "grant \"read\"");
  EXPECT_EQ(parsed.at("after").text, "1");
  // Last member, and nested: the commas stay right.
  results::Node nested = results::Node::object();
  nested.add("inner", std::move(object));
  nested.add("last", results::Node::absence({"absent", "w", "p", "d", "r"}));
  const Json again = JsonReader::parse(to_json(nested));
  EXPECT_EQ(again.at("inner").at("value_unavailable").at("kind").text, "denied");
  EXPECT_EQ(again.at("last_unavailable").at("kind").text, "absent");
}

TEST(ToJsonTest, AnAbsenceThatIsNotAMemberIsItsOwnObject) {
  results::Node array = results::Node::array();
  array.push(results::Node::absence({"vanished", "w", "p", "d", "r"}));
  const Json parsed = JsonReader::parse(to_json(array));
  EXPECT_EQ(parsed.at(0).at("kind").text, "vanished");
  EXPECT_EQ(parsed.at(0).at("remediation").text, "r");
  EXPECT_EQ(JsonReader::parse(to_json(results::Node::absence({"a", "b", "c", "d", "e"})))
                .at("words")
                .text,
            "b");
}

}  // namespace
}  // namespace loadforge::reporters
// NOLINTEND(modernize-raw-string-literal)
