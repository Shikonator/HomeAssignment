#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "src/net/json.h"
#include "src/venues/venue.h"

namespace md {

// Binance spot depth.
//
// Binance is the only one of the three that does not snapshot over the
// websocket: the local book is seeded from a REST call and then amended by the
// diff stream. Reconciling those two sources is the subtlest sequencing problem
// in the project, and the order matters -- buffering must begin BEFORE the REST
// call is issued, or updates that land during the round trip are lost and the
// book starts out silently wrong.
class BinanceProtocol final : public VenueProtocol {
 public:
  struct Config {
    std::string symbol = "BTCUSDT";
    std::string stream_url = "wss://stream.binance.com:9443/ws";
    std::string rest_url = "https://api.binance.com/api/v3/depth";
    // Depth of the seeding snapshot. 5000 is the deepest Binance offers and is
    // what the 50M notional band needs; it costs more request weight, which is
    // why the runner rate-limits resyncs rather than retrying freely.
    int snapshot_limit = 5000;
    // Cap on updates held during the REST round trip. Exceeding it means the
    // snapshot is taking implausibly long, and starting over beats growing an
    // unbounded buffer.
    std::size_t max_buffered_events = 4096;
  };

  explicit BinanceProtocol(Config config) : config_(std::move(config)) {}

  std::string_view name() const override { return "binance"; }
  std::string venue_symbol() const override { return config_.symbol; }
  std::string stream_url() const override;
  std::vector<std::string> SubscribeFrames() const override { return {}; }

  bool needs_rest_snapshot() const override { return true; }
  std::string RestSnapshotUrl() const override;
  FrameVerdict OnRestSnapshot(std::string_view body, std::int64_t recv_ts_ns,
                              std::vector<FeedUpdate>* out) override;

  bool synced() const override { return synced_; }
  FrameVerdict OnFrame(std::string_view frame, std::int64_t recv_ts_ns,
                       std::vector<FeedUpdate>* out) override;
  void Reset() override;

 private:
  struct PendingEvent {
    std::int64_t first_id = 0;
    std::int64_t final_id = 0;
    std::int64_t exchange_ts_ns = 0;
    std::vector<Level> bids;
    std::vector<Level> asks;
  };

  bool ParseDepthEvent(simdjson::dom::element root, PendingEvent* event) const;

  Config config_;
  JsonParser parser_;
  std::vector<FeedUpdate> staging_;
  std::deque<PendingEvent> buffered_;
  std::int64_t last_update_id_ = 0;
  bool synced_ = false;
};

}  // namespace md
