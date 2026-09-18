#include "src/core/fixed.h"

#include <algorithm>

namespace md {

bool ParseFixed(std::string_view s, std::int64_t* out) {
  if (s.empty()) return false;

  std::size_t i = 0;
  bool negative = false;
  if (s[i] == '+' || s[i] == '-') {
    negative = (s[i] == '-');
    ++i;
  }

  std::int64_t integer_part = 0;
  bool saw_digit = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    saw_digit = true;
    if (integer_part > kMaxIntegerPart / 10) return false;
    integer_part = integer_part * 10 + (s[i] - '0');
    if (integer_part > kMaxIntegerPart) return false;
  }

  std::int64_t fraction = 0;
  int digits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
      saw_digit = true;
      if (digits < kScaleDigits) {
        fraction = fraction * 10 + (s[i] - '0');
        ++digits;
      }
      // Digits past the 8th are truncated; exchanges occasionally publish more
      // precision than the instrument's tick size actually uses.
    }
  }

  // Anything left over (exponent notation, stray whitespace, units) is a parse
  // failure rather than a silent partial read.
  if (i != s.size() || !saw_digit) return false;

  while (digits < kScaleDigits) {
    fraction *= 10;
    ++digits;
  }

  if (integer_part > (INT64_MAX - fraction) / kScale) return false;
  const std::int64_t value = integer_part * kScale + fraction;
  *out = negative ? -value : value;
  return true;
}

std::string FormatFixed(std::int64_t v, int min_decimals) {
  const bool negative = v < 0;
  // Negate in unsigned space so INT64_MIN does not trap.
  std::uint64_t magnitude =
      negative ? (~static_cast<std::uint64_t>(v) + 1) : static_cast<std::uint64_t>(v);

  const std::uint64_t integer_part = magnitude / kScale;
  std::uint64_t fraction = magnitude % kScale;

  char frac_digits[kScaleDigits];
  for (int i = kScaleDigits - 1; i >= 0; --i) {
    frac_digits[i] = static_cast<char>('0' + (fraction % 10));
    fraction /= 10;
  }

  int keep = kScaleDigits;
  while (keep > min_decimals && frac_digits[keep - 1] == '0') --keep;

  std::string out;
  out.reserve(24);
  if (negative) out.push_back('-');
  out += std::to_string(integer_part);
  if (keep > 0) {
    out.push_back('.');
    out.append(frac_digits, static_cast<std::size_t>(keep));
  }
  return out;
}

}  // namespace md
