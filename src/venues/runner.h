#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>

#include "src/core/retry.h"
#include "src/core/spsc_ring.h"
#include "src/net/http_client.h"
#include "src/net/ws_client.h"
#include "src/venues/venue.h"

namespace md {

// Mirrors md.v1.ConnState.
enum class VenueState : int {
  kDisconnected = 1,
  kConnecting = 2,
  kSyncing = 3,
  kLive = 4,
  kStale = 5,
};

// Written by the venue thread, read by the aggregator and by GetVenueStatus.
// Relaxed atomics throughout: these are observability counters, and ordering
// between them carries no meaning.
struct VenueStats {
  std::atomic<int> state{static_cast<int>(VenueState::kDisconnected)};
  std::atomic<std::uint64_t> messages{0};
  std::atomic<std::uint64_t> resyncs{0};
  std::atomic<std::uint64_t> sequence_gaps{0};
  std::atomic<std::uint64_t> ring_overflows{0};
  std::atomic<std::uint64_t> parse_errors{0};
  std::atomic<std::int64_t> last_recv_ts_ns{0};
  std::atomic<std::int64_t> last_exchange_ts_ns{0};
  std::atomic<bool> fatal{false};
};

// One frame's worth of book changes, moved across the ring in a single push.
struct FeedBatch {
  int venue_index = 0;
  std::vector<FeedUpdate> updates;
};

using FeedRing = SpscRing<FeedBatch>;

// Owns one venue's connection, its io_context and the thread driving it.
//
// The runner knows nothing about any venue's wire format; that lives entirely
// in VenueProtocol. What it owns is everything stateful about being connected:
// reconnection with backoff, resync policy, rate limiting and statistics.
class VenueRunner {
 public:
  struct Options {
    std::string ca_file;
    std::chrono::milliseconds reconnect_initial{500};
    std::chrono::milliseconds reconnect_max{30000};

    // Binance's depth-5000 snapshot costs ~250 of a 6000/minute request-weight
    // budget. An unthrottled resync loop would get the aggregator rate-limited
    // and then banned, which looks exactly like a network outage. Six per
    // minute leaves roughly an order of magnitude of headroom.
    int max_resyncs_per_minute = 6;

    // Force a reconnect after this much total silence. A TCP flow dropped by a
    // NAT or middlebox produces no FIN, no RST and no data, so async_read never
    // completes and never errors: without this the venue sits "live" forever
    // with a frozen book while the system reports itself healthy. BTCUSDT is
    // never genuinely quiet for this long on any of these venues, so silence is
    // an unambiguous signal.
    std::chrono::seconds silence_timeout{30};

  };

  VenueRunner(int venue_index, std::unique_ptr<VenueProtocol> protocol, FeedRing* ring,
              Options options);
  ~VenueRunner();

  VenueRunner(const VenueRunner&) = delete;
  VenueRunner& operator=(const VenueRunner&) = delete;

  void Start();
  void Stop();

  int venue_index() const { return venue_index_; }
  std::string name() const { return std::string(protocol_->name()); }
  std::string venue_symbol() const { return protocol_->venue_symbol(); }
  const VenueStats& stats() const { return stats_; }
  std::string fatal_reason() const { return protocol_->fatal_reason(); }

 private:
  void Connect();
  void OnOpen();
  void OnFrame(std::string_view frame);
  void OnClose(const std::string& reason);
  void FetchSnapshot();
  void Resync(const char* why);
  void ScheduleReconnect();
  void ArmWatchdog();
  void MarkHealthy();
  void Publish(std::vector<FeedUpdate>* updates);
  bool ConsumeResyncToken();
  void SetState(VenueState state);

  const int venue_index_;
  std::unique_ptr<VenueProtocol> protocol_;
  FeedRing* const ring_;
  Options options_;

  boost::asio::io_context io_;
  boost::asio::ssl::context tls_;
  boost::asio::steady_timer reconnect_timer_;
  boost::asio::steady_timer watchdog_timer_;
  std::shared_ptr<WsConnection> connection_;
  std::thread thread_;

  VenueStats stats_;
  ExponentialBackoff backoff_;
  RateBudget resync_budget_;
  std::chrono::steady_clock::time_point last_activity_;
  std::vector<FeedUpdate> scratch_;
  std::atomic<bool> stopping_{false};
  std::uint64_t reconnect_generation_ = 0;
  bool logged_live_ = false;
};

}  // namespace md
