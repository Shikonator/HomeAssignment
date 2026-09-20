#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <algorithm>
#include <vector>
#include <string>

#include "google/protobuf/util/json_util.h"
#include "proto/md/v1/bbo.grpc.pb.h"
#include "proto/md/v1/price_bands.grpc.pb.h"
#include "proto/md/v1/status.grpc.pb.h"
#include "proto/md/v1/volume_bands.grpc.pb.h"
#include "src/common/flags.h"
#include "src/core/clock.h"
#include "src/core/fixed.h"

namespace md {

// Shared plumbing for the three publisher services. They differ only in which
// stream they subscribe to and how they render it, so everything else lives
// here rather than being copied three times.
struct ClientOptions {
  std::string server = "localhost:50051";
  std::string instrument;
  std::uint32_t min_interval_micros = 0;
  bool json = false;
  std::string out_file;  // empty = stdout only
  int max_updates = 0;   // 0 = run forever; used by the end-to-end tests
};

// Flags every publisher accepts. `extra` carries the band overrides that only
// two of them take.
inline void RequireKnownClientFlags(const Flags& flags,
                                    std::initializer_list<std::string_view> extra = {}) {
  std::vector<std::string_view> known{"server", "instrument", "min-interval-us",
                                      "json",   "max-updates", "quiet", "out-file"};
  known.insert(known.end(), extra.begin(), extra.end());
  flags.RequireKnown(std::span<const std::string_view>(known));
}

inline ClientOptions ParseClientOptions(const Flags& flags) {
  ClientOptions options;
  options.server = flags.Get("server", options.server);
  options.instrument = flags.Get("instrument", "");
  // Bounded before the unsigned cast, which would otherwise launder a negative
  // into an enormous positive: -1 becomes 4,294,967,295 microseconds, or 71
  // minutes between updates, and the client merely looks hung.
  options.min_interval_micros =
      static_cast<std::uint32_t>(flags.GetInt("min-interval-us", 0, 0, 60'000'000));
  options.json = flags.GetBool("json", false);
  options.out_file = flags.Get("out-file", "");
  options.max_updates = flags.GetInt("max-updates", 0, 0, 1'000'000);
  return options;
}

// Splits "a,b,c". Empty segments are skipped; whitespace is NOT trimmed, so
// "--venues=binance, okx" is rejected by the server rather than quietly fixed.
inline std::vector<std::string> SplitCsv(const std::string& text) {
  std::vector<std::string> out;
  std::string item;
  for (const char c : text + ",") {
    if (c != ',') {
      item.push_back(c);
    } else if (!item.empty()) {
      out.push_back(item);
      item.clear();
    }
  }
  return out;
}

inline void FillSubscription(const ClientOptions& options, v1::Subscription* subscription) {
  if (!options.instrument.empty()) subscription->set_instrument(options.instrument);
  subscription->set_min_interval_micros(options.min_interval_micros);
}

// Waits for the aggregator to come up rather than exiting on a connection
// refused. Under docker compose the clients start alongside the server, and a
// screen of connection errors on first run is a bad first impression.
inline std::shared_ptr<grpc::Channel> Connect(const std::string& server) {
  auto channel = grpc::CreateChannel(server, grpc::InsecureChannelCredentials());
  std::fprintf(stderr, "connecting to %s...\n", server.c_str());
  if (!channel->WaitForConnected(std::chrono::system_clock::now() +
                                 std::chrono::seconds(120))) {
    // Report the real problem here rather than letting the first RPC surface a
    // generic status code two minutes later. "The server never came up" and
    // "the server rejected the call" deserve different messages.
    std::fprintf(stderr, "%s did not become reachable within 120s\n", server.c_str());
  }
  return channel;
}

inline std::string ToJson(const google::protobuf::Message& message) {
  std::string out;
  google::protobuf::util::JsonPrintOptions print_options;
  print_options.always_print_fields_with_no_presence = true;
  const absl::Status status =
      google::protobuf::util::MessageToJsonString(message, &out, print_options);
  if (!status.ok()) {
    // --json exists so a script can consume this stream. Emitting nothing and
    // saying nothing is the worst available failure for that.
    std::fprintf(stderr, "failed to serialise update as JSON: %s\n",
                 std::string(status.message()).c_str());
    return {};
  }
  return out + "\n";
}

// Fixed-width so a stream of these reads as a table rather than ragged text.
inline std::string FmtPx(std::int64_t value) { return FormatFixed(value, 2); }
inline std::string FmtQty(std::int64_t value) { return FormatFixed(value, 8); }

// Millions() and Bps() convert to double for DISPLAY ONLY, and the result is
// never read back into arithmetic. The no-floating-point rule this codebase
// follows is about book state and about products of scaled values, where double
// is both inexact and overflow-prone; formatting a single already-computed
// value at two decimal places is neither. BpsLabel below stays on the
// fixed-point path because its output is a band label that must match the
// specification exactly.
inline std::string Millions(std::int64_t notional_e8) {
  const double millions = static_cast<double>(notional_e8) / static_cast<double>(kScale) / 1e6;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%8.3fM", millions);
  return buffer;
}

inline std::string Bps(std::int64_t bps_e8) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f",
                static_cast<double>(bps_e8) / static_cast<double>(kScale));
  return buffer;
}

