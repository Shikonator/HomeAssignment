#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace md {

// Exchange feeds are exact decimal strings; doubles cannot hold them exactly.
using Px = std::int64_t;
using Qty = std::int64_t;
using Notional = std::int64_t;  // quote currency (USDT), same scale

inline constexpr std::int64_t kScale = 100'000'000;  // 1e8
inline constexpr int kScaleDigits = 8;
inline constexpr std::int64_t kMaxIntegerPart = INT64_MAX / kScale;

// MANDATORY, not a safety margin. A price of 100,000 is 1e13 at this scale and
// 0.01 BTC is 1e6, so their product is 1e19 -- past INT64_MAX (9.22e18).
// Multiplying two scaled values in int64 overflows at a hundredth of a coin.
// Every such product goes through the helpers below, never a bare `*`.
using Wide = __int128;

// No floating point. Digits past the 8th truncate. False on empty input,
// trailing garbage, exponents, or overflow.
bool ParseFixed(std::string_view s, std::int64_t* out);

// Keeps at least `min_decimals` places.
std::string FormatFixed(std::int64_t v, int min_decimals = 2);

// Asserts in debug, saturates in release: a silent clamp would turn a real
// overflow into a plausible-looking number, which is worse than crashing.
inline std::int64_t NarrowSaturating(Wide v) {
  assert(v <= static_cast<Wide>(INT64_MAX) && v >= static_cast<Wide>(INT64_MIN) &&
         "fixed-point value overflowed int64 at the wire boundary");
  if (v > static_cast<Wide>(INT64_MAX)) return INT64_MAX;
  if (v < static_cast<Wide>(INT64_MIN)) return INT64_MIN;
  return static_cast<std::int64_t>(v);
}

inline Wide NotionalWide(Px px, Qty qty) {
  return (static_cast<Wide>(px) * qty) / kScale;
}
inline Notional NotionalE8(Px px, Qty qty) {
  return NarrowSaturating(NotionalWide(px, qty));
}

// Truncates, so a sweep never claims more fill than exists.
inline Qty QtyForNotional(Wide notional_e8, Px px) {
  if (px <= 0 || notional_e8 <= 0) return 0;
  return NarrowSaturating((notional_e8 * kScale) / px);
}

inline Px Vwap(Wide notional_e8, Wide qty_e8) {
  if (qty_e8 <= 0) return 0;
  return NarrowSaturating((notional_e8 * kScale) / qty_e8);
}

// Signed.
inline std::int64_t BpsE8(std::int64_t delta, Px reference) {
  if (reference <= 0) return 0;
  return NarrowSaturating((static_cast<Wide>(delta) * 10000 * kScale) / reference);
}

inline constexpr Wide kBpsScale = Wide(10000) * kScale;

// `offset_bps_e8` away from `reference`, rounded OUTWARD on both sides so a
// level resting exactly on the boundary behaves identically for bid and ask.
inline Px OffsetByBpsOutward(Px reference, std::int64_t offset_bps_e8, bool downward) {
  if (reference <= 0 || offset_bps_e8 <= 0) return reference;
  const Wide numerator = static_cast<Wide>(reference) * offset_bps_e8;
  const Wide delta = (numerator + kBpsScale - 1) / kBpsScale;  // ceil
  return NarrowSaturating(downward ? reference - delta : reference + delta);
}

}  // namespace md
