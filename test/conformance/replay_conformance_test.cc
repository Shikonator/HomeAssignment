// Venue conformance, in two halves.
//
// REPLAY: real recorded sessions replayed through the production adapters must
// reproduce books computed independently.
//
// SCENARIOS: hand-built frames for the unhealthy paths a healthy recording
// cannot contain -- gaps, no-change heartbeats, mid-stream snapshots, rejected
// subscriptions.
//
// Together these are the project's divergence control, replacing the OKX
// checksum. Continuity detects LOSS; only replay detects a stream that is
// correctly sequenced and applied WRONGLY. It is not circular: the recordings
// were captured by an independent client (reference/capture.py), the seeding
// snapshot is the venue's own, and the expected books come from an independent
// implementation of each venue's rules.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "simdjson.h"
#include "src/core/book.h"
#include "src/core/fixed.h"
#include <memory>

#include "src/venues/binance.h"
#include "src/venues/bybit.h"
#include "src/venues/okx.h"
#include "src/venues/venue.h"
#include "test/conformance/fixture.h"

namespace md::conformance {
namespace {

struct Entry {
  std::int64_t recv_ts_ns = 0;
  std::string frame;
  bool is_rest_snapshot = false;
};

std::vector<Entry> LoadRecording(const std::string& name) {
  std::vector<Entry> entries;
  std::ifstream file(RecordingPath(name));
  EXPECT_TRUE(file.is_open()) << "cannot open recording " << RecordingPath(name);

  simdjson::dom::parser parser;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    // Padded copy: simdjson requires SIMDJSON_PADDING slack past the input.
    simdjson::padded_string padded(line);
    auto result = parser.parse(padded);
    EXPECT_FALSE(result.error()) << simdjson::error_message(result.error());
    if (result.error()) continue;
    simdjson::dom::element root = result.value();

    Entry entry;
    entry.recv_ts_ns = std::int64_t(root["recv_ts_ns"]);
    entry.frame = std::string(std::string_view(root["frame"]));
    std::string_view kind;
    if (root["kind"].get_string().get(kind) == simdjson::SUCCESS) {
      entry.is_rest_snapshot = (kind == "rest_snapshot");
    }
    entries.push_back(std::move(entry));
  }

  // Delivered in RECEIVE order. The snapshot's position in the file is a
  // convention; recv_ts_ns is what says when it actually arrived, and getting
  // this wrong makes correct sequencing look like a gap.
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& a, const Entry& b) { return a.recv_ts_ns < b.recv_ts_ns; });
  return entries;
}

// Engine::Apply calls this same function, so replay drives the real path
// rather than a copy that could drift.
void ApplyToBook(VenueBook* book, FeedUpdate* update) { ApplyFeedUpdate(book, update); }

void CompareSide(std::span<const Level> got, simdjson::dom::element want, const char* side) {
  const std::size_t expected = simdjson::dom::array(want).size();
  ASSERT_EQ(expected, got.size()) << side << ": level count differs";
  std::size_t i = 0;
  for (simdjson::dom::element level : want) {
    const std::int64_t px = std::int64_t(level["px_e8"]);
    const std::int64_t qty = std::int64_t(level["qty_e8"]);
    ASSERT_EQ(px, got[i].px) << side << " level " << i << ": price differs (want "
                            << FormatFixed(px, 8) << ", got " << FormatFixed(got[i].px, 8) << ")";
    ASSERT_EQ(qty, got[i].qty) << side << " level " << i << " @ " << FormatFixed(px, 8)
                               << ": quantity differs (want " << FormatFixed(qty, 8) << ", got "
                               << FormatFixed(got[i].qty, 8) << ")";
    ++i;
  }
}

std::unique_ptr<VenueProtocol> MakeProtocol(const std::string& recording) {
  if (recording == "binance_btcusdt") {
    BinanceProtocol::Config config;
    config.symbol = "BTCUSDT";
    return std::make_unique<BinanceProtocol>(std::move(config));
  }
  if (recording == "okx_btcusdt") {
    OkxProtocol::Config config;
    config.symbol = "BTC-USDT";
    return std::make_unique<OkxProtocol>(std::move(config));
  }
  if (recording == "bybit_btcusdt") {
    BybitProtocol::Config config;
    config.symbol = "BTCUSDT";
    return std::make_unique<BybitProtocol>(std::move(config));
  }
  ADD_FAILURE() << "no protocol wired for recording " << recording;
  return nullptr;
}