// Whole basis points render without decimals, so the output transcribes the
// specification's "50bps/100bps/..." rather than "50.00bps".
inline std::string BpsLabel(std::int64_t bps_e8) { return FormatFixed(bps_e8, 0); }

// Writes each rendered record to stdout and, when --out-file is set, appends
// the same bytes to that file. Rendering goes through one buffer so the two
// destinations cannot drift apart.
class Output {
 public:
  explicit Output(const std::string& path) {
    if (!path.empty()) {
      file_.open(path, std::ios::out | std::ios::app);
      if (!file_) std::fprintf(stderr, "cannot open %s for writing\n", path.c_str());
    }
  }

  void Write(const std::string& record) {
    std::fputs(record.c_str(), stdout);
    // One flush per record, so Docker's combined log does not interleave
    // three services mid-record.
    std::fflush(stdout);
    if (file_.is_open()) {
      file_ << record;
      file_.flush();
    }
  }

 private:
  std::ofstream file_;
};

// Drives a server-streaming RPC: read, render, flush, honour --max-updates.
// Rendering is the only thing the three publishers do differently.
template <typename Update, typename Reader, typename Render>
int StreamLoop(const ClientOptions& options, Reader* reader, Render render) {
  Output out(options.out_file);
  Update update;
  int count = 0;
  while (reader->Read(&update)) {
    out.Write(options.json ? ToJson(update) : render(update));
    if (options.max_updates > 0 && ++count >= options.max_updates) break;
  }
  const grpc::Status status = reader->Finish();
  if (!status.ok() && options.max_updates == 0) {
    std::fprintf(stderr, "stream ended: %s\n", status.error_message().c_str());
    return 1;
  }
  return 0;
}

// printf into a std::string, so renderers can compose a record and hand it to
// Output rather than each writing to stdout themselves.
template <typename... Args>
std::string Line(const char* format, Args... args) {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer), format, args...);
  return std::string(buffer) + "\n";
}

// "binance:1.50000000 okx:0.50000000 " for a quote's venue attribution.
inline std::string VenueBreakdown(const v1::Quote& quote) {
  std::string out;
  for (const v1::VenueQty& contribution : quote.venues()) {
    out += contribution.venue() + ":" + FmtQty(contribution.qty_e8()) + " ";
  }
  return out;
}

// One header line per update, shared by all three publishers so their output
// can be correlated by sequence number in a combined compose log.
inline std::string Header(const v1::SnapshotMeta& meta, const v1::Touch& touch) {
  std::string line = "seq=" + std::to_string(meta.sequence()) + " " +
                     FormatWallNs(meta.publish_ts_ns());
  if (touch.crossed()) line += " CROSSED";
  if (!meta.stale_venues().empty()) {
    line += " stale=";
    for (const std::string& venue : meta.stale_venues()) line += venue + ",";
    line.pop_back();
  }
  line += " venues=";
  for (const std::string& venue : meta.contributing_venues()) line += venue + ",";
  if (line.back() == ',') line.pop_back();
  return line;
}

}  // namespace md
