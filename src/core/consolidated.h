#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "src/core/book.h"
#include "src/core/fixed.h"

namespace md {

// Compile-time venue capacity.
//
// Sized at 4 rather than the 3 venues actually configured so one more can be
// added without changing the published ladder's layout. It is not larger
// because every slot costs 8 bytes on every published level and the band walk
// is a linear scan over these arrays. Raising it is a one-constant change; the
// aggregator refuses to start if more venues are configured than there are
// slots, rather than silently dropping one.
inline constexpr int kMaxVenues = 4;

using VenueMask = std::uint32_t;

inline constexpr VenueMask MaskOf(int venue_index) {
  return VenueMask{1} << venue_index;
}

// One price level of the consolidated ladder, carrying which venue supplied
// what.
//
// The per-venue array is not overhead paid for the optional venue filter: it is
// required by staleness exclusion, which is the identical operation ("re-derive
// the ladder including only these venues"). Having it, the filter is free.
struct MergedLevel {
  Px px = 0;
  Qty qty = 0;  // sum over the venues that contributed to this ladder
  std::array<Qty, kMaxVenues> by_venue{};

  Qty QtyFor(VenueMask mask) const {
    Qty total = 0;
    for (int v = 0; v < kMaxVenues; ++v) {
      if (mask & MaskOf(v)) total += by_venue[v];
    }
    return total;
  }
};

struct VenueClockInfo {
  std::string name;
  std::int64_t last_recv_ts_ns = 0;
  std::int64_t last_exchange_ts_ns = 0;
};

// An immutable consolidated state. Produced once per publish by the aggregator
// thread and shared by every subscriber; subscribers derive their own bands
// from it and never mutate it.
struct ConsolidatedBook {
  std::uint64_t sequence = 0;
  std::int64_t publish_ts_ns = 0;
  std::int64_t ingest_recv_ts_ns = 0;
  std::string instrument;

  // Index-aligned with the MergedLevel::by_venue slots.
  std::vector<std::string> venue_names;
  VenueMask contributing_mask = 0;
  std::vector<std::string> contributing;
  std::vector<std::string> stale;
  std::vector<VenueClockInfo> clocks;

  std::vector<MergedLevel> bids;  // best first: descending
  std::vector<MergedLevel> asks;  // best first: ascending

  // True when the ladder stopped at the configured depth rather than at the end
  // of the underlying books.
  bool bids_truncated = false;
  bool asks_truncated = false;

  // Retains capacity so a pooled book can be refilled without allocating.
  void Reset() {
    sequence = 0;
    publish_ts_ns = 0;
    ingest_recv_ts_ns = 0;
    instrument.clear();
    venue_names.clear();
    contributing_mask = 0;
    contributing.clear();
    stale.clear();
    clocks.clear();
    bids.clear();
    asks.clear();
    bids_truncated = false;
    asks_truncated = false;
  }
};

// How deep to publish.
//
// The PRICE bound is the important one, and it exists for a correctness reason
// rather than a bandwidth one.
//
// Venue books are maintained from diff streams and are never truncated
// internally (see book.h). A REST snapshot returns the levels nearest the
// touch -- about 100 bps of range on BTCUSDT -- but the diff stream then
// delivers updates for levels far outside that window, which accumulate for as
// long as the process stays connected. Two aggregators started ten minutes
// apart therefore hold different books for the same market at the same instant,
// and the accumulated deep region is a biased subset: it contains levels that
// happened to tick since we connected and omits levels that never changed.
//
// Publishing that unbounded book makes every derived number depend on our
// uptime, which a consumer cannot reason about. It also makes them
// economically meaningless: a 50M sweep "fills" only by running 800-2500 bps
// through the book, at an average slippage of several percent, which is not a
// price anyone would trade at.
//
// Bounding by price fixes both. The published ladder covers a stated distance
// from the touch, so it is reproducible regardless of uptime and describes
// liquidity someone would actually cross.
struct MergeLimits {
  Wide notional_target_e8 = 0;
  // Maximum distance from the touch, in bps scaled 1e8. 0 means unbounded.
  std::int64_t max_bps_from_touch_e8 = 0;
  int max_levels = 4096;
};

// Input to the merge: which by_venue slot a book belongs in, and its levels.
struct VenueSideInput {
  int venue_index = 0;
  std::span<const Level> levels;
};

// k-way merge of per-venue side books into a single ladder, best price first.
//
// With a handful of venues a linear scan over the cursor heads beats a heap:
// no branch-misprediction-heavy sift, and the whole cursor array lives in one
// cache line. Returns true when the walk stopped early because the limits were
// reached (i.e. the ladder is truncated).
bool MergeSide(bool descending, std::span<const VenueSideInput> inputs,
               const MergeLimits& limits, std::vector<MergedLevel>* out);

}  // namespace md
