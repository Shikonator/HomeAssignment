#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/net/json.h"
#include "src/venues/venue.h"

namespace md {

// OKX v5 `books` channel: a 400-level snapshot over the websocket followed by
// incremental updates carrying seqId/prevSeqId.
//
// OKX also publishes a CRC32 checksum over the top 25 levels. This
// implementation deliberately does not verify it -- see README, "Why the OKX
// checksum is not verified". The short version: the checksum is computed over
// the venue's own decimal STRING rendering, which cannot be reproduced after
// normalising to fixed point, it only ever covered the top 25 levels (nowhere
// near the depth the volume bands consume), and a subtly wrong implementation
// resync-loops against a live venue. Divergence is instead covered offline, for
// all three venues, by the replay-versus-fresh-snapshot test.
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

  // OKX closes a connection silent for 30 seconds. The keepalive is a bare text
  // frame containing "ping", not a websocket control ping, so the
  // protocol-level keepalive would not satisfy it.
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
  // OKX itself uses seqId = -1 on some restarts, so -1 cannot double as "no
  // sequence seen yet". An explicit flag avoids the in-band sentinel.
  bool has_seq_ = false;
  bool synced_ = false;
  std::string fatal_reason_;
};

}  // namespace md
