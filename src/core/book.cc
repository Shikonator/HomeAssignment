#include "src/core/book.h"

#include <algorithm>
#include <utility>

namespace md {
namespace {

// Closer to the touch: higher for bids, lower for asks.
template <bool Descending>
constexpr bool Better(Px a, Px b) {
  return Descending ? (a > b) : (a < b);
}

// Above this many changes, one linear merge beats N binary-search-plus-shift
// insertions.
constexpr std::size_t kBatchMergeThreshold = 8;

template <bool Descending>
void SortBestFirstLastWins(std::vector<Level>* v) {
  // Stable, so a repeated price keeps its later entry below.
  std::stable_sort(v->begin(), v->end(),
                   [](const Level& a, const Level& b) { return Better<Descending>(a.px, b.px); });
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

}  // namespace

template <bool Descending>
void SideBook<Descending>::Apply(Px px, Qty qty) {
  const auto it = std::lower_bound(
      levels_.begin(), levels_.end(), px,
      [](const Level& level, Px probe) { return Better<Descending>(level.px, probe); });
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

template <bool Descending>
void SideBook<Descending>::ApplyBatch(std::vector<Level>* deltas) {
  if (deltas->empty()) return;
  if (deltas->size() < kBatchMergeThreshold) {
    for (const Level& level : *deltas) Apply(level.px, level.qty);
    return;
  }
  SortBestFirstLastWins<Descending>(deltas);

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
    } else if (Better<Descending>(existing.px, update.px)) {
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

template <bool Descending>
void SideBook<Descending>::Replace(std::vector<Level> levels) {
  SortBestFirstLastWins<Descending>(&levels);
  levels.erase(
      std::remove_if(levels.begin(), levels.end(), [](const Level& l) { return l.qty == 0; }),
      levels.end());
  levels_ = std::move(levels);
}

template <bool Descending>
bool SideBook<Descending>::Invariant() const {
  for (std::size_t i = 0; i < levels_.size(); ++i) {
    // Zero price is the "no liquidity" sentinel downstream. Rejected at
    // ingestion; asserted here in case it arrives by another route.
    if (levels_[i].qty <= 0 || levels_[i].px <= 0) return false;
    if (i > 0 && !Better<Descending>(levels_[i - 1].px, levels_[i].px)) return false;
  }
  return true;
}

template class SideBook<true>;
template class SideBook<false>;

void ApplyFeedUpdate(VenueBook* book, FeedUpdateLevels* update) {
  if (update->is_snapshot) {
    book->bids.Replace(std::move(update->bids));
    book->asks.Replace(std::move(update->asks));
  } else {
    book->bids.ApplyBatch(&update->bids);
    book->asks.ApplyBatch(&update->asks);
  }
}

}  // namespace md
