#include "test/conformance/fixture.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>

#include "gtest/gtest.h"

namespace md::conformance {
namespace {

// Bazel stages data deps under $TEST_SRCDIR/$TEST_WORKSPACE; fall back to a
// workspace-relative path so the suite is also runnable outside `bazel test`.
std::filesystem::path FixtureDir() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  const char* workspace = std::getenv("TEST_WORKSPACE");
  if (srcdir != nullptr && workspace != nullptr) {
    std::filesystem::path p =
        std::filesystem::path(srcdir) / workspace / "test" / "conformance" / "fixtures";
    if (std::filesystem::exists(p)) return p;
  }
  return std::filesystem::path("test") / "conformance" / "fixtures";
}

std::vector<Level> ParseLevels(simdjson::dom::element arr) {
  std::vector<Level> out;
  for (simdjson::dom::element pair : arr) {
    std::vector<std::string_view> parts;
    for (simdjson::dom::element v : pair) parts.emplace_back(v);
    EXPECT_EQ(parts.size(), 2u) << "fixture level is not a [price, qty] pair";
    out.push_back(Level{ParseOrDie(parts[0]), ParseOrDie(parts[1])});
  }
  return out;
}

}  // namespace

std::int64_t ParseOrDie(std::string_view s) {
  std::int64_t v = 0;
  EXPECT_TRUE(ParseFixed(s, &v)) << "ParseFixed rejected wire value " << s;
  return v;
}

std::filesystem::path CorpusPath(const std::string& name) {
  return FixtureDir() / (name + ".json");
}

std::filesystem::path RecordingPath(const std::string& name) {
  return FixtureDir().parent_path() / "recordings" / (name + ".jsonl");
}

std::vector<std::string> AllFixtureNames() {
  std::vector<std::string> names;
  simdjson::dom::parser parser;
  for (const auto& entry : std::filesystem::directory_iterator(FixtureDir())) {
    if (entry.path().extension() != ".json") continue;
    // Identify ladder fixtures by SHAPE rather than by filename. A suffix
    // convention silently breaks the next time a file is added that does not
    // follow it, and the failure appears as a confusing missing-field error in
    // an unrelated test rather than as a discovery problem.
    auto doc = parser.load(entry.path().string());
    if (doc.error()) continue;
    simdjson::dom::object merged;
    if (doc.value()["expected"]["merged"].get_object().get(merged) != simdjson::SUCCESS) {
      continue;
    }
    names.push_back(entry.path().stem().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

int Fixture::VenueIndex(const std::string& venue) const {
  const auto it = std::find(venue_names_.begin(), venue_names_.end(), venue);
  EXPECT_NE(it, venue_names_.end()) << "unknown venue in golden: " << venue;
  return static_cast<int>(std::distance(venue_names_.begin(), it));
}

bool Fixture::Includes(std::size_t venue_index) const {
  if (venue_filter_.empty()) return true;
  const std::string& name = venue_names_[venue_index];
  return std::find(venue_filter_.begin(), venue_filter_.end(), name) != venue_filter_.end();
}

Fixture Fixture::FromElement(simdjson::dom::element doc) {
  Fixture fx;
  fx.doc_ = doc;
  fx.name_ = std::string(std::string_view(doc["name"]));
  fx.description_ = std::string(std::string_view(doc["description"]));
  fx.expected_ = doc["expected"];

  simdjson::dom::element venues = doc["input"]["venues"];
  for (auto [key, value] : simdjson::dom::object(venues)) {
    (void)value;
    fx.venue_names_.emplace_back(key);
  }
  // Sorted, so slot assignment does not depend on JSON document order.
  std::sort(fx.venue_names_.begin(), fx.venue_names_.end());
  EXPECT_LE(fx.venue_names_.size(), static_cast<std::size_t>(kMaxVenues))
      << "fixture configures more venues than kMaxVenues slots";

  fx.books_.resize(fx.venue_names_.size());
  for (std::size_t i = 0; i < fx.venue_names_.size(); ++i) {
    simdjson::dom::element book = venues[fx.venue_names_[i]];
    // Through Replace() rather than assigned directly, so the suite also
    // exercises the production sort/dedup/zero-strip path.
    fx.books_[i].bids.Replace(ParseLevels(book["bids"]));
    fx.books_[i].asks.Replace(ParseLevels(book["asks"]));
  }

  simdjson::dom::element filter = doc["input"]["venue_filter"];
  if (!filter.is_null()) {
    for (simdjson::dom::element v : filter) fx.venue_filter_.emplace_back(std::string_view(v));
  }

  for (simdjson::dom::element t : doc["request"]["notional_bands_e8"]) {
    fx.bands_.notionals_e8.push_back(std::int64_t(t));
  }
  for (simdjson::dom::element o : doc["request"]["offsets_bps_e8"]) {
    fx.bands_.offsets_bps_e8.push_back(std::int64_t(o));
  }
  return fx;
}

Fixture Fixture::Load(const std::string& name) {
  auto parser = std::make_unique<simdjson::dom::parser>();
  const std::filesystem::path path = FixtureDir() / (name + ".json");
  auto result = parser->load(path.string());
  EXPECT_FALSE(result.error()) << "cannot load fixture " << path
                               << ": " << simdjson::error_message(result.error());
  Fixture fx = FromElement(result.value());
  fx.parser_ = std::move(parser);
  return fx;
}

}  // namespace md::conformance
