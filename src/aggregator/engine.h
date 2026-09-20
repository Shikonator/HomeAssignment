#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/core/analytics.h"
#include "src/core/book.h"
#include "src/core/conflating_slot.h"
#include "src/core/consolidated.h"
#include "src/venues/runner.h"

namespace md {

// One per subscriber.
using BookSlot = ConflatingSlot<ConsolidatedBook>;

struct LatencySummary {
  std::int64_t p50_ns = 0;
  std::int64_t p99_ns = 0;
  std::int64_t max_ns = 0;
  std::uint64_t count = 0;
};

// Deliberately not a VenueRunner: the engine has no business knowing about
// sockets, and this way it is testable by pushing batches into a ring.
struct VenueFeed {
  std::string name;
  std::string venue_symbol;
  const VenueStats* stats = nullptr;
  FeedRing* ring = nullptr;
};

struct EngineConfig {
  std::string instrument = "BTCUSDT";

  // The bound that makes published numbers reproducible; see MergeLimits.
  // Generous for BTCUSDT, where a REST snapshot spans about 100 bps.
  int max_publish_bps = 500;

  // Backstop only; venue books stay full-depth internally.
  int max_publish_levels = 8192;

  // Excluded from the merge past this. Measured on LOCAL receive time.
  std::chrono::milliseconds staleness_timeout{5000};

  // How long the aggregation thread sleeps when every ring is empty.
  std::chrono::microseconds idle_poll{200};

  // Republish even with no inbound data, so staleness and an empty book are
  // observable rather than merely representable.
  std::chrono::milliseconds heartbeat_interval{1000};
};

// Owns the consolidated book. Exactly ONE thread touches the venue books:
// this one. Venue threads only parse and hand deltas over a ring, so the
// expensive work happens in parallel and the books need no synchronisation.
class Engine {
 public:
  Engine(EngineConfig config, std::vector<VenueFeed> venues);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void Start();
  void Stop();

  std::shared_ptr<BookSlot> AddSubscriber();
  void RemoveSubscriber(const std::shared_ptr<BookSlot>& slot);

  // Lets a new subscriber get a message without waiting for the next tick.
  std::shared_ptr<const ConsolidatedBook> Latest() const;

  LatencySummary Latency() const;
  std::uint64_t published() const { return published_.load(std::memory_order_relaxed); }
  const std::vector<VenueFeed>& venues() const { return venues_; }
  const EngineConfig& config() const { return config_; }
  std::int64_t start_wall_ns() const { return start_wall_ns_; }

 private:
  void Run();
  void Apply(FeedBatch* batch);
  void Publish();
  void SelectContributingVenues(std::int64_t now_steady, ConsolidatedBook* book,
                                std::vector<VenueSideInput>* bid_inputs,
                                std::vector<VenueSideInput>* ask_inputs);
  void RecordLatency(std::int64_t nanos);

  EngineConfig config_;
  std::vector<VenueFeed> venues_;

  // Indexed by venue. Touched only by the engine thread.
  std::vector<VenueBook> books_;
  std::vector<std::int64_t> last_recv_ns_;
  std::vector<std::int64_t> last_exchange_ns_;
  std::int64_t newest_recv_ns_ = 0;

  std::chrono::steady_clock::time_point last_publish_{};
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> published_{0};
  std::int64_t start_wall_ns_ = 0;

  mutable std::mutex subscribers_mutex_;
  std::vector<std::shared_ptr<BookSlot>> subscribers_;

  mutable std::mutex latest_mutex_;
  std::shared_ptr<const ConsolidatedBook> latest_;

  mutable std::mutex latency_mutex_;
  std::vector<std::int64_t> latency_samples_;
  std::size_t latency_cursor_ = 0;
  std::int64_t latency_max_ = 0;
  std::uint64_t latency_count_ = 0;
};

}  // namespace md
