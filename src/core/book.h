#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "src/core/fixed.h"

namespace md {

struct Level {
  Px px;
  Qty qty;
};

// One side of one venue's book: a contiguous array sorted best-first.
//
// NEVER TRUNCATE THIS. If deep levels were dropped, a later update at a dropped
// price would be indistinguishable from an insert and the book would silently
// desync. Truncation happens only when publishing.
template <bool Descending>
class SideBook {
 public:
  // Closer to the touch: higher for bids, lower for asks.
  static constexpr bool Better(Px a, Px b) { return Descending ? (a > b) : (a < b); }

  // Above this many changes, one linear merge beats N binary-search-plus-shift
  // insertions.
  static constexpr std::size_t kBatchMergeThreshold = 8;

  // qty == 0 removes the level.
  void Apply(Px px, Qty qty) {
    const auto it = std::lower_bound(
        levels_.begin(), levels_.end(), px,
        [](const Level& level, Px probe) { return Better(level.px, probe); });
    if (it != levels_.end() && it->px == px) {
      if (qty == 0) {
        levels_.erase(it);
      } else {
        it->qty = qty;
      }
    } else if (qty != 0) {
      levels_.insert(it, Level{px, qty});
    }
  }

  // Sorts `deltas` in place. Duplicate prices resolve last-wins.
  void ApplyBatch(std::vector<Level>* deltas) {
    if (deltas->empty()) return;
    if (deltas->size() < kBatchMergeThreshold) {
      for (const Level& level : *deltas) Apply(level.px, level.qty);
      return;
    }
    SortBestFirstLastWins(deltas);

    scratch_.clear();
    scratch_.reserve(levels_.size() + deltas->size());
    std::size_t i = 0, j = 0;
    while (i < levels_.size() && j < deltas->size()) {
      const Level& existing = levels_[i];
      const Level& update = (*deltas)[j];
      if (existing.px == update.px) {
        if (update.qty != 0) scratch_.push_back(update);
        ++i;
        ++j;
      } else if (Better(existing.px, update.px)) {
        scratch_.push_back(existing);
        ++i;
      } else {
        if (update.qty != 0) scratch_.push_back(update);
        ++j;
      }
    }
    scratch_.insert(scratch_.end(), levels_.begin() + static_cast<std::ptrdiff_t>(i),
                    levels_.end());
    for (; j < deltas->size(); ++j) {
      if ((*deltas)[j].qty != 0) scratch_.push_back((*deltas)[j]);
    }
    levels_.swap(scratch_);
  }

  void Replace(std::vector<Level> levels) {
    SortBestFirstLastWins(&levels);
    levels.erase(std::remove_if(levels.begin(), levels.end(),
                                [](const Level& l) { return l.qty == 0; }),
                 levels.end());
    levels_ = std::move(levels);
  }

  void Clear() { levels_.clear(); }

  std::span<const Level> levels() const { return levels_; }
  bool empty() const { return levels_.empty(); }
  std::size_t size() const { return levels_.size(); }
  Px BestPx() const { return levels_.empty() ? 0 : levels_.front().px; }
  Qty BestQty() const { return levels_.empty() ? 0 : levels_.front().qty; }

  bool Invariant() const {
    for (std::size_t i = 0; i < levels_.size(); ++i) {
      if (levels_[i].qty <= 0) return false;
      // Zero is the "no liquidity" sentinel downstream. Rejected at ingestion;
      // asserted here in case it arrives by another route.
      if (levels_[i].px <= 0) return false;
      if (i > 0 && !Better(levels_[i - 1].px, levels_[i].px)) return false;
    }
    return true;
  }

 private:
  static void SortBestFirstLastWins(std::vector<Level>* v) {
    // Stable, so a repeated price keeps its later entry below.
    std::stable_sort(v->begin(), v->end(),
                     [](const Level& a, const Level& b) { return Better(a.px, b.px); });
    // Keep the last occurrence of each price.
    auto write = v->begin();
    for (auto read = v->begin(); read != v->end();) {
      auto next = read;
      while (next != v->end() && next->px == read->px) ++next;
      *write++ = *(next - 1);
      read = next;
    }
    v->erase(write, v->end());
  }

  std::vector<Level> levels_;
  std::vector<Level> scratch_;
};

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

// Kept here rather than inside the engine so the replay test drives the real
// path instead of a copy that would drift.
struct FeedUpdateLevels {
  bool is_snapshot = false;
  std::vector<Level> bids;
  std::vector<Level> asks;
};

inline void ApplyFeedUpdate(VenueBook* book, FeedUpdateLevels* update) {
  if (update->is_snapshot) {
    book->bids.Replace(std::move(update->bids));
    book->asks.Replace(std::move(update->asks));
  } else {
    book->bids.ApplyBatch(&update->bids);
    book->asks.ApplyBatch(&update->asks);
  }
}

}  // namespace md
