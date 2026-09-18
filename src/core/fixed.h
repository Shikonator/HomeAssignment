#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace md {

// Fixed-point price / quantity / notional. See market_data.proto for the
// full rationale; the short version is that exchange feeds are exact decimal
// strings and doubles cannot hold them exactly.
using Px = std::int64_t;
using Qty = std::int64_t;
using Notional = std::int64_t;  // quote currency (USDT), same scale

inline constexpr std::int64_t kScale = 100'000'000;  // 1e8
inline constexpr int kScaleDigits = 8;
inline constexpr std::int64_t kMaxIntegerPart = INT64_MAX / kScale;

// Wide accumulator for every product of two scaled values.
//
// This is not a safety margin, it is mandatory: a price of 100,000 is 1e13 at
// this scale, and a quantity of just 0.01 BTC is 1e6, so their product is 1e19
// -- already past INT64_MAX (9.22e18). Multiplying two scaled values in int64
// overflows at one hundredth of a bitcoin, so every such product in this
// codebase routes through the helpers below and never through a bare `*`.
using Wide = __int128;

// Parses a decimal string ("68123.45", "0", "-1.5") into a 1e8-scaled integer
// without touching floating point. Digits beyond 8 decimal places are
// truncated toward zero. Returns false on empty input, trailing garbage,
// exponent notation, or integer-part overflow.
bool ParseFixed(std::string_view s, std::int64_t* out);

// Renders a 1e8-scaled integer as a decimal string, trimming trailing zeros
// but keeping at least `min_decimals` of them.
std::string FormatFixed(std::int64_t v, int min_decimals = 2);

// Narrowing at the wire boundary.
//
// Because publish depth is derived rather than unbounded, saturation is
// unreachable on valid data -- so it asserts in debug builds and saturates in
// release. Silent saturation alone would turn a genuine overflow into a
// plausible-looking clamped number, which is a far worse failure than a crash
// in a test run.
inline std::int64_t NarrowSaturating(Wide v) {
  assert(v <= static_cast<Wide>(INT64_MAX) && v >= static_cast<Wide>(INT64_MIN) &&
         "fixed-point value overflowed int64 at the wire boundary");
  if (v > static_cast<Wide>(INT64_MAX)) return INT64_MAX;
  if (v < static_cast<Wide>(INT64_MIN)) return INT64_MIN;
  return static_cast<std::int64_t>(v);
}

// notional = price * qty, in quote currency at the same 1e8 scale.
inline Wide NotionalWide(Px px, Qty qty) {
  return (static_cast<Wide>(px) * qty) / kScale;
}
inline Notional NotionalE8(Px px, Qty qty) {
  return NarrowSaturating(NotionalWide(px, qty));
}

// Quantity required to consume `notional_e8` of quote currency at `px`.
// Truncates toward zero, so a sweep never claims more fill than exists.
inline Qty QtyForNotional(Wide notional_e8, Px px) {
  if (px <= 0 || notional_e8 <= 0) return 0;
  return NarrowSaturating((notional_e8 * kScale) / px);
}

// Quantity-weighted average price = notional / qty.
inline Px Vwap(Wide notional_e8, Wide qty_e8) {
  if (qty_e8 <= 0) return 0;
  return NarrowSaturating((notional_e8 * kScale) / qty_e8);
}

// Basis points of `delta` relative to `reference`, scaled by 1e8. Signed.
inline std::int64_t BpsE8(std::int64_t delta, Px reference) {
  if (reference <= 0) return 0;
  return NarrowSaturating((static_cast<Wide>(delta) * 10000 * kScale) / reference);
}

// bps are carried scaled by 1e8 like everything else, so one basis point is
// 1e8 and the divisor for `price * bps_e8` is 1e4 * 1e8.
inline constexpr Wide kBpsScale = Wide(10000) * kScale;

// Price `offset_bps_e8` basis points away from `reference`, rounded OUTWARD:
// down on the bid side, up on the ask side.
//
// Rounding outward on both sides is deliberate. Truncating toward zero would
// push the bid bound down (widening the band) and the ask bound down too
// (narrowing it), so a level resting exactly on the boundary would be included
// on one side and excluded on the other for the same offset.
inline Px OffsetByBpsOutward(Px reference, std::int64_t offset_bps_e8, bool downward) {
  if (reference <= 0 || offset_bps_e8 <= 0) return reference;
  const Wide numerator = static_cast<Wide>(reference) * offset_bps_e8;
  const Wide delta = (numerator + kBpsScale - 1) / kBpsScale;  // ceil
  return NarrowSaturating(downward ? reference - delta : reference + delta);
}

}  // namespace md
