#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "src/core/book.h"
#include "src/core/fixed.h"

namespace md {

// One book change set from one venue.
//
// Inherits the level-carrying part from core so that ApplyFeedUpdate -- the
// single implementation of "fold an update into a book" -- works on both the
// live path and the offline replay test.
struct FeedUpdate : FeedUpdateLevels {
  // The venue's own clock. Reproduced for information; never used for ordering
  // or staleness, because it is not our clock and we cannot bound its skew.
  std::int64_t exchange_ts_ns = 0;
  // Local monotonic receive time. This is what staleness decisions use.
  std::int64_t recv_ts_ns = 0;
};

// CONTRACT FOR EVERY VERDICT OTHER THAN kOk:
//   1. `out` is UNSPECIFIED and the caller must discard whatever is in it. The
//      adapters also enforce this structurally, by staging into a local buffer
//      and appending to `out` only on the success path -- a contract that is
//      only documented gets violated by the next edit.
//   2. The caller MUST call Reset() before feeding another frame. A stateful
//      failure (an overflowed buffer, a broken sequence) is still present
//      afterwards, so feeding the next frame without resetting returns the same
//      failure forever.
enum class FrameVerdict {
  // Frame consumed. Zero or more updates were appended.
  kOk,
  // Sequencing broke, or the venue told us to start over. The book must be
  // discarded and rebuilt from a fresh snapshot; applying anything further
  // would silently diverge.
  kNeedsResync,
  // Malformed payload. Treated like kNeedsResync by the runner but counted
  // separately, because a parse error is our bug and a gap is the network's.
  kParseError,
  // Permanently broken: an unknown symbol, a delisted instrument, a channel
  // requiring auth. Reconnecting cannot fix it, so the runner marks the venue
  // down and stops rather than looping forever while presenting as a network
  // problem. The venue's own error text is surfaced in GetVenueStatus.
  kFatal,
};

// Pure protocol logic for one venue: bytes in, book updates out.
//
// Deliberately contains no sockets, no threads and no clock. Everything that
// makes a venue hard -- sequence validation, snapshot reconciliation, resync
// triggers -- lives here and is therefore testable by feeding it recorded
// frames with no network involved. The runner owns all I/O and knows nothing
// about any venue's wire format.
class VenueProtocol {
 public:
  virtual ~VenueProtocol() = default;

  virtual std::string_view name() const = 0;
  // The venue's own spelling of the instrument (BTC-USDT on OKX, BTCUSDT
  // elsewhere), surfaced so status output is auditable against the venue.
  virtual std::string venue_symbol() const = 0;

  // Overridable so the end-to-end suite and docker-compose can point the same
  // code at a local mock instead of the public endpoint.
  virtual std::string stream_url() const = 0;

  // Sent immediately after the websocket handshake.
  virtual std::vector<std::string> SubscribeFrames() const = 0;

  // Application-level keepalive. Empty means the venue needs none.
  //
  // These are NOT websocket control pings. OKX requires a text frame
  // containing "ping" and Bybit requires {"op":"ping"}; a protocol-level ping
  // satisfies neither, so Beast's built-in keepalive cannot be relied on here.
  virtual std::string KeepaliveFrame() const { return {}; }
  // Send a keepalive only after this much silence. Inbound book updates count
  // as traffic, so on a live feed almost no keepalives are actually sent.
  virtual std::chrono::seconds keepalive_idle() const { return std::chrono::seconds(0); }

  // Binance seeds its book from REST; the others snapshot over the websocket.
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

  // Discards all sequencing state ahead of a fresh subscribe. Required after
  // any verdict other than kOk; see the contract above.
  virtual void Reset() = 0;

  // Human-readable reason a venue went kFatal, for GetVenueStatus.
  virtual std::string fatal_reason() const { return {}; }
};

}  // namespace md
