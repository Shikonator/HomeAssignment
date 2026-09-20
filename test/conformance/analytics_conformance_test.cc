// The production analytics against goldens from an independent Python
// reference, written from the specification and market_data.proto rather than
// from this C++.
//
// A disagreement means one of the two is wrong and which has to be established.
// Do not "fix" a golden to match the code.

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "simdjson.h"
#include "src/core/analytics.h"
#include "src/core/consolidated.h"
#include "src/core/fixed.h"
#include "test/conformance/fixture.h"

namespace md::conformance {
namespace {

// Wide enough that nothing truncates: these fixtures assert full-ladder
// behaviour, and ladder truncation has its own coverage.
MergeLimits NoTruncation() {
  MergeLimits limits;
  limits.notional_target_e8 = Wide(1) << 100;
  limits.max_levels = 1 << 20;
  return limits;
}

std::int64_t I(simdjson::dom::element e) { return std::int64_t(e); }
bool B(simdjson::dom::element e) { return bool(e); }

// Renders as a decimal so a failure reads as a price, not as 10000025000000.
::testing::AssertionResult EqFixed(const char* what, std::int64_t want, std::int64_t got) {
  if (want == got) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure()
         << what << ": want " << FormatFixed(want, 8) << " (" << want << "), got "
         << FormatFixed(got, 8) << " (" << got << ")";
}

#define EXPECT_FIXED(what, want, got) EXPECT_TRUE(EqFixed(what, want, got))

struct Side {
  std::vector<MergedLevel> levels;
  LadderView view;
  bool truncated = false;
};

Side BuildSide(const Fixture& fx, bool descending) {
  // Only included venues become merge inputs, which is exactly how the engine
  // excludes a stale venue: it never puts that book in the merge at all.
  std::vector<VenueSideInput> inputs;
  for (std::size_t i = 0; i < fx.books().size(); ++i) {
    if (!fx.Includes(i)) continue;
    VenueSideInput in;
    in.venue_index = static_cast<int>(i);
    in.levels = descending ? fx.books()[i].bids.levels() : fx.books()[i].asks.levels();
    inputs.push_back(in);
  }
  Side side;
  side.truncated = MergeSide(descending, inputs, NoTruncation(), &side.levels);
  side.view.levels = side.levels;
  side.view.descending = descending;
  return side;
}

void CheckLadder(const Fixture& fx, const Side& side, const char* key) {
  simdjson::dom::element want = fx.expected()["merged"][key];
  std::size_t i = 0;
  for (simdjson::dom::element level : want) {
    ASSERT_LT(i, side.levels.size()) << key << ": ladder ended early, want level "
                                     << FormatFixed(I(level["px_e8"]), 8);
    SCOPED_TRACE(std::string(key) + " level " + std::to_string(i));
    EXPECT_FIXED("px", I(level["px_e8"]), side.levels[i].px);
    EXPECT_FIXED("qty", I(level["qty_total_e8"]), side.levels[i].qty);
    ++i;
  }
  EXPECT_EQ(i, side.levels.size()) << key << ": more levels than the golden expects";
  EXPECT_FALSE(side.truncated) << key << ": unexpected truncation";
}

void CheckTouch(const Fixture& fx, const TouchInfo& got) {
  simdjson::dom::element want = fx.expected()["touch"];
  SCOPED_TRACE("touch");
  EXPECT_EQ(B(want["has_bid"]), got.has_bid);
  EXPECT_EQ(B(want["has_ask"]), got.has_ask);
  EXPECT_FIXED("best_bid", I(want["best_bid_e8"]), got.best_bid);
  EXPECT_FIXED("best_ask", I(want["best_ask_e8"]), got.best_ask);
  EXPECT_FIXED("mid", I(want["mid_price_e8"]), got.mid);
  EXPECT_FIXED("spread", I(want["spread_e8"]), got.spread_e8);
  EXPECT_FIXED("spread_bps", I(want["spread_bps_e8"]), got.spread_bps_e8);
  EXPECT_EQ(B(want["crossed"]), got.crossed);
}

void CheckVolumeBands(const Fixture& fx, const SideBands& got, const char* key) {
  simdjson::dom::element want = fx.expected()["volume_bands"][key];
  ASSERT_EQ(simdjson::dom::array(want).size(), got.volume.size())
      << key << ": wrong number of volume bands";
  std::size_t i = 0;
  for (simdjson::dom::element band : want) {
    const VolumeBandResult& g = got.volume[i];
    SCOPED_TRACE(std::string(key) + " volume band " + std::to_string(i));
    EXPECT_FIXED("target", I(band["notional_target_e8"]), g.target_e8);
    EXPECT_FIXED("vwap", I(band["vwap_price_e8"]), g.vwap_e8);
    EXPECT_FIXED("worst", I(band["worst_price_e8"]), g.worst_e8);
    EXPECT_FIXED("filled_qty", I(band["filled_qty_e8"]), g.filled_qty_e8);
    EXPECT_FIXED("filled_notional", I(band["filled_notional_e8"]), g.filled_notional_e8);
    EXPECT_EQ(B(band["fully_filled"]), g.fully_filled);
    EXPECT_EQ(B(band["open_ended"]), g.open_ended);
    EXPECT_EQ(I(band["levels_consumed"]), g.levels_consumed);
    ++i;
  }
}

void CheckPriceBands(const Fixture& fx, const SideBands& got, const char* key) {
  simdjson::dom::element want = fx.expected()["price_bands"][key];
  ASSERT_EQ(simdjson::dom::array(want).size(), got.price.size())
      << key << ": wrong number of price bands";
  std::size_t i = 0;
  for (simdjson::dom::element band : want) {
    const PriceBandResult& g = got.price[i];
    SCOPED_TRACE(std::string(key) + " price band " + std::to_string(i));
    EXPECT_FIXED("offset_bps", I(band["offset_bps_e8"]), g.offset_bps_e8);
    EXPECT_FIXED("bound", I(band["bound_price_e8"]), g.bound_e8);
    EXPECT_FIXED("qty", I(band["qty_e8"]), g.qty_e8);
    EXPECT_FIXED("notional", I(band["notional_e8"]), g.notional_e8);
    EXPECT_FIXED("vwap", I(band["vwap_price_e8"]), g.vwap_e8);
    EXPECT_EQ(I(band["levels"]), g.levels);
    EXPECT_EQ(B(band["open_ended"]), g.open_ended);
    EXPECT_EQ(B(band["depth_limited"]), g.depth_limited);
    ++i;
  }
}

// Runs one case end to end: build the ladders, compute, compare everything.
void RunCase(const Fixture& fx) {
  SCOPED_TRACE(fx.name() + ": " + fx.description());

  const Side bids = BuildSide(fx, /*descending=*/true);
  const Side asks = BuildSide(fx, /*descending=*/false);

  CheckLadder(fx, bids, "bids");
  CheckLadder(fx, asks, "asks");
  CheckTouch(fx, ComputeTouch(bids.view, asks.view));

  SideBands bid_bands;
  SideBands ask_bands;
  ComputeSideBands(bids.view, fx.bands(), &bid_bands);
  ComputeSideBands(asks.view, fx.bands(), &ask_bands);

  CheckVolumeBands(fx, bid_bands, "bid");
  CheckVolumeBands(fx, ask_bands, "ask");
  CheckPriceBands(fx, bid_bands, "bid");
  CheckPriceBands(fx, ask_bands, "ask");
}

class ConformanceTest : public ::testing::TestWithParam<std::string> {};

TEST_P(ConformanceTest, MatchesIndependentReference) { RunCase(Fixture::Load(GetParam())); }

INSTANTIATE_TEST_SUITE_P(Fixtures, ConformanceTest,
                         ::testing::ValuesIn(AllFixtureNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });

// Randomised corpus. Places notional targets exactly on cumulative level
// boundaries -- the case a single-pass walk is most likely to get wrong, and
// the one hand-built fixtures never happen to hit.
TEST(RandomCorpus, MatchesIndependentReference) {
  simdjson::dom::parser parser;
  auto result = parser.load(CorpusPath("random_books_corpus").string());
  ASSERT_FALSE(result.error()) << simdjson::error_message(result.error());

  int cases = 0;
  for (simdjson::dom::element one : result.value()["cases"]) {
    RunCase(Fixture::FromElement(one));
    ++cases;
    if (::testing::Test::HasFatalFailure()) break;
  }
  EXPECT_GE(cases, 100) << "corpus missing or truncated";
}

// Every fixture must be reachable; an empty parameter list would make the
// suite vacuously green.
TEST(ConformanceSuite, FixturesAreDiscovered) {
  EXPECT_GE(AllFixtureNames().size(), 8u)
      << "fixtures missing -- did the data dependency not get staged?";
}

}  // namespace
}  // namespace md::conformance
