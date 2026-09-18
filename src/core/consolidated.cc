#include "src/core/consolidated.h"

#include <cassert>
#include <limits>

namespace md {

bool MergeSide(bool descending, std::span<const VenueSideInput> inputs,
               const MergeLimits& limits, std::vector<MergedLevel>* out) {
  out->clear();

  std::array<std::size_t, kMaxVenues> cursor{};
  const std::size_t n = inputs.size();

  // The aggregator refuses to start with more venues than slots, but MergeSide
  // is public and takes a caller-supplied index. Without this, a misconfigured
  // or future caller gets a silent out-of-bounds write into the middle of a
  // MergedLevel -- memory corruption presenting as wrong prices.
  assert(n <= static_cast<std::size_t>(kMaxVenues));
  for (const VenueSideInput& input : inputs) {
    assert(input.venue_index >= 0 && input.venue_index < kMaxVenues);
    (void)input;
  }

  Wide cumulative_notional = 0;
  bool truncated = false;
  // Computed from the first level found, since the touch is not known until
  // then. An explicit flag rather than `bound == 0` as a sentinel: a very low
  // price with a very wide offset could legitimately produce a bound of zero,
  // and while --max-publish-bps is range-checked so that cannot happen here, a
  // sentinel that can collide with a real value is the wrong shape regardless.
  Px bound = 0;
  bool bound_computed = false;

  while (true) {
    // Pick the best price among the cursor heads.
    bool found = false;
    Px best = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const auto& in = inputs[i];
      if (cursor[i] >= in.levels.size()) continue;
      const Px candidate = in.levels[cursor[i]].px;
      if (!found || (descending ? candidate > best : candidate < best)) {
        best = candidate;
        found = true;
      }
    }
    if (!found) break;

    if (out->size() >= static_cast<std::size_t>(limits.max_levels)) {
      truncated = true;
      break;
    }

    if (limits.max_bps_from_touch_e8 > 0) {
      if (!bound_computed) {
        bound = OffsetByBpsOutward(best, limits.max_bps_from_touch_e8, descending);
        bound_computed = true;
      }
      const bool inside = descending ? (best >= bound) : (best <= bound);
      if (!inside) {
        // Everything further out is worse, so the walk is finished rather than
        // merely interrupted -- but the ladder IS truncated with respect to the
        // underlying books, which is what the flag reports.
        truncated = true;
        break;
      }
    }

    // Consume every venue sitting at that price.
    MergedLevel level;
    level.px = best;
    for (std::size_t i = 0; i < n; ++i) {
      const auto& in = inputs[i];
      if (cursor[i] >= in.levels.size()) continue;
      if (in.levels[cursor[i]].px != best) continue;
      const Qty q = in.levels[cursor[i]].qty;
      level.by_venue[in.venue_index] = q;
      level.qty += q;
      ++cursor[i];
    }
    out->push_back(level);

    cumulative_notional += NotionalWide(level.px, level.qty);
    if (limits.notional_target_e8 > 0 && cumulative_notional >= limits.notional_target_e8) {
      // Stop once the deepest band any subscriber can request is covered. Check
      // whether anything remained, so `truncated` reports honestly.
      for (std::size_t i = 0; i < n; ++i) {
        if (cursor[i] < inputs[i].levels.size()) {
          truncated = true;
          break;
        }
      }
      break;
    }
  }

  return truncated;
}

}  // namespace md
