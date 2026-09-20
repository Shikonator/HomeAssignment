#include "src/aggregator/engine.h"

#include <algorithm>
#include <utility>

#include "src/core/clock.h"

namespace md {
namespace {
constexpr std::size_t kLatencySamples = 4096;

// Upper bound on batches folded in before publishing. Comfortably above what
// three venues produce in one cycle; anything left over is drained next pass.
constexpr int kMaxBatchesPerPass = 256;
}  // namespace

Engine::Engine(EngineConfig config, std::vector<VenueFeed> venues)
    : config_(std::move(config)),
      venues_(std::move(venues)),
      books_(venues_.size()),
      last_recv_ns_(venues_.size(), 0),
      last_exchange_ns_(venues_.size(), 0),
      latency_samples_(kLatencySamples, 0) {}

Engine::~Engine() { Stop(); }

void Engine::Start() {
  start_wall_ns_ = WallNowNs();
  thread_ = std::thread([this] { Run(); });
}

void Engine::Stop() {
  if (stopping_.exchange(true)) return;
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  for (const auto& slot : subscribers_) slot->Stop();
  subscribers_.clear();
}

std::shared_ptr<BookSlot> Engine::AddSubscriber() {
  auto slot = std::make_shared<BookSlot>();
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  subscribers_.push_back(slot);
  return slot;
}

void Engine::RemoveSubscriber(const std::shared_ptr<BookSlot>& slot) {
  slot->Stop();
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  subscribers_.erase(std::remove(subscribers_.begin(), subscribers_.end(), slot),
                     subscribers_.end());
}

std::shared_ptr<const ConsolidatedBook> Engine::Latest() const {
  std::lock_guard<std::mutex> lock(latest_mutex_);
  return latest_;
}

void Engine::Run() {
  FeedBatch batch;
  while (!stopping_.load(std::memory_order_relaxed)) {
    bool changed = false;
    // Bounded so the drain cannot starve the publish. In practice a venue can
    // never outrun us -- the producer parses JSON per item while the consumer
    // only applies levels, and venue rates are bounded by the exchange at a few
    // messages a second -- but that is an argument about relative speed, not a
    // guarantee. The bound makes termination structural, and also stops a burst
    // on one venue delaying the publish that carries the other two.
    int drained = 0;
    for (const VenueFeed& venue : venues_) {
      while (drained < kMaxBatchesPerPass && venue.ring->Pop(&batch)) {
        Apply(&batch);
        ++drained;
        changed = true;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (changed) {
      Publish();
      last_publish_ = now;
    } else if (now - last_publish_ >= config_.heartbeat_interval) {
      // Publish on a timer as well as on change.
      //
      // Staleness is computed inside Publish(), so without this the one
      // scenario staleness exists to report -- every venue silent -- is exactly
      // the scenario where nothing is ever published and no subscriber is ever
      // told. A client's last message would show a healthy book and then
      // updates would simply stop, leaving "the market is quiet" and "all three
      // feeds are dead" indistinguishable. It also makes Touch.has_bid/has_ask
      // false reachable at all, which is otherwise dead code.
      Publish();
      last_publish_ = now;
    } else {
      // Polling rather than blocking. A condition variable would need the venue
      // threads to signal, which puts a lock on the ingest hot path to save a
      // wakeup that costs almost nothing. The sleep bounds the latency this
      // adds to a fraction of a millisecond against feeds that publish every
      // 100ms.
      std::this_thread::sleep_for(config_.idle_poll);
    }
  }
}

void Engine::Apply(FeedBatch* batch) {
  const int venue = batch->venue_index;
  VenueBook& book = books_[venue];

  for (FeedUpdate& update : batch->updates) {
    ApplyFeedUpdate(&book, &update);
    last_recv_ns_[venue] = update.recv_ts_ns;
    if (update.exchange_ts_ns != 0) last_exchange_ns_[venue] = update.exchange_ts_ns;
    newest_recv_ns_ = std::max(newest_recv_ns_, update.recv_ts_ns);
  }
  batch->updates.clear();
}

// Decides which venue books become merge inputs, and records the rest as
// stale. Every venue gets a clock entry either way, so a consumer can see how
// far behind each one is.
void Engine::SelectContributingVenues(std::int64_t now_steady, ConsolidatedBook* book_ptr,
                                      std::vector<VenueSideInput>* bid_inputs,
                                      std::vector<VenueSideInput>* ask_inputs) {
  ConsolidatedBook& book = *book_ptr;
  const std::int64_t staleness_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(config_.staleness_timeout).count();

  bid_inputs->reserve(venues_.size());
  ask_inputs->reserve(venues_.size());

  for (std::size_t i = 0; i < venues_.size(); ++i) {
    const std::string& name = venues_[i].name;

    // Two conditions because they answer different questions, not one twice.
    //
    // `live` drops a venue the moment it starts resyncing: Resync() sets
    // kSyncing but books_[i] still holds the OLD book until a fresh snapshot
    // lands, so freshness alone would publish stale prices for seconds.
    //
    // `fresh` drops a connected-but-silent venue, long before the runner's 30s
    // watchdog tears the socket down. And note last_recv_ns_ is stamped in
    // Apply(), not on receipt -- so it measures data that actually reached the
    // book, which the runner cannot know and cannot answer for us.
    const auto state = static_cast<VenueState>(venues_[i].stats->state.load());
    const bool live = (state == VenueState::kLive);
    const bool fresh = last_recv_ns_[i] != 0 && (now_steady - last_recv_ns_[i]) < staleness_ns;

    // A venue whose socket half-dies keeps a plausible-looking book that would
    // poison the merge indefinitely. Excluding it is the same operation as the
    // subscriber-facing venue filter, which is why the per-level attribution
    // array pays for itself twice.
    if (live && fresh) {
      book.contributing.push_back(name);
      bid_inputs->push_back({static_cast<int>(i), books_[i].bids.levels()});
      ask_inputs->push_back({static_cast<int>(i), books_[i].asks.levels()});
    } else {
      book.stale.push_back(name);
    }

    VenueClockInfo clock;
    clock.name = name;
    clock.last_recv_ts_ns = last_recv_ns_[i];
    clock.last_exchange_ts_ns = last_exchange_ns_[i];
    clock.bid_levels = static_cast<int>(books_[i].bids.size());
    clock.ask_levels = static_cast<int>(books_[i].asks.size());
    clock.best_bid = books_[i].bids.BestPx();
    clock.best_ask = books_[i].asks.BestPx();
    book.clocks.push_back(std::move(clock));
  }
}

void Engine::Publish() {
  const std::int64_t now_steady = SteadyNowNs();

  auto book = std::make_shared<ConsolidatedBook>();
  book->instrument = config_.instrument;
  book->sequence = published_.fetch_add(1, std::memory_order_relaxed) + 1;
  book->publish_ts_ns = WallNowNs();
  book->ingest_recv_ts_ns = newest_recv_ns_;

  std::vector<VenueSideInput> bid_inputs;
  std::vector<VenueSideInput> ask_inputs;
  SelectContributingVenues(now_steady, book.get(), &bid_inputs, &ask_inputs);

  MergeLimits limits;
  limits.max_levels = config_.max_publish_levels;
  limits.max_bps_from_touch_e8 = static_cast<std::int64_t>(config_.max_publish_bps) * kScale;
  book->bids_truncated = MergeSide(true, bid_inputs, limits, &book->bids);
  book->asks_truncated = MergeSide(false, ask_inputs, limits, &book->asks);

  // Who is at the touch, read straight off the venue books. Three comparisons
  // per side, rather than 24 bytes of attribution on every published level.
  const Px best_bid = book->bids.empty() ? 0 : book->bids.front().px;
  const Px best_ask = book->asks.empty() ? 0 : book->asks.front().px;
  for (const VenueSideInput& input : bid_inputs) {
    const VenueBook& venue_book = books_[input.venue_index];
    if (best_bid != 0 && venue_book.bids.BestPx() == best_bid) {
      book->bid_touch.push_back({venues_[input.venue_index].name, venue_book.bids.BestQty()});
    }
  }
  for (const VenueSideInput& input : ask_inputs) {
    const VenueBook& venue_book = books_[input.venue_index];
    if (best_ask != 0 && venue_book.asks.BestPx() == best_ask) {
      book->ask_touch.push_back({venues_[input.venue_index].name, venue_book.asks.BestQty()});
    }
  }

  if (newest_recv_ns_ != 0) RecordLatency(now_steady - newest_recv_ns_);

  std::shared_ptr<const ConsolidatedBook> published = std::move(book);
  {
    std::lock_guard<std::mutex> lock(latest_mutex_);
    latest_ = published;
  }
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    for (const auto& slot : subscribers_) slot->Publish(published);
  }
}

void Engine::RecordLatency(std::int64_t nanos) {
  std::lock_guard<std::mutex> lock(latency_mutex_);
  latency_samples_[latency_cursor_] = nanos;
  latency_cursor_ = (latency_cursor_ + 1) % latency_samples_.size();
  latency_max_ = std::max(latency_max_, nanos);
  ++latency_count_;
}

LatencySummary Engine::Latency() const {
  std::lock_guard<std::mutex> lock(latency_mutex_);
  LatencySummary summary;
  summary.count = latency_count_;
  summary.max_ns = latency_max_;
  if (latency_count_ == 0) return summary;

  const std::size_t filled =
      std::min<std::size_t>(latency_count_, latency_samples_.size());
  std::vector<std::int64_t> sorted(latency_samples_.begin(),
                                   latency_samples_.begin() + static_cast<std::ptrdiff_t>(filled));
  std::sort(sorted.begin(), sorted.end());
  summary.p50_ns = sorted[filled / 2];
  summary.p99_ns = sorted[std::min(filled - 1, (filled * 99) / 100)];
  return summary;
}

}  // namespace md
