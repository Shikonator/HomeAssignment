// Operational status query, and the container health check.
//
// Doubles as both so the compose healthcheck needs no extra binary in the
// image: it exits non-zero unless at least one venue is LIVE, which is the
// weakest condition under which the aggregator is actually useful.
#include "src/clients/common.h"

namespace {

std::string Render(const md::v1::GetVenueStatusResponse& response) {
  std::string out = md::Line("published=%d  latency p50=%dus p99=%dus max=%dus  uptime=%ds",
                             response.published_snapshots(),
                             response.aggregation_latency().p50_ns() / 1000,
                             response.aggregation_latency().p99_ns() / 1000,
                             response.aggregation_latency().max_ns() / 1000,
                             response.uptime_ns() / 1000000000);
  for (const md::v1::VenueStatus& venue : response.venues()) {
    out += md::Line(
        "  %-8s %-22s %-12s msgs=%-8d resync=%-4d gaps=%-4d overflow=%-4d "
        "bids=%-5d asks=%-5d  %s / %s",
        venue.venue(), venue.venue_symbol(), md::v1::ConnState_Name(venue.state()),
        venue.messages(), venue.resyncs(), venue.sequence_gaps(), venue.ring_overflows(),
        venue.bid_levels(), venue.ask_levels(), md::FmtPx(venue.best_bid_e8()),
        md::FmtPx(venue.best_ask_e8()));
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  md::RequireKnownClientFlags(flags);
  const md::ClientOptions options = md::ParseClientOptions(flags);
  const bool quiet = flags.GetBool("quiet", false);

  auto channel = grpc::CreateChannel(options.server, grpc::InsecureChannelCredentials());
  auto stub = md::v1::Status::NewStub(channel);

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  md::v1::GetVenueStatusRequest request;
  md::v1::GetVenueStatusResponse response;

  const grpc::Status status = stub->GetVenueStatus(&context, request, &response);
  if (!status.ok()) {
    if (!quiet) md::Log("status", "query failed: " + status.error_message());
    return 1;
  }

  int live = 0;
  for (const md::v1::VenueStatus& venue : response.venues()) {
    if (venue.state() == md::v1::CONN_STATE_LIVE) ++live;
  }

  if (!quiet) {
    md::Output out(options.out_file);
    out.Write(options.json ? md::ToJson(response) : Render(response));
  }

  return live > 0 ? 0 : 1;
}
