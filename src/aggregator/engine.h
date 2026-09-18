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

// Each subscriber owns one of these. See ConflatingSlot for why overwriting is
// the right policy in this direction and the wrong one upstream.
using BookSlot = ConflatingSlot<ConsolidatedBook>;

struct LatencySummary {
  std::int64_t p50_ns = 0;
  std::int64_t p99_ns = 0;
  std::int64_t max_ns = 0;
  std::uint64_t count = 0;
};

// What the engine needs from a venue: a name, a way to read its health, and a
// ring to drain. Deliberately NOT a VenueRunner -- the engine has no business
// knowing about sockets or reconnection, and this way the engine can be tested
// by pushing batches straight into a ring with no network at all.
struct VenueFeed {
  std::string name;
  std::string venue_symbol;
  const VenueStats* stats = nullptr;
  FeedRing* ring = nullptr;
};

struct EngineConfig {
  std::string instrument = "BTCUSDT";

  // Backstop on published depth. The venue books are kept full-depth
  // internally; this only bounds what goes on the wire. Sized to publish the
  // whole merged ladder in practice -- roughly 5500 levels per side across the
  // three venues, at 48 bytes each, is about 260KB.
  int max_publish_levels = 8192;

  // A venue silent for longer than this is excluded from the merge. Measured on
  // LOCAL receive time; an exchange's own clock is not ours to trust.
  std::chrono::milliseconds staleness_timeout{5000};

  // How long the aggregation thread sleeps when every ring is empty.
  std::chrono::microseconds idle_poll{200};

  // Republish at least this often even with no inbound data, so that venue
  // staleness and a fully-empty book are observable by subscribers rather than
  // merely representable. Costs one snapshot per interval on an idle book.
  std::chrono::milliseconds heartbeat_interval{1000};
};

// Owns the consolidated book.
//
// Exactly ONE thread ever touches the venue books: this engine's. Venue threads
// only parse and hand deltas over a lock-free ring, so the expensive work
// (JSON) happens in parallel while the book itself needs no synchronisation at
// all. Publication produces an immutable snapshot that subscribers share.
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

  // Most recent published state, for unary calls and for a subscriber's first
  // message so it does not have to wait for the next tick.
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
