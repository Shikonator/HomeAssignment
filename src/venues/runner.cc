#include "src/venues/runner.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "src/common/log.h"
#include "src/core/clock.h"

namespace md {
namespace {

// Minimal JSON string escaping, for the frame recorder. Frames are stored as
// JSON STRINGS rather than embedded objects so that a single oversized integer
// literal somewhere in a venue payload cannot make a strict reader reject the
// whole recording.
std::string JsonEscape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (const char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

}  // namespace

VenueRunner::VenueRunner(int venue_index, std::unique_ptr<VenueProtocol> protocol, FeedRing* ring,
                         Options options)
    : venue_index_(venue_index),
      protocol_(std::move(protocol)),
      ring_(ring),
      options_(std::move(options)),
      tls_(MakeTlsContext(options_.ca_file)),
      reconnect_timer_(io_),
      watchdog_timer_(io_),
      backoff_(options_.reconnect_initial, options_.reconnect_max),
      resync_budget_(options_.max_resyncs_per_minute, std::chrono::minutes(1)),
      last_activity_(std::chrono::steady_clock::now()) {
  if (!options_.record_path.empty()) {
    recorder_.open(options_.record_path, std::ios::out | std::ios::trunc);
  }
}

VenueRunner::~VenueRunner() { Stop(); }

void VenueRunner::Start() {
  thread_ = std::thread([this] {
    auto guard = boost::asio::make_work_guard(io_);
    boost::asio::post(io_, [this] { Connect(); });
    io_.run();
  });
}

void VenueRunner::Stop() {
  if (stopping_.exchange(true)) return;
  boost::asio::post(io_, [this] {
    reconnect_timer_.cancel();
    watchdog_timer_.cancel();
    if (connection_) connection_->Close();
    // Stopping the context here means async_close's handler almost certainly
    // never runs and the socket dies abruptly. That is deliberate: a graceful
    // websocket close can hang on an unresponsive peer, and a container that
    // ignores SIGTERM for ten seconds is worse than an abrupt FIN.
    io_.stop();
  });
  if (thread_.joinable()) thread_.join();
  if (recorder_.is_open()) recorder_.close();
}

void VenueRunner::SetState(VenueState state) {
  stats_.state.store(static_cast<int>(state), std::memory_order_relaxed);
}

void VenueRunner::Connect() {
  if (stopping_.load() || stats_.fatal.load()) return;
  logged_live_ = false;
  SetState(VenueState::kConnecting);
  protocol_->Reset();

  WsConnection::Callbacks callbacks;
  callbacks.on_open = [this] { OnOpen(); };
  callbacks.on_frame = [this](std::string_view frame) { OnFrame(frame); };
  const std::uint64_t generation = ++reconnect_generation_;
  callbacks.on_close = [this, generation](const std::string& reason) {
    // Ignore a late close from a connection we already replaced, so one
    // disconnect can never schedule two reconnects.
    if (generation != reconnect_generation_) return;
    OnClose(reason);
  };

  Log(protocol_->name().data(), "connecting to " + protocol_->stream_url());
  connection_ = std::make_shared<WsConnection>(io_, tls_, std::move(callbacks));
  connection_->Open(protocol_->stream_url());
  last_activity_ = std::chrono::steady_clock::now();
  ArmWatchdog();
}

void VenueRunner::ArmWatchdog() {
  if (stopping_.load()) return;
  watchdog_timer_.expires_after(options_.silence_timeout / 2);
  watchdog_timer_.async_wait([this](boost::beast::error_code ec) {
    if (ec || stopping_.load() || stats_.fatal.load()) return;
    const auto silent_for = std::chrono::steady_clock::now() - last_activity_;
    if (silent_for >= options_.silence_timeout) {
      SetState(VenueState::kStale);
      if (connection_) connection_->Close();
      return;  // OnClose schedules the reconnect
    }
    ArmWatchdog();
  });
}

// Backoff is reset only once a venue is genuinely working. Resetting on the
// reconnect ATTEMPT -- or even on OnOpen -- means an endpoint that accepts the
// TCP connection and then drops us a second later, which is exactly what a
// rate-limited venue does, would reset the backoff on every cycle and never
// actually back off.
void VenueRunner::MarkHealthy() {
  if (!logged_live_) {
    logged_live_ = true;
    Log(protocol_->name().data(), "live");
  }
  backoff_.Reset();
}

void VenueRunner::OnOpen() {
  Log(protocol_->name().data(), "websocket open, subscribing");
  SetState(VenueState::kSyncing);
  for (const std::string& frame : protocol_->SubscribeFrames()) {
    connection_->Send(frame);
  }
  connection_->StartKeepalive(protocol_->KeepaliveFrame(), protocol_->keepalive_idle());

  // The snapshot request is issued only AFTER the read loop is running, so
  // updates arriving during the round trip are already being buffered by the
  // protocol. Fetching first is the classic way to start with a wrong book.
  if (protocol_->needs_rest_snapshot()) FetchSnapshot();
}

void VenueRunner::FetchSnapshot() {
  if (stopping_.load()) return;
  if (!ConsumeResyncToken()) {
    // Out of budget. Back off rather than hammering a rate-limited endpoint.
    ScheduleReconnect();
    return;
  }
  HttpGet(io_, tls_, protocol_->RestSnapshotUrl(),
          [this](bool ok, std::string body, std::string error) {
            if (stopping_.load()) return;
            if (!ok) {
              OnClose("snapshot failed: " + error);
              return;
            }
            Log(protocol_->name().data(), "rest snapshot received");
            const std::int64_t recv = SteadyNowNs();
            Record(recv, body, /*is_rest_snapshot=*/true);
            scratch_.clear();
            const FrameVerdict verdict = protocol_->OnRestSnapshot(body, recv, &scratch_);
            if (verdict != FrameVerdict::kOk) {
              scratch_.clear();
              Resync("snapshot reconciliation failed");
              return;
            }
            Publish(&scratch_);
            if (protocol_->synced()) {
              SetState(VenueState::kLive);
              MarkHealthy();
            }
          });
}

void VenueRunner::OnFrame(std::string_view frame) {
  const std::int64_t recv = SteadyNowNs();
  last_activity_ = std::chrono::steady_clock::now();
  stats_.messages.fetch_add(1, std::memory_order_relaxed);
  stats_.last_recv_ts_ns.store(recv, std::memory_order_relaxed);
  Record(recv, frame, /*is_rest_snapshot=*/false);

  scratch_.clear();
  const FrameVerdict verdict = protocol_->OnFrame(frame, recv, &scratch_);
  switch (verdict) {
    case FrameVerdict::kOk:
      break;
    case FrameVerdict::kNeedsResync:
      scratch_.clear();
      stats_.sequence_gaps.fetch_add(1, std::memory_order_relaxed);
      Resync("sequence gap");
      return;
    case FrameVerdict::kParseError:
      scratch_.clear();
      stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
      Resync("parse error");
      return;
    case FrameVerdict::kFatal:
      // Not transient. Reconnecting would fail identically forever while
      // presenting as a network problem.
      scratch_.clear();
      Log(protocol_->name().data(), "FATAL: " + protocol_->fatal_reason());
      stats_.fatal.store(true, std::memory_order_relaxed);
      SetState(VenueState::kDisconnected);
      if (connection_) connection_->Close();
      return;
  }

  if (!scratch_.empty()) {
    if (scratch_.back().exchange_ts_ns != 0) {
      stats_.last_exchange_ts_ns.store(scratch_.back().exchange_ts_ns, std::memory_order_relaxed);
    }
    Publish(&scratch_);
    if (protocol_->synced()) {
      SetState(VenueState::kLive);
      MarkHealthy();
    }
  }
}

void VenueRunner::Publish(std::vector<FeedUpdate>* updates) {
  if (updates->empty()) return;
  FeedBatch batch;
  batch.venue_index = venue_index_;
  batch.updates = std::move(*updates);
  updates->clear();

  if (!ring_->Push(std::move(batch))) {
    // The ring carries DELTAS. Dropping one silently and permanently corrupts
    // the book, and blocking the venue thread would back up the receive buffer
    // until the exchange disconnects us -- turning a hiccup into an outage. The
    // only safe response is to discard the batch and rebuild from a snapshot.
    stats_.ring_overflows.fetch_add(1, std::memory_order_relaxed);
    Resync("ingest ring full");
  }
}

void VenueRunner::Resync(const char* why) {
  Log(protocol_->name().data(), std::string("resync: ") + why);
  stats_.resyncs.fetch_add(1, std::memory_order_relaxed);
  SetState(VenueState::kSyncing);
  protocol_->Reset();

  if (protocol_->needs_rest_snapshot()) {
    // The diff stream is still flowing and is being buffered again, so a fresh
    // snapshot is enough; tearing down the socket would be strictly more work.
    FetchSnapshot();
    return;
  }
  // Venues that snapshot over the websocket resync by reconnecting, which is
  // the only way to make them send a new snapshot.
  if (connection_) connection_->Close();
}

void VenueRunner::OnClose(const std::string& reason) {
  Log(protocol_->name().data(), "disconnected: " + reason);
  SetState(VenueState::kDisconnected);
  if (stopping_.load() || stats_.fatal.load()) return;
  ScheduleReconnect();
}

void VenueRunner::ScheduleReconnect() {
  if (stopping_.load() || stats_.fatal.load()) return;
  reconnect_timer_.expires_after(backoff_.Next());
  reconnect_timer_.async_wait([this](boost::beast::error_code ec) {
    if (ec || stopping_.load()) return;
    Connect();
  });
}

bool VenueRunner::ConsumeResyncToken() {
  return resync_budget_.TryConsume(std::chrono::steady_clock::now());
}

void VenueRunner::Record(std::int64_t recv_ts_ns, std::string_view frame, bool is_rest_snapshot) {
  if (!recorder_.is_open()) return;
  recorder_ << R"({"recv_ts_ns":)" << recv_ts_ns << R"(,"kind":")"
            << (is_rest_snapshot ? "rest_snapshot" : "ws_frame") << R"(","frame":")"
            << JsonEscape(frame) << "\"}\n";
}

}  // namespace md