class ReplayConformance : public ::testing::TestWithParam<std::string> {};

TEST_P(ReplayConformance, RecordingReproducesIndependentBook) {
  const std::string name = GetParam();
  SCOPED_TRACE("recording " + name);

  const std::vector<Entry> entries = LoadRecording(name);
  ASSERT_GE(entries.size(), 50u) << "recording missing or too short to be meaningful";

  std::unique_ptr<VenueProtocol> protocol = MakeProtocol(name);
  ASSERT_NE(nullptr, protocol);

  VenueBook book;
  std::vector<FeedUpdate> updates;
  int applied = 0;
  int snapshots = 0;

  for (const Entry& entry : entries) {
    updates.clear();
    const FrameVerdict verdict =
        entry.is_rest_snapshot
            ? protocol->OnRestSnapshot(entry.frame, entry.recv_ts_ns, &updates)
            : protocol->OnFrame(entry.frame, entry.recv_ts_ns, &updates);

    // A recording taken from a healthy session must replay cleanly. A resync
    // here means either the adapter's sequencing is wrong or the recording is
    // not faithful -- both are failures, and neither should be swallowed.
    ASSERT_EQ(FrameVerdict::kOk, verdict)
        << "replay demanded a resync at recv_ts_ns=" << entry.recv_ts_ns
        << (entry.is_rest_snapshot ? " (rest snapshot)" : " (stream frame)");

    for (FeedUpdate& update : updates) {
      if (update.is_snapshot) ++snapshots;
      ApplyToBook(&book, &update);
      ++applied;
    }
  }

  EXPECT_EQ(1, snapshots) << "expected exactly one seeding snapshot in a healthy session";
  EXPECT_TRUE(protocol->synced()) << "replay finished without ever syncing";
  EXPECT_GT(applied, 50) << "recording produced too few updates to be meaningful";

  // Invariants that must hold for any single venue, independent of the golden.
  EXPECT_TRUE(book.bids.Invariant()) << "bid book violates ordering/positivity";
  EXPECT_TRUE(book.asks.Invariant()) << "ask book violates ordering/positivity";
  EXPECT_FALSE(book.Crossed())
      << "a single venue's own book crossed, which means delta application is wrong: "
      << FormatFixed(book.bids.BestPx(), 8) << " >= " << FormatFixed(book.asks.BestPx(), 8);

  simdjson::dom::parser parser;
  auto golden = parser.load(CorpusPath("replay_" + name + "_golden").string());
  ASSERT_FALSE(golden.error()) << simdjson::error_message(golden.error());
  CompareSide(book.bids.levels(), golden.value()["bids"], "bids");
  CompareSide(book.asks.levels(), golden.value()["asks"], "asks");
}

INSTANTIATE_TEST_SUITE_P(Venues, ReplayConformance,
                         ::testing::Values("binance_btcusdt", "okx_btcusdt",
                                           "bybit_btcusdt"),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });

// ---------------------------------------------------------------------------
// Constructed scenarios
// ---------------------------------------------------------------------------

FrameVerdict VerdictFromName(std::string_view name) {
  if (name == "ok") return FrameVerdict::kOk;
  if (name == "needs_resync") return FrameVerdict::kNeedsResync;
  if (name == "parse_error") return FrameVerdict::kParseError;
  if (name == "fatal") return FrameVerdict::kFatal;
  ADD_FAILURE() << "unknown expected verdict " << name;
  return FrameVerdict::kOk;
}

const char* VerdictName(FrameVerdict verdict) {
  switch (verdict) {
    case FrameVerdict::kOk: return "ok";
    case FrameVerdict::kNeedsResync: return "needs_resync";
    case FrameVerdict::kParseError: return "parse_error";
    case FrameVerdict::kFatal: return "fatal";
  }
  return "?";
}

void CheckFinalSide(std::span<const Level> got, simdjson::dom::element want, const char* side) {
  std::vector<Level> expected;
  for (simdjson::dom::element level : want) {
    std::vector<std::string_view> parts;
    for (simdjson::dom::element v : level) parts.emplace_back(v);
    ASSERT_EQ(2u, parts.size());
    expected.push_back(Level{ParseOrDie(parts[0]), ParseOrDie(parts[1])});
  }
  // The scenario lists levels in whatever order reads best; the book holds them
  // best-first, so compare as sets keyed by price.
  ASSERT_EQ(expected.size(), got.size()) << side << ": level count differs";
  for (const Level& want_level : expected) {
    const auto it = std::find_if(got.begin(), got.end(), [&](const Level& l) {
      return l.px == want_level.px;
    });
    ASSERT_NE(it, got.end()) << side << ": missing level " << FormatFixed(want_level.px, 8);
    EXPECT_EQ(want_level.qty, it->qty)
        << side << " @ " << FormatFixed(want_level.px, 8) << ": quantity differs";
  }
}

