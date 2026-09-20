#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "src/core/book.h"
#include "src/core/fixed.h"

namespace md {

// Inherits the level-carrying part from core so ApplyFeedUpdate serves both
// the live path and the replay test.
struct FeedUpdate : FeedUpdateLevels {
  // The venue's clock: informational, never trusted for ordering.
  std::int64_t exchange_ts_ns = 0;
  // All staleness decisions use this, never the venue's clock.
  std::int64_t recv_ts_ns = 0;
};

// On any verdict except kOk: `out` is unspecified and the caller must discard
// it, and the caller must Reset() before the next frame. The adapters also
// stage internally so a partial batch never escapes.
enum class FrameVerdict {
  kOk,           // consumed; zero or more updates appended
  kNeedsResync,  // sequencing broke; rebuild the book from a fresh snapshot
  kParseError,   // malformed payload; counted separately, handled as a resync
  kFatal,        // permanently broken. Reconnecting cannot fix it.
};

// Bytes in, book updates out.
//
// NO SOCKETS, NO THREADS, NO CLOCK. Sequence validation, snapshot
// reconciliation and resync triggers all live here, which is what makes them
// testable from recorded frames. The runner owns every bit of I/O.
class VenueProtocol {
 public:
  virtual ~VenueProtocol() = default;

  virtual std::string_view name() const = 0;
  // BTC-USDT on OKX, BTCUSDT elsewhere.
  virtual std::string venue_symbol() const = 0;
  // Overridable so tests can point at a local mock.
  virtual std::string stream_url() const = 0;
  virtual std::vector<std::string> SubscribeFrames() const = 0;

  // NOT a websocket control ping: OKX wants the text "ping", Bybit wants
  // {"op":"ping"}, and Beast's built-in keepalive satisfies neither.
  virtual std::string KeepaliveFrame() const { return {}; }
  // FIXED interval, not idle-gated. Bybit wants a ping every 20s whether or
  // not data flows, so an idle-gated ping never fires on a busy feed and the
  // venue drops us. Do not "optimise" this back.
  virtual std::chrono::seconds keepalive_interval() const { return std::chrono::seconds(0); }

  // Binance seeds from REST; the others snapshot over the socket.
  virtual bool needs_rest_snapshot() const { return false; }
  virtual std::string RestSnapshotUrl() const { return {}; }
  virtual FrameVerdict OnRestSnapshot(std::string_view body, std::int64_t recv_ts_ns,
                                      std::vector<FeedUpdate>* out) {
    (void)body;
    (void)recv_ts_ns;
    (void)out;
    return FrameVerdict::kOk;
  }

  virtual bool synced() const = 0;

  virtual FrameVerdict OnFrame(std::string_view frame, std::int64_t recv_ts_ns,
                               std::vector<FeedUpdate>* out) = 0;

  // Required after any verdict except kOk.
  virtual void Reset() = 0;

  virtual std::string fatal_reason() const { return {}; }
};

}  // namespace md
