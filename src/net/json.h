#pragma once

#include <cstdint>
#include <string_view>

#include "simdjson.h"
#include <vector>

#include "src/core/book.h"
#include "src/core/fixed.h"

namespace md {

// simdjson DOM parser, reused across frames. DOM rather than On-Demand
// deliberately: On-Demand is forward-only and field order differs between
// venues, and at a few kilobytes per frame the tape costs nanoseconds against a
// 100ms cadence. Not where latency lives.
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

// Accessors take simdjson_result, not element: the implicit conversion THROWS
// when the lookup failed, which is every subscription ack on every venue.
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

// Parses [["68123.45","0.5"], ...]. Any malformed entry fails the whole frame;
// a partially-applied update would desync the book silently.
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
    // Rejected here, where untrusted bytes become book state: zero is the
    // "no liquidity" sentinel downstream, so a zero-priced ask would sort to
    // the front and make a full side report as empty. Zero QUANTITY is
    // legitimate and means "remove this level".
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
    // Guarded: this parses venue-supplied strings, and a wrapped sequence
    // number could walk a corrupt frame through continuity validation.
    if (parsed > (INT64_MAX - digit) / 10) {
      // INT64_MIN's magnitude exceeds INT64_MAX, so it fails the guard above
      // despite being representable. Accept exactly that one value.
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
