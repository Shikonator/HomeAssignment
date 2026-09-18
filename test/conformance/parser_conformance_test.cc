// Conformance: ParseFixed / FormatFixed against an independent reference.
//
// This is the boundary where every venue's decimal strings become integers. A
// silent bug here corrupts every price, quantity, notional and VWAP downstream
// while the system continues to look entirely healthy, and no higher-level
// fixture would catch it -- the goldens would simply be wrong in the same way
// the code is. Hence a dedicated differential corpus.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "simdjson.h"
#include "src/core/fixed.h"
#include "test/conformance/fixture.h"

namespace md::conformance {
namespace {

struct Case {
  std::string input;
  bool ok = false;
  std::int64_t out = 0;
};

std::vector<Case> LoadCorpus() {
  static simdjson::dom::parser parser;
  auto result = parser.load(CorpusPath("parser_corpus").string());
  EXPECT_FALSE(result.error()) << simdjson::error_message(result.error());
  std::vector<Case> cases;
  for (simdjson::dom::element c : result.value()["cases"]) {
    Case one;
    one.input = std::string(std::string_view(c["in"]));
    one.ok = bool(c["ok"]);
    if (one.ok) one.out = std::int64_t(c["out"]);
    cases.push_back(std::move(one));
  }
  return cases;
}

// Printable rendering so a failure on a control character or a non-ASCII digit
// is readable rather than a hole in the terminal.
std::string Show(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f) {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  out.push_back('"');
  return out;
}

TEST(ParserConformance, MatchesIndependentReference) {
  const std::vector<Case> cases = LoadCorpus();
  ASSERT_GE(cases.size(), 400u) << "corpus missing or truncated";

  int accepted = 0;
  for (const Case& c : cases) {
    std::int64_t got = 0;
    const bool ok = ParseFixed(c.input, &got);
    // EXPECT rather than ASSERT: a differential run should report every
    // disagreement in one pass, not stop at the first and hide the pattern.
    EXPECT_EQ(c.ok, ok) << "verdict differs for " << Show(c.input)
                        << " (reference " << (c.ok ? "accepts" : "rejects")
                        << ", ParseFixed " << (ok ? "accepts" : "rejects") << ")";
    if (c.ok && ok) {
      EXPECT_EQ(c.out, got) << "value differs for " << Show(c.input) << ": reference "
                            << FormatFixed(c.out, 8) << ", ParseFixed " << FormatFixed(got, 8);
      ++accepted;
    }
  }
  EXPECT_GT(accepted, 350) << "corpus is nearly all rejections -- it is not testing much";
}

// Every accepted value must survive a render/reparse cycle unchanged. This is
// what makes FormatFixed safe to use in diagnostics and in the clients' stdout.
TEST(ParserConformance, FormatRoundTrips) {
  for (const Case& c : LoadCorpus()) {
    if (!c.ok) continue;
    for (int min_decimals : {0, 2, 8}) {
      const std::string rendered = FormatFixed(c.out, min_decimals);
      std::int64_t reparsed = 0;
      ASSERT_TRUE(ParseFixed(rendered, &reparsed))
          << "FormatFixed produced something ParseFixed rejects: " << Show(rendered)
          << " from " << c.out << " (min_decimals=" << min_decimals << ")";
      EXPECT_EQ(c.out, reparsed)
          << "round trip changed the value: " << c.out << " -> " << Show(rendered) << " -> "
          << reparsed << " (min_decimals=" << min_decimals << ")";
    }
  }
}

// The representable range ends mid-decimal, at 92233720368.54775807, so the
// boundary is worth asserting directly rather than only through the corpus.
TEST(ParserConformance, Int64Boundary) {
  std::int64_t v = 0;
  ASSERT_TRUE(ParseFixed("92233720368.54775807", &v));
  EXPECT_EQ(INT64_MAX, v);
  EXPECT_FALSE(ParseFixed("92233720368.54775808", &v));
  EXPECT_FALSE(ParseFixed("92233720369", &v));
}

// Truncation past the eighth decimal is documented behaviour, not rounding:
// a venue publishing more precision than the tick size uses must not round up
// into a price level that does not exist.
TEST(ParserConformance, TruncatesRatherThanRounds) {
  std::int64_t v = 0;
  ASSERT_TRUE(ParseFixed("0.000000019", &v));
  EXPECT_EQ(1, v) << "0.000000019 must truncate to 0.00000001, not round to 0.00000002";
  ASSERT_TRUE(ParseFixed("1.999999999", &v));
  EXPECT_EQ(199999999, v);
}

}  // namespace
}  // namespace md::conformance
