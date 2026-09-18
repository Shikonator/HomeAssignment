#include "src/venues/binance.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace md {
namespace {

std::string Lowercase(std::string_view in) {
  std::string out(in);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

std::string BinanceProtocol::stream_url() const {
  return config_.stream_url + "/" + Lowercase(config_.symbol) + "@depth@100ms";
}

std::string BinanceProtocol::RestSnapshotUrl() const {
  return config_.rest_url + "?symbol=" + config_.symbol +
         "&limit=" + std::to_string(config_.snapshot_limit);
}

void BinanceProtocol::Reset() {
  staging_.clear();
  buffered_.clear();
  last_update_id_ = 0;
  synced_ = false;
}

bool BinanceProtocol::ParseDepthEvent(simdjson::dom::element root, PendingEvent* event) const {
  std::int64_t event_ms = 0;
  if (GetI64(root["E"], &event_ms)) event->exchange_ts_ns = event_ms * 1'000'000;
  if (!GetI64(root["U"], &event->first_id)) return false;
  if (!GetI64(root["u"], &event->final_id)) return false;
  if (!ParseLevelArray(root["b"], &event->bids)) return false;
  if (!ParseLevelArray(root["a"], &event->asks)) return false;
  return true;
}

FrameVerdict BinanceProtocol::OnFrame(std::string_view frame, std::int64_t recv_ts_ns,
                                      std::vector<FeedUpdate>* out) {
  simdjson::dom::element root;
  if (!parser_.Parse(frame, &root)) return FrameVerdict::kParseError;

  std::string_view event_type;
  if (!GetString(root["e"], &event_type)) {
    // Subscription acknowledgements carry "result"/"id" and no "e".
    return FrameVerdict::kOk;
  }
  if (event_type != "depthUpdate") return FrameVerdict::kOk;

  std::string_view symbol;
  if (GetString(root["s"], &symbol) && symbol != config_.symbol) return FrameVerdict::kOk;

  PendingEvent event;
  if (!ParseDepthEvent(root, &event)) return FrameVerdict::kParseError;

  // Before the snapshot lands, every event is held rather than applied. This
  // buffering must already be running when the REST request goes out.
  if (!synced_) {
    if (buffered_.size() >= config_.max_buffered_events) {
      // Clear as we fail. A stateful failure left in place returns the same
      // verdict on every subsequent frame, so a runner that forgot to Reset()
      // would livelock instead of recovering.
      buffered_.clear();
      return FrameVerdict::kNeedsResync;
    }
    buffered_.push_back(std::move(event));
    return FrameVerdict::kOk;
  }

  // Binance guarantees consecutive events satisfy U == previous u + 1. Any
  // other value means at least one event was lost, and applying this one would
  // leave the book permanently and undetectably wrong.
  if (event.first_id != last_update_id_ + 1) return FrameVerdict::kNeedsResync;

  FeedUpdate update;
  update.is_snapshot = false;
  update.bids = std::move(event.bids);
  update.asks = std::move(event.asks);
  update.exchange_ts_ns = event.exchange_ts_ns;
  update.recv_ts_ns = recv_ts_ns;
  out->push_back(std::move(update));
  last_update_id_ = event.final_id;
  return FrameVerdict::kOk;
}

FrameVerdict BinanceProtocol::OnRestSnapshot(std::string_view body, std::int64_t recv_ts_ns,
                                             std::vector<FeedUpdate>* out) {
  simdjson::dom::element root;
  if (!parser_.Parse(body, &root)) return FrameVerdict::kParseError;

  std::int64_t snapshot_id = 0;
  if (!GetI64(root["lastUpdateId"], &snapshot_id)) return FrameVerdict::kParseError;

  // Staged, not appended. Reconciliation below can still fail, and handing the
  // caller a snapshot alongside a failure verdict is the exact shape staging
  // exists to prevent -- one message instead of many is no better.
  staging_.clear();
  FeedUpdate snapshot;
  snapshot.is_snapshot = true;
  snapshot.recv_ts_ns = recv_ts_ns;
  if (!ParseLevelArray(root["bids"], &snapshot.bids)) return FrameVerdict::kParseError;
  if (!ParseLevelArray(root["asks"], &snapshot.asks)) return FrameVerdict::kParseError;
  staging_.push_back(std::move(snapshot));

  // Anything wholly older than the snapshot is already reflected in it.
  while (!buffered_.empty() && buffered_.front().final_id <= snapshot_id) {
    buffered_.pop_front();
  }

  // The first surviving event must straddle the snapshot: U <= id+1 <= u. If it
  // starts after id+1 there is a hole between the snapshot and the stream, and
  // the only correct response is to fetch a newer snapshot.
  if (!buffered_.empty()) {
    const PendingEvent& first = buffered_.front();
    if (!(first.first_id <= snapshot_id + 1 && snapshot_id + 1 <= first.final_id)) {
      buffered_.clear();
      return FrameVerdict::kNeedsResync;
    }
  }

  std::int64_t applied_through = snapshot_id;
  bool first = true;
  for (PendingEvent& event : buffered_) {
    // Continuity within the replayed buffer, same rule as the live path. An
    // explicit first-iteration flag rather than comparing against snapshot_id:
    // that comparison happens to work only because the drop loop above
    // guarantees every surviving event has final_id > snapshot_id, and relying
    // on an invariant established ten lines away in a different loop means the
    // next edit to that loop silently disables continuity checking here.
    if (!first && event.first_id != applied_through + 1) {
      buffered_.clear();
      return FrameVerdict::kNeedsResync;
    }
    first = false;
    FeedUpdate update;
    update.is_snapshot = false;
    update.bids = std::move(event.bids);
    update.asks = std::move(event.asks);
    update.exchange_ts_ns = event.exchange_ts_ns;
    update.recv_ts_ns = recv_ts_ns;
    staging_.push_back(std::move(update));
    applied_through = event.final_id;
  }
  out->insert(out->end(), std::make_move_iterator(staging_.begin()),
              std::make_move_iterator(staging_.end()));
  staging_.clear();

  buffered_.clear();
  last_update_id_ = applied_through;
  synced_ = true;
  return FrameVerdict::kOk;
}

}  // namespace md
