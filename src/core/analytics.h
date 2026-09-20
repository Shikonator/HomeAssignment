#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "src/core/consolidated.h"
#include "src/core/fixed.h"

namespace md {

struct TouchInfo {
  bool has_bid = false;
  bool has_ask = false;
  Px best_bid = 0;
  Px best_ask = 0;
  Px mid = 0;
  std::int64_t spread_e8 = 0;
  std::int64_t spread_bps_e8 = 0;
  bool crossed = false;
};

struct VolumeBandResult {
  Notional target_e8 = 0;  // 0 for the open-ended band
  Px vwap_e8 = 0;
  Px worst_e8 = 0;
  Qty filled_qty_e8 = 0;
  Notional filled_notional_e8 = 0;
  bool fully_filled = false;
  bool open_ended = false;
  int levels_consumed = 0;
};

struct PriceBandResult {
  std::int64_t offset_bps_e8 = 0;  // 0 for the open-ended band
  Px bound_e8 = 0;
  Px vwap_e8 = 0;
  Qty qty_e8 = 0;
  Notional notional_e8 = 0;
  int levels = 0;
  bool open_ended = false;
  bool depth_limited = false;
};

// Finite band thresholds, both ascending. The open-ended trailing band is
// always appended by the walk and is not listed here.
struct BandConfig {
  std::vector<Notional> notionals_e8;
  std::vector<std::int64_t> offsets_bps_e8;
};

struct SideBands {
  std::vector<VolumeBandResult> volume;
  std::vector<PriceBandResult> price;
};

struct LadderView {
  std::span<const MergedLevel> levels;
  bool descending = false;
};

Px ViewBestPx(const LadderView& view);
Qty ViewBestQty(const LadderView& view);

TouchInfo ComputeTouch(const LadderView& bids, const LadderView& asks);

// Walks `side` exactly ONCE and produces both band families.
//
// Both families are monotonic in distance from the touch -- volume bands cross
// their notional thresholds in ascending order, price bands leave their bps
// bounds in ascending order -- so a single outward walk can close both as it
// goes. Bands are CUMULATIVE FROM THE TOUCH: the 5M band describes sweeping 5M
// starting at the best price, not the slice between 1M and 5M.
//
// Allocation-free apart from growing `out`, which the caller reuses.
void ComputeSideBands(const LadderView& side, const BandConfig& config, SideBands* out);

}  // namespace md