void RunScenario(simdjson::dom::element scenario) {
  // `=` rather than parens: T name(U(x["k"])) is parsed as a declaration of x
  // as an array with bound "k", not as an initialisation.
  const std::string name = std::string(std::string_view(scenario["name"]));
  SCOPED_TRACE(name + ": " + std::string(std::string_view(scenario["description"])));

  const std::string venue = std::string(std::string_view(scenario["venue"]));
  std::unique_ptr<VenueProtocol> protocol = MakeProtocol(venue + "_btcusdt");
  ASSERT_NE(nullptr, protocol);

  VenueBook book;
  std::vector<FeedUpdate> updates;
  int step_index = 0;

  for (simdjson::dom::element step : scenario["steps"]) {
    SCOPED_TRACE("step " + std::to_string(step_index));
    const std::string frame = std::string(std::string_view(step["frame"]));
    std::string_view kind;
    const bool is_rest = step["kind"].get_string().get(kind) == simdjson::SUCCESS &&
                         kind == "rest_snapshot";

    updates.clear();
    const FrameVerdict got = is_rest ? protocol->OnRestSnapshot(frame, 1, &updates)
                                     : protocol->OnFrame(frame, 1, &updates);
    const FrameVerdict want = VerdictFromName(std::string_view(step["expect"]));
    ASSERT_EQ(want, got) << "expected " << VerdictName(want) << ", got " << VerdictName(got);

    std::int64_t want_updates = 0;
    if (step["updates"].get_int64().get(want_updates) == simdjson::SUCCESS) {
      // On any verdict other than kOk this must be zero: a failing frame that
      // appends anything leaves the caller holding a partial batch alongside a
      // verdict telling it to throw its state away.
      EXPECT_EQ(want_updates, static_cast<std::int64_t>(updates.size()))
          << "wrong number of updates appended";
    }

    for (FeedUpdate& update : updates) ApplyToBook(&book, &update);
    ++step_index;
  }

  simdjson::dom::array want_bids;
  if (scenario["final_bids"].get_array().get(want_bids) == simdjson::SUCCESS) {
    CheckFinalSide(book.bids.levels(), want_bids, "bids");
    CheckFinalSide(book.asks.levels(), scenario["final_asks"], "asks");
    EXPECT_TRUE(book.bids.Invariant());
    EXPECT_TRUE(book.asks.Invariant());
    EXPECT_FALSE(book.Crossed());
  }
}

class VenueScenario : public ::testing::TestWithParam<std::size_t> {};

TEST_P(VenueScenario, BehavesAsTheVenueRulesRequire) {
  static simdjson::dom::parser parser;
  auto doc = parser.load(CorpusPath("venue_scenarios").string());
  ASSERT_FALSE(doc.error()) << simdjson::error_message(doc.error());

  std::size_t index = 0;
  for (simdjson::dom::element scenario : doc.value()["scenarios"]) {
    if (index++ != GetParam()) continue;
    RunScenario(scenario);
    return;
  }
  FAIL() << "scenario index " << GetParam() << " out of range";
}

// Indexed rather than named because the parameter list is built before the
// fixture file is read; the scenario name is in the failure trace.
INSTANTIATE_TEST_SUITE_P(Scenarios, VenueScenario, ::testing::Range(std::size_t{0},
                                                                   std::size_t{16}));

TEST(VenueScenario, EveryScenarioIsCovered) {
  simdjson::dom::parser parser;
  auto doc = parser.load(CorpusPath("venue_scenarios").string());
  ASSERT_FALSE(doc.error());
  // If a scenario is added without widening the Range above it would never run.
  std::size_t count = 0;
  for (simdjson::dom::element one : doc.value()["scenarios"]) {
    (void)one;
    ++count;
  }
  EXPECT_EQ(16u, count)
      << "scenario count changed -- widen the INSTANTIATE_TEST_SUITE_P range";
}

}  // namespace
}  // namespace md::conformance
