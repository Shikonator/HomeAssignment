#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "simdjson.h"
#include "src/core/analytics.h"
#include "src/core/book.h"
#include "src/core/consolidated.h"
#include "src/core/fixed.h"

namespace md::conformance {

// A golden fixture produced by test/conformance/reference/gen_fixtures.py.
//
// Inputs are held as exchange-style decimal STRINGS and parsed here through
// the production ParseFixed, so a conformance run exercises the whole path --
// parse, book, merge, analytics -- rather than starting from pre-scaled
// integers the way a unit test would.
class Fixture {
 public:
  // Loads by bare name, e.g. "deep_50m_sweep". Fails the test on error.
  static Fixture Load(const std::string& name);

  // Builds from one case of a multi-case document. The caller owns the parser
  // and must keep it alive for the lifetime of the returned Fixture.
  static Fixture FromElement(simdjson::dom::element doc);

  const std::string& name() const { return name_; }
  const std::string& description() const { return description_; }

  // Venue slot assignment: input venue names, sorted, so the mapping from a
  // golden's by_venue keys to MergedLevel::by_venue indices is deterministic
  // and does not depend on JSON document order.
  const std::vector<std::string>& venue_names() const { return venue_names_; }
  int VenueIndex(const std::string& venue) const;

  const std::vector<VenueBook>& books() const { return books_; }

  // Empty when the fixture applies no filter.
  const std::vector<std::string>& venue_filter() const { return venue_filter_; }
  bool filtered() const { return !venue_filter_.empty(); }
  VenueMask mask() const;

  const BandConfig& bands() const { return bands_; }

  // The `expected` object. Held rather than copied into structs so a missing
  // golden field surfaces as a lookup error instead of a silent default.
  simdjson::dom::element expected() const { return expected_; }

 private:
  // Null when the document is owned by the caller (FromElement). When set, it
  // owns the backing store and must outlive every element handed out above.
  std::unique_ptr<simdjson::dom::parser> parser_;
  simdjson::dom::element doc_;
  simdjson::dom::element expected_;

  std::string name_;
  std::string description_;
  std::vector<std::string> venue_names_;
  std::vector<VenueBook> books_;
  std::vector<std::string> venue_filter_;
  BandConfig bands_;
};

// Parses a wire-form decimal string through the production parser.
std::int64_t ParseOrDie(std::string_view s);

// Every analytics fixture in fixtures/, by bare name. Excludes corpora, which
// are not ladder fixtures and have their own tests.
std::vector<std::string> AllFixtureNames();

// Path to a non-ladder corpus file in fixtures/, by bare name.
std::filesystem::path CorpusPath(const std::string& name);

// Path to a captured venue recording in recordings/, by bare name.
std::filesystem::path RecordingPath(const std::string& name);

}  // namespace md::conformance
