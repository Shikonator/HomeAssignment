#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/net/json.h"
#include "src/venues/venue.h"

namespace md {

// Bybit v5 spot orderbook: a snapshot over the websocket followed by deltas,
// sequenced by a monotonic `u`.
//
// Bybit may push a fresh snapshot mid-stream at any time, after its own
// internal reconnects. That is not an error and must not be diffed against the
// existing book -- it replaces it wholesale.
class BybitProtocol final : public VenueProtocol {
 public:
  struct Config {
    std::string symbol = "BTCUSDT";
    std::string stream_url = "wss://stream.bybit.com/v5/public/spot";
    int depth = 200;
  };

  explicit BybitProtocol(Config config) : config_(std::move(config)) {}

  std::string_view name() const override { return "bybit"; }
  std::string venue_symbol() const override { return config_.symbol; }
  std::string stream_url() const override { return config_.stream_url; }
  std::vector<std::string> SubscribeFrames() const override;

  std::string KeepaliveFrame() const override { return "{\"op\":\"ping\"}"; }
  std::chrono::seconds keepalive_interval() const override { return std::chrono::seconds(20); }

  bool synced() const override { return synced_; }
  FrameVerdict OnFrame(std::string_view frame, std::int64_t recv_ts_ns,
                       std::vector<FeedUpdate>* out) override;
  void Reset() override;
  std::string fatal_reason() const override { return fatal_reason_; }

 private:
  std::string Topic() const;

  Config config_;
  JsonParser parser_;
  std::vector<FeedUpdate> staging_;
  std::int64_t last_update_id_ = 0;
  bool synced_ = false;
  std::string fatal_reason_;
};

}  // namespace md
