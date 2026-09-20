#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/net/json.h"
#include "src/venues/venue.h"

namespace md {

// OKX v5 `books`: a 400-level snapshot over the socket, then seqId/prevSeqId
// updates.
//
// The CRC32 checksum OKX publishes is deliberately NOT verified: it is computed
// over the venue's decimal string rendering, which cannot be reproduced after
// normalising to fixed point, and covers only the top 25 levels. Divergence is
// covered offline for all three venues by the replay test. See README.
class OkxProtocol final : public VenueProtocol {
 public:
  struct Config {
    std::string symbol = "BTC-USDT";
    std::string stream_url = "wss://ws.okx.com:8443/ws/v5/public";
    std::string channel = "books";
  };

  explicit OkxProtocol(Config config) : config_(std::move(config)) {}

  std::string_view name() const override { return "okx"; }
  std::string venue_symbol() const override { return config_.symbol; }
  std::string stream_url() const override { return config_.stream_url; }
  std::vector<std::string> SubscribeFrames() const override;

  // A bare text "ping", not a websocket control ping.
  std::string KeepaliveFrame() const override { return "ping"; }
  std::chrono::seconds keepalive_interval() const override { return std::chrono::seconds(20); }

  bool synced() const override { return synced_; }
  FrameVerdict OnFrame(std::string_view frame, std::int64_t recv_ts_ns,
                       std::vector<FeedUpdate>* out) override;
  void Reset() override;
  std::string fatal_reason() const override { return fatal_reason_; }

 private:
  Config config_;
  JsonParser parser_;
  std::vector<FeedUpdate> staging_;
  std::int64_t last_seq_id_ = 0;
  // OKX itself uses seqId = -1 on some restarts, so it cannot double as a
  // "nothing seen yet" sentinel.
  bool has_seq_ = false;
  bool synced_ = false;
  std::string fatal_reason_;
};

}  // namespace md
