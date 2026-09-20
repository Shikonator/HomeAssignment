#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "src/core/fixed.h"

namespace md {

struct Level {
  Px px;
  Qty qty;
};

// One side of one venue's book: a contiguous array sorted best-first,
// descending for bids and ascending for asks.
//
// NEVER TRUNCATE THIS. If deep levels were dropped, a later update at a dropped
// price would be indistinguishable from an insert and the book would silently
// desync. Truncation happens only when publishing.
template <bool Descending>
class SideBook {
 public:
  // qty == 0 removes the level.
  void Apply(Px px, Qty qty);

  // Sorts `deltas` in place. Duplicate prices resolve last-wins.
  void ApplyBatch(std::vector<Level>* deltas);

  void Replace(std::vector<Level> levels);

  // Strictly ordered, no duplicate prices, no non-positive price or quantity.
  bool Invariant() const;

  void Clear() { levels_.clear(); }
  std::span<const Level> levels() const { return levels_; }
  bool empty() const { return levels_.empty(); }
  std::size_t size() const { return levels_.size(); }
  Px BestPx() const { return levels_.empty() ? 0 : levels_.front().px; }
  Qty BestQty() const { return levels_.empty() ? 0 : levels_.front().qty; }

 private:
  std::vector<Level> levels_;
  std::vector<Level> scratch_;
};

// Only these two exist, so the template is instantiated once in book.cc rather
// than in every translation unit that includes this header.
extern template class SideBook<true>;
extern template class SideBook<false>;

using BidBook = SideBook<true>;
using AskBook = SideBook<false>;

struct VenueBook {
  BidBook bids;
  AskBook asks;

  void Clear() {
    bids.Clear();
    asks.Clear();
  }

  // A venue's own book crossing means OUR delta application is wrong. Cheapest
  // possible detector for a broken resync; runs continuously in debug.
  bool Crossed() const {
    return !bids.empty() && !asks.empty() && bids.BestPx() >= asks.BestPx();
  }
};

struct FeedUpdateLevels {
  bool is_snapshot = false;
  std::vector<Level> bids;
  std::vector<Level> asks;
};

// Kept here rather than inside the engine so the replay test drives the real
// path instead of a copy that would drift.
void ApplyFeedUpdate(VenueBook* book, FeedUpdateLevels* update);

}  // namespace md
