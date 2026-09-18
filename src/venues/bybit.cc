#include "src/venues/bybit.h"

namespace md {

std::string BybitProtocol::Topic() const {
  return "orderbook." + std::to_string(config_.depth) + "." + config_.symbol;
}

std::vector<std::string> BybitProtocol::SubscribeFrames() const {
  return {"{\"op\":\"subscribe\",\"args\":[\"" + Topic() + "\"]}"};
}

void BybitProtocol::Reset() {
  staging_.clear();
  last_update_id_ = 0;
  synced_ = false;
}

FrameVerdict BybitProtocol::OnFrame(std::string_view frame, std::int64_t recv_ts_ns,
                                    std::vector<FeedUpdate>* out) {
  simdjson::dom::element root;
  if (!parser_.Parse(frame, &root)) return FrameVerdict::kParseError;

  // Subscribe and ping acknowledgements carry "op" and no "topic".
  std::string_view topic;
  if (!GetString(root["topic"], &topic)) {
    bool success = false;
    if (root["success"].get_bool().get(success) == simdjson::SUCCESS && !success) {
      // A rejected subscription will be rejected identically on every retry.
      std::string_view message;
      fatal_reason_ = GetString(root["ret_msg"], &message)
                          ? std::string(message)
                          : "bybit rejected the subscription";
      return FrameVerdict::kFatal;
    }
    return FrameVerdict::kOk;
  }
  if (topic != Topic()) return FrameVerdict::kOk;

  std::string_view type;
  if (!GetString(root["type"], &type)) return FrameVerdict::kParseError;
  const bool is_snapshot = (type == "snapshot");
  if (!is_snapshot && type != "delta") return FrameVerdict::kOk;

  simdjson::dom::element data;
  if (root["data"].get(data) != simdjson::SUCCESS) return FrameVerdict::kParseError;

  std::int64_t update_id = 0;
  if (!GetI64(data["u"], &update_id)) return FrameVerdict::kParseError;

  // A mid-stream snapshot follows Bybit's own internal reconnect. It replaces
  // the book rather than amending it, and resets sequencing.
  if (!is_snapshot) {
    if (!synced_) return FrameVerdict::kNeedsResync;
    if (update_id != last_update_id_ + 1) return FrameVerdict::kNeedsResync;
  }

  FeedUpdate update;
  update.is_snapshot = is_snapshot;
  update.recv_ts_ns = recv_ts_ns;
  std::int64_t ts_ms = 0;
  if (GetI64(root["ts"], &ts_ms)) update.exchange_ts_ns = ts_ms * 1'000'000;
  // Staged, so a malformed ask array cannot leave the caller holding bids that
  // were already appended.
  if (!ParseLevelArray(data["b"], &update.bids)) return FrameVerdict::kParseError;
  if (!ParseLevelArray(data["a"], &update.asks)) return FrameVerdict::kParseError;

  out->push_back(std::move(update));
  last_update_id_ = update_id;
  if (is_snapshot) synced_ = true;
  return FrameVerdict::kOk;
}

}  // namespace md
