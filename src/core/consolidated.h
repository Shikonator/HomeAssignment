#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "src/core/book.h"
#include "src/core/fixed.h"

namespace md {

// Adding a venue is a one-constant recompile; the aggregator refuses to start
// if the configuration exceeds it.
inline constexpr int kMaxVenues = 3;

struct MergedLevel {
  Px px = 0;
  Qty qty = 0;  // summed over the venues that contributed
};

// Who is resting at the touch. Computed only for the best level, because that
// is the only place a consumer can use it -- carrying it on every level cost
// 24 bytes each and served nothing else.
struct VenueTouch {
  std::string venue;
  Qty qty = 0;
};

// Per-venue state at publish time. The depth and touch figures come straight
// off that venue's own book, which the engine already holds -- the status
// endpoint previously re-derived them by scanning the merged ladder.
struct VenueClockInfo {
  std::string name;
  std::int64_t last_recv_ts_ns = 0;
  std::int64_t last_exchange_ts_ns = 0;
  int bid_levels = 0;
  int ask_levels = 0;
  Px best_bid = 0;
  Px best_ask = 0;
};

// Immutable consolidated state, produced once per publish and shared by every
// subscriber, which derive their own bands from it.
struct ConsolidatedBook {
  std::uint64_t sequence = 0;
  std::int64_t publish_ts_ns = 0;
  std::int64_t ingest_recv_ts_ns = 0;
  std::string instrument;

  // Index-aligned with the MergedLevel::by_venue slots.
  std::vector<std::string> contributing;
  std::vector<std::string> stale;
  std::vector<VenueClockInfo> clocks;

  std::vector<MergedLevel> bids;  // best first: descending
  std::vector<MergedLevel> asks;  // best first: ascending
  std::vector<VenueTouch> bid_touch;
  std::vector<VenueTouch> ask_touch;

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
    contributing.clear();
    stale.clear();
    clocks.clear();
    bids.clear();
    asks.clear();
    bid_touch.clear();
    ask_touch.clear();
    bids_truncated = false;
    asks_truncated = false;
  }
};

// How deep to publish. The PRICE bound is a correctness fix, not a bandwidth
// one: venue books are never truncated internally (see book.h), so the diff
// stream accumulates levels far outside any snapshot for as long as the process
// runs. Publishing that makes every derived number depend on our uptime, and
// makes a 50M sweep "fill" only by running 800-2500 bps out at several percent
// slippage. Bounding by price makes the answer reproducible and meaningful.
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

// k-way merge into one ladder, best price first. A linear scan over cursor
// heads beats a heap at this venue count. Returns true if the ladder was
// truncated by the limits.
bool MergeSide(bool descending, std::span<const VenueSideInput> inputs,
               const MergeLimits& limits, std::vector<MergedLevel>* out);

}  // namespace md
