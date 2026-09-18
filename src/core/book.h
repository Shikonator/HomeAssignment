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

// One side of one venue's book, held as a contiguous array sorted best-first.
//
// Why a sorted vector rather than std::map: the hot loop is the k-way merge
// across venues, which is a pure sequential scan. A contiguous array is
// prefetch-friendly and node-free, while std::map pays a cache miss per level
// and stores 40+ bytes of overhead for 16 bytes of payload. Insert cost is a
// memmove, but updates cluster near the touch so the moved span is short.
//
// The book is never truncated internally. If deep levels were dropped, a later
// update at a dropped price would be indistinguishable from an insert, and the
// book would silently desync. Truncation happens only when publishing.
template <bool Descending>
class SideBook {
 public:
  // "Better" means closer to the touch: higher for bids, lower for asks.
  static constexpr bool Better(Px a, Px b) { return Descending ? (a > b) : (a < b); }

  // Above this many changes in one frame, a single linear merge beats N
  // binary-search-plus-memmove insertions. A frame touching three levels of a
  // 5000-level book wants the in-place path; a frame touching three hundred
  // wants the merge.
  static constexpr std::size_t kBatchMergeThreshold = 8;

  // Applies one price update. qty == 0 removes the level.
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

  // Applies a frame's worth of updates. `deltas` is sorted in place and may be
  // reordered; duplicates are resolved last-wins.
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

  // Installs a fresh snapshot, discarding all prior state.
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

  // Invariant used by tests and by the debug build: strictly ordered, no
  // duplicate prices, no zero quantities.
  bool Invariant() const {
    for (std::size_t i = 0; i < levels_.size(); ++i) {
      if (levels_[i].qty <= 0) return false;
      // Zero is the "no liquidity" sentinel downstream, so it must never reach
      // the book. Rejected at ingestion; asserted here in case it ever arrives
      // by another route.
      if (levels_[i].px <= 0) return false;
      if (i > 0 && !Better(levels_[i - 1].px, levels_[i].px)) return false;
    }
    return true;
  }

 private:
  static void SortBestFirstLastWins(std::vector<Level>* v) {
    // Stable sort so that when a frame repeats a price, the later entry wins
    // after the unique() pass below.
    std::stable_sort(v->begin(), v->end(),
                     [](const Level& a, const Level& b) { return Better(a.px, b.px); });
    // Keep the LAST occurrence of each price. Walk backwards keeping firsts of
    // the reversed sequence, which is the same thing.
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

// A single venue's two-sided book.
struct VenueBook {
  BidBook bids;
  AskBook asks;

  void Clear() {
    bids.Clear();
    asks.Clear();
  }

  // A single venue's own book must never cross. If it does, our delta
  // application is wrong -- this is the cheapest possible detector for a
  // broken resync, and it runs continuously in the debug build.
  bool Crossed() const {
    return !bids.empty() && !asks.empty() && bids.BestPx() >= asks.BestPx();
  }
};

// Applies one venue update to one venue's book.
//
// Extracted as a free function rather than living inside the engine so the
// offline replay test drives the REAL path instead of a hand-mirrored copy of
// it. A copy would silently drift the first time a rule is added here, and the
// replay test would then be validating something the aggregator no longer does.
//
// `update` is consumed: its level vectors are moved from or sorted in place.

// The subset of a venue update that book application actually needs. Declared
// here rather than taking the full FeedUpdate so that src/core does not have to
// know about the venue layer.
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
