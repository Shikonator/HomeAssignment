#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "src/core/book.h"
#include "src/core/fixed.h"

namespace md {

// One book change set from one venue. Inherits the level-carrying part from
// core so ApplyFeedUpdate works on both the live path and the replay test.
struct FeedUpdate : FeedUpdateLevels {
  // The venue's clock: informational only, never trusted for ordering.
  std::int64_t exchange_ts_ns = 0;
  // Local monotonic receive time. All staleness decisions use this.
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

// Pure protocol logic for one venue: bytes in, book updates out.
//
// No sockets, no threads, no clock. Sequence validation, snapshot
// reconciliation and resync triggers all live here, so all of it is testable
// from recorded frames with no network. The runner owns every bit of I/O.
class VenueProtocol {
 public:
  virtual ~VenueProtocol() = default;

  virtual std::string_view name() const = 0;
  // The venue's own spelling (BTC-USDT on OKX, BTCUSDT elsewhere).
  virtual std::string venue_symbol() const = 0;
  // Overridable so tests and compose can point at a local mock.
  virtual std::string stream_url() const = 0;
  // Sent immediately after the websocket handshake.
  virtual std::vector<std::string> SubscribeFrames() const = 0;

  // Application-level keepalive; empty means none. NOT a websocket control
  // ping -- OKX wants the text "ping", Bybit wants {"op":"ping"}, and Beast's
  // built-in keepalive satisfies neither.
  virtual std::string KeepaliveFrame() const { return {}; }
  // Keepalive cadence, sent on a FIXED interval rather than only after silence.
  // Bybit wants a client ping every 20s whether or not data is flowing, so an
  // idle-gated ping never fires on a busy feed and the venue drops us. Do not
  // "optimise" this back to idle-based.
  virtual std::chrono::seconds keepalive_interval() const { return std::chrono::seconds(0); }

  // Binance seeds from REST; the others snapshot over the websocket.
  virtual bool needs_rest_snapshot() const { return false; }
  virtual std::string RestSnapshotUrl() const { return {}; }
  virtual FrameVerdict OnRestSnapshot(std::string_view body, std::int64_t recv_ts_ns,
                                      std::vector<FeedUpdate>* out) {
    (void)body;
    (void)recv_ts_ns;
    (void)out;
    return FrameVerdict::kOk;
  }

  // True once the local book is provably aligned with the venue's sequence.
  virtual bool synced() const = 0;

  // Handles one inbound text frame, appending zero or more updates.
  virtual FrameVerdict OnFrame(std::string_view frame, std::int64_t recv_ts_ns,
                               std::vector<FeedUpdate>* out) = 0;

  // Discards all sequencing state. Required after any verdict except kOk.
  virtual void Reset() = 0;

  // Human-readable reason a venue went kFatal, for GetVenueStatus.
  virtual std::string fatal_reason() const { return {}; }
};

}  // namespace md
