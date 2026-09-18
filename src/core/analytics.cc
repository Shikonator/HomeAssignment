#include "src/core/analytics.h"

namespace md {
namespace {

PriceBandResult MakePriceBand(std::int64_t offset_bps_e8, Px bound, Wide cum_qty,
                              Wide cum_notional, int levels, bool open_ended,
                              bool depth_limited) {
  PriceBandResult band;
  band.offset_bps_e8 = offset_bps_e8;
  band.bound_e8 = bound;
  band.qty_e8 = NarrowSaturating(cum_qty);
  band.notional_e8 = NarrowSaturating(cum_notional);
  band.vwap_e8 = Vwap(cum_notional, cum_qty);
  band.levels = levels;
  band.open_ended = open_ended;
  band.depth_limited = depth_limited;
  return band;
}

}  // namespace

const MergedLevel* ViewBestLevel(const LadderView& view) {
  for (const MergedLevel& level : view.levels) {
    if (ViewQty(view, level) > 0) return &level;
  }
  return nullptr;
}

Px ViewBestPx(const LadderView& view) {
  const MergedLevel* best = ViewBestLevel(view);
  return best ? best->px : 0;
}

Qty ViewBestQty(const LadderView& view) {
  const MergedLevel* best = ViewBestLevel(view);
  return best ? ViewQty(view, *best) : 0;
}

TouchInfo ComputeTouch(const LadderView& bids, const LadderView& asks) {
  TouchInfo touch;
  touch.best_bid = ViewBestPx(bids);
  touch.best_ask = ViewBestPx(asks);
  touch.has_bid = touch.best_bid != 0;
  touch.has_ask = touch.best_ask != 0;

  // With a side missing there is no meaningful mid, spread or cross. Leaving
  // them at zero is specified behaviour, not an accident -- see Touch in
  // market_data.proto.
  if (!touch.has_bid || !touch.has_ask) return touch;

  touch.mid = NarrowSaturating((static_cast<Wide>(touch.best_bid) + touch.best_ask) / 2);
  touch.spread_e8 = touch.best_ask - touch.best_bid;
  touch.spread_bps_e8 = BpsE8(touch.spread_e8, touch.mid);
  touch.crossed = touch.best_bid >= touch.best_ask;
  return touch;
}

void ComputeSideBands(const LadderView& side, const BandConfig& config, SideBands* out) {
  out->volume.clear();
  out->price.clear();
  out->volume.reserve(config.notionals_e8.size() + 1);
  out->price.reserve(config.offsets_bps_e8.size() + 1);

  const Px reference = ViewBestPx(side);

  // Empty side. Emit zeroed bands anyway so the repeated fields stay aligned
  // with what the subscriber asked for; a consumer should never have to
  // distinguish "no band" from "empty band".
  if (reference == 0) {
    for (const Notional target : config.notionals_e8) {
      VolumeBandResult band;
      band.target_e8 = target;
      out->volume.push_back(band);
    }
    VolumeBandResult open_volume;
    open_volume.open_ended = true;
    out->volume.push_back(open_volume);

    for (const std::int64_t offset : config.offsets_bps_e8) {
      out->price.push_back(MakePriceBand(offset, 0, 0, 0, 0, false, true));
    }
    out->price.push_back(MakePriceBand(0, 0, 0, 0, 0, true, false));
    return;
  }

  Wide cumulative_qty = 0;
  Wide cumulative_notional = 0;
  int levels = 0;
  Px worst_px = reference;
  std::size_t volume_index = 0;
  std::size_t price_index = 0;

  for (const MergedLevel& level : side.levels) {
    const Qty qty = ViewQty(side, level);
    if (qty <= 0) continue;  // every venue at this price was filtered out
    const Px px = level.px;

    // Close each price band this level falls outside, using the cumulative
    // state from BEFORE the level -- the level is not inside the band.
    while (price_index < config.offsets_bps_e8.size()) {
      const std::int64_t offset = config.offsets_bps_e8[price_index];
      const Px bound = OffsetByBpsOutward(reference, offset, side.descending);
      const bool inside = side.descending ? (px >= bound) : (px <= bound);
      if (inside) break;
      out->price.push_back(MakePriceBand(offset, bound, cumulative_qty, cumulative_notional,
                                         levels, false, false));
      ++price_index;
    }

    const Wide level_notional = NotionalWide(px, qty);

    // Complete each volume band that finishes inside this level. The boundary
    // level is consumed only partially, which is the normal case and is what
    // makes the VWAP correct.
    while (volume_index < config.notionals_e8.size() &&
           cumulative_notional + level_notional >= config.notionals_e8[volume_index]) {
      const Wide target = config.notionals_e8[volume_index];

      // When the level is consumed in full there is no partial fill to compute,
      // and re-deriving the quantity from the notional would be WRONG rather
      // than merely redundant: the target was itself produced by truncating
      // px*qty/kScale, so dividing back truncates a second time and lands one
      // unit short. At exact equality the answer is already in hand.
      const bool consumes_whole_level = (cumulative_notional + level_notional <= target);
      const Qty partial = consumes_whole_level
                              ? qty
                              : QtyForNotional(target - cumulative_notional, px);

      // When the residual is smaller than one representable unit of quantity at
      // this price, the sweep completes without touching the level at all. The
      // shortfall is bounded by px/kScale -- a fraction of a cent -- and
      // reporting it as unfilled would call a satisfied sweep a failure over a
      // rounding artifact.
      const Wide partial_notional = partial > 0 ? NotionalWide(px, partial) : 0;
      const Wide filled_qty = cumulative_qty + partial;
      const Wide filled_notional = cumulative_notional + partial_notional;

      VolumeBandResult band;
      band.target_e8 = config.notionals_e8[volume_index];
      // The ACTUAL notional swept, not the requested target. Truncating the
      // boundary quantity leaves this a hair under the target, and reporting
      // the target instead would break the identity
      // filled_qty * vwap == filled_notional that lets a consumer cross-check
      // the triple. Every other field here is an actual; this one is too.
      band.filled_notional_e8 = NarrowSaturating(filled_notional);
      band.filled_qty_e8 = NarrowSaturating(filled_qty);
      band.vwap_e8 = Vwap(filled_notional, filled_qty);
      band.worst_e8 = partial > 0 ? px : worst_px;
      band.fully_filled = true;
      band.levels_consumed = levels + (partial > 0 ? 1 : 0);
      out->volume.push_back(band);
      ++volume_index;
    }

    cumulative_qty += qty;
    cumulative_notional += level_notional;
    ++levels;
    worst_px = px;
  }

  // Price bands whose bound lies beyond the end of the published ladder.
  while (price_index < config.offsets_bps_e8.size()) {
    const std::int64_t offset = config.offsets_bps_e8[price_index];
    const Px bound = OffsetByBpsOutward(reference, offset, side.descending);
    out->price.push_back(MakePriceBand(offset, bound, cumulative_qty, cumulative_notional,
                                       levels, false, true));
    ++price_index;
  }
  // The specification's trailing "1000bps+": everything to the end of the
  // ladder. Its bound is defined as the worst price actually present, so that
  // bound is reached by construction and the band is never depth-limited.
  // Whether the LADDER itself was cut short by the publish depth is a separate,
  // orthogonal fact, reported once in SnapshotMeta rather than per band.
  out->price.push_back(MakePriceBand(0, worst_px, cumulative_qty, cumulative_notional, levels,
                                     true, false));

  // Volume targets the book was too thin to fill.
  while (volume_index < config.notionals_e8.size()) {
    VolumeBandResult band;
    band.target_e8 = config.notionals_e8[volume_index];
    band.filled_qty_e8 = NarrowSaturating(cumulative_qty);
    band.filled_notional_e8 = NarrowSaturating(cumulative_notional);
    band.vwap_e8 = Vwap(cumulative_notional, cumulative_qty);
    band.worst_e8 = worst_px;
    band.fully_filled = false;
    band.levels_consumed = levels;
    out->volume.push_back(band);
    ++volume_index;
  }

  // The specification's trailing "50M+": sweep everything available.
  VolumeBandResult open_volume;
  open_volume.target_e8 = 0;
  open_volume.open_ended = true;
  open_volume.fully_filled = false;  // no target, so "met the target" is undefined
  open_volume.filled_qty_e8 = NarrowSaturating(cumulative_qty);
  open_volume.filled_notional_e8 = NarrowSaturating(cumulative_notional);
  open_volume.vwap_e8 = Vwap(cumulative_notional, cumulative_qty);
  open_volume.worst_e8 = worst_px;
  open_volume.levels_consumed = levels;
  out->volume.push_back(open_volume);
}

}  // namespace md
