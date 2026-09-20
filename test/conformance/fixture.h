#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "simdjson.h"
#include "src/core/analytics.h"
#include "src/core/book.h"
#include "src/core/consolidated.h"
#include "src/core/fixed.h"

namespace md::conformance {

// A golden fixture from reference/gen_fixtures.py.
//
// Inputs are decimal STRINGS, parsed here through the production ParseFixed, so
// a run exercises parse -> book -> merge -> analytics rather than starting from
// pre-scaled integers.
class Fixture {
 public:
  static Fixture Load(const std::string& name);

  // One case of a multi-case document. The caller owns the parser and must keep
  // it alive for the lifetime of the returned Fixture.
  static Fixture FromElement(simdjson::dom::element doc);

  const std::string& name() const { return name_; }
  const std::string& description() const { return description_; }

  // Sorted, so venue slot assignment does not depend on JSON key order.
  const std::vector<std::string>& venue_names() const { return venue_names_; }
  int VenueIndex(const std::string& venue) const;

  const std::vector<VenueBook>& books() const { return books_; }
  // Which venues are merge inputs. An empty filter means all of them; a
  // non-empty one is what staleness exclusion does -- the engine simply does
  // not pass a stale venue's book to the merge.
  const std::vector<std::string>& venue_filter() const { return venue_filter_; }
  bool Includes(std::size_t venue_index) const;
  const BandConfig& bands() const { return bands_; }
  simdjson::dom::element expected() const { return expected_; }

 private:
  // Null when the caller owns the document (FromElement).
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

std::int64_t ParseOrDie(std::string_view s);

// Ladder fixtures only, identified by shape: corpora and replay goldens live in
// the same directory and are driven by their own tests.
std::vector<std::string> AllFixtureNames();

std::filesystem::path CorpusPath(const std::string& name);
std::filesystem::path RecordingPath(const std::string& name);

}  // namespace md::conformance
