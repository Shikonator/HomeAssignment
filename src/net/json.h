#pragma once

#include <cstdint>
#include <string_view>

#include "simdjson.h"
#include <vector>

#include "src/core/book.h"
#include "src/core/fixed.h"

namespace md {

// Thin wrapper over simdjson's DOM parser, reused across frames so the tape
// buffer is allocated once.
//
// DOM rather than On-Demand deliberately. On-Demand is faster because it never
// materialises a tape, but it is forward-only: fields must be visited in
// document order and every value is invalidated when the buffer is reused.
// Exchange payloads differ in field order between venues and between message
// types on one venue, so On-Demand would mean either fragile ordering
// assumptions or repeated restarts. At the 30-60 frames per second these feeds
// actually produce, tape construction on a few-kilobyte frame is nanoseconds
// against a 100ms publication cadence -- it is not where latency lives, and
// buying speed here with a correctness hazard would be a bad trade. The
// benchmark in tools/bench measures it rather than assuming.
class JsonParser {
 public:
  bool Parse(std::string_view frame, simdjson::dom::element* out) {
    auto result = parser_.parse(frame.data(), frame.size(), /*realloc_if_needed=*/true);
    if (result.error() != simdjson::SUCCESS) return false;
    *out = result.value_unsafe();
    return true;
  }

 private:
  simdjson::dom::parser parser_;
};

// Every accessor below takes simdjson_result rather than element.
//
// That is not a style choice: simdjson_result's implicit conversion to element
// THROWS when the lookup failed, so `Get(root["maybe_absent"])` on a plain
// element parameter aborts on any frame missing the field -- which is every
// subscription acknowledgement on every venue. Taking the result type forces
// the error check. A plain element converts implicitly the other way, so
// iterating an array still works unchanged.
using JsonValue = simdjson::simdjson_result<simdjson::dom::element>;

// Reads a fixed-point value from a JSON string ("68123.45"), which is how all
// three venues publish prices and sizes. Never routed through a double.
inline bool GetFixed(JsonValue value, std::int64_t* out) {
  simdjson::dom::element element;
  if (value.get(element) != simdjson::SUCCESS) return false;
  std::string_view text;
  if (element.get_string().get(text) == simdjson::SUCCESS) return ParseFixed(text, out);
  return false;
}

// Overload for a value already known to exist, e.g. while iterating an array.
inline bool GetFixed(simdjson::dom::element element, std::int64_t* out) {
  std::string_view text;
  if (element.get_string().get(text) == simdjson::SUCCESS) return ParseFixed(text, out);
  return false;
}

// Parses the [["68123.45","0.5"], ...] shape that all three venues use for
// depth. Any malformed entry fails the whole frame rather than yielding a
// partially-applied update, which would desync the book silently.
inline bool ParseLevelArray(JsonValue value, std::vector<Level>* out) {
  simdjson::dom::array array;
  if (value.get_array().get(array) != simdjson::SUCCESS) return false;
  for (simdjson::dom::element entry : array) {
    simdjson::dom::array pair;
    if (entry.get_array().get(pair) != simdjson::SUCCESS) return false;
    std::int64_t px = 0;
    std::int64_t qty = 0;
    int index = 0;
    for (simdjson::dom::element field : pair) {
      if (index == 0 && !GetFixed(field, &px)) return false;
      if (index == 1 && !GetFixed(field, &qty)) return false;
      ++index;
    }
    if (index < 2) return false;
    // A non-positive price is rejected HERE, at the only point where untrusted
    // bytes become book state, because zero is the sentinel for "no liquidity"
    // everywhere downstream: Quote.price_e8 == 0 on the wire, and
    // ComputeSideBands treats a zero reference as an empty side. An ask priced
    // at 0 would sort to the front (asks order ascending), become the best ask,
    // and make a fully-populated side report as empty with no error anywhere. A
    // negative price is worse -- it passes the empty check and becomes the
    // touch that every band and bps bound is computed from.
    //
    // A quantity of zero is legitimate and means "remove this level"; a
    // negative one is not.
    if (px <= 0 || qty < 0) return false;
    out->push_back(Level{px, qty});
  }
  return true;
}

inline bool GetString(JsonValue value, std::string_view* out) {
  simdjson::dom::element element;
  if (value.get(element) != simdjson::SUCCESS) return false;
  return element.get_string().get(*out) == simdjson::SUCCESS;
}

inline bool GetI64(JsonValue value, std::int64_t* out) {
  simdjson::dom::element element;
  if (value.get(element) != simdjson::SUCCESS) return false;
  if (element.get_int64().get(*out) == simdjson::SUCCESS) return true;
  // Several venues publish sequence numbers and timestamps as strings.
  std::string_view text;
  if (element.get_string().get(text) != simdjson::SUCCESS) return false;
  if (text.empty()) return false;
  std::int64_t parsed = 0;
  bool negative = false;
  std::size_t index = 0;
  if (text[0] == '-') {
    negative = true;
    index = 1;
    if (text.size() == 1) return false;
  }
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c < '0' || c > '9') return false;
    const int digit = c - '0';
    // Guarded because this parses VENUE-SUPPLIED strings: several venues
    // publish sequence numbers and timestamps as text, which is the only reason
    // this path exists. Signed overflow is undefined behaviour, and we build
    // -c opt -- a wrapped sequence number that happened to equal
    // last_seq_id + 1 would walk a corrupt frame straight through continuity
    // validation, which nothing downstream would catch.
    if (parsed > (INT64_MAX - digit) / 10) {
      // INT64_MIN's magnitude exceeds INT64_MAX, so it would fail the guard
      // above on its final digit despite being representable. Accept exactly
      // that one value rather than silently rejecting a legitimate number.
      if (negative && index + 1 == text.size() && parsed == INT64_MAX / 10 &&
          digit == 8 && (INT64_MAX % 10) == 7) {
        *out = INT64_MIN;
        return true;
      }
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  *out = negative ? -parsed : parsed;
  return true;
}

}  // namespace md
