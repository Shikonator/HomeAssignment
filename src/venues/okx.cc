#include "src/venues/okx.h"

#include <iterator>

namespace md {

std::vector<std::string> OkxProtocol::SubscribeFrames() const {
  return {"{\"op\":\"subscribe\",\"args\":[{\"channel\":\"" + config_.channel +
          "\",\"instId\":\"" + config_.symbol + "\"}]}"};
}

void OkxProtocol::Reset() {
  staging_.clear();
  last_seq_id_ = 0;
  has_seq_ = false;
  synced_ = false;
}

FrameVerdict OkxProtocol::OnFrame(std::string_view frame, std::int64_t recv_ts_ns,
                                  std::vector<FeedUpdate>* out) {
  // The keepalive reply is the bare word "pong", not JSON.
  if (frame == "pong") return FrameVerdict::kOk;

  simdjson::dom::element root;
  if (!parser_.Parse(frame, &root)) return FrameVerdict::kParseError;

  // Subscription acknowledgements and errors carry "event" and no "action".
  std::string_view event;
  if (GetString(root["event"], &event)) {
    if (event != "error") return FrameVerdict::kOk;
    // A rejected subscription is not transient: an unknown instrument or a
    // channel needing auth will fail identically on every reconnect.
    std::string_view message;
    fatal_reason_ = GetString(root["msg"], &message) ? std::string(message)
                                                     : "okx rejected the subscription";
    return FrameVerdict::kFatal;
  }

  std::string_view action;
  if (!GetString(root["action"], &action)) return FrameVerdict::kOk;
  const bool is_snapshot = (action == "snapshot");
  // Anything that is neither is a channel behaviour we do not model. Ignoring
  // it is safe; treating it as a delta would not be.
  if (!is_snapshot && action != "update") return FrameVerdict::kOk;

  // Guard against a frame for an instrument we did not subscribe to. Moot with
  // one connection per subscription, but it is the bug that appears the day
  // someone multiplexes.
  std::string_view instrument;
  if (GetString(root["arg"]["instId"], &instrument) && instrument != config_.symbol) {
    return FrameVerdict::kOk;
  }

  simdjson::dom::array data;
  if (root["data"].get_array().get(data) != simdjson::SUCCESS) return FrameVerdict::kParseError;

  // Staged rather than written straight to `out`, so a failure on the second
  // entry cannot leave the caller holding half a batch.
  staging_.clear();
  std::int64_t next_seq_id = last_seq_id_;
  bool next_has_seq = has_seq_;

  for (simdjson::dom::element entry : data) {
    std::int64_t seq_id = 0;
    if (!GetI64(entry["seqId"], &seq_id)) return FrameVerdict::kParseError;

    if (!is_snapshot) {
      std::int64_t prev_seq_id = 0;
      if (!GetI64(entry["prevSeqId"], &prev_seq_id)) return FrameVerdict::kParseError;
      if (!synced_ || !next_has_seq) return FrameVerdict::kNeedsResync;

      // CONTINUITY IS CHECKED BEFORE the no-change shortcut, and the order is
      // load-bearing. OKX repeats the sequence number when nothing moved; if
      // that shortcut ran first, a gap followed by a no-change frame would skip
      // validation entirely and the book would diverge permanently with no
      // error. The OKX checksum used to be the backstop for exactly this class
      // of failure and it is deliberately not implemented, so this ordering is
      // the only thing standing behind it.
      if (prev_seq_id != next_seq_id) return FrameVerdict::kNeedsResync;
      if (prev_seq_id == seq_id) continue;  // nothing changed
    }

    FeedUpdate update;
    update.is_snapshot = is_snapshot;
    update.recv_ts_ns = recv_ts_ns;
    std::int64_t ts_ms = 0;
    if (GetI64(entry["ts"], &ts_ms)) update.exchange_ts_ns = ts_ms * 1'000'000;

    // OKX level entries are [price, size, deprecated, orderCount]; the trailing
    // fields are ignored.
    if (!ParseLevelArray(entry["bids"], &update.bids)) return FrameVerdict::kParseError;
    if (!ParseLevelArray(entry["asks"], &update.asks)) return FrameVerdict::kParseError;

    staging_.push_back(std::move(update));
    next_seq_id = seq_id;
    next_has_seq = true;
  }

  out->insert(out->end(), std::make_move_iterator(staging_.begin()),
              std::make_move_iterator(staging_.end()));
  staging_.clear();
  last_seq_id_ = next_seq_id;
  has_seq_ = next_has_seq;
  if (is_snapshot) synced_ = true;
  return FrameVerdict::kOk;
}

}  // namespace md
