// Operational status query, and the container health check.
//
// Doubles as both so the compose healthcheck needs no extra binary in the
// image: it exits non-zero unless at least one venue is LIVE, which is the
// weakest condition under which the aggregator is actually useful.
#include <cstdio>

#include "src/clients/common.h"

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  const md::ClientOptions options = md::ParseClientOptions(flags);
  const bool quiet = flags.GetBool("quiet", false);

  auto channel = grpc::CreateChannel(options.server, grpc::InsecureChannelCredentials());
  auto stub = md::v1::MarketData::NewStub(channel);

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  md::v1::GetVenueStatusRequest request;
  md::v1::GetVenueStatusResponse response;

  const grpc::Status status = stub->GetVenueStatus(&context, request, &response);
  if (!status.ok()) {
    if (!quiet) std::fprintf(stderr, "status failed: %s\n", status.error_message().c_str());
    return 1;
  }

  int live = 0;
  for (const md::v1::VenueStatus& venue : response.venues()) {
    if (venue.state() == md::v1::CONN_STATE_LIVE) ++live;
  }

  if (!quiet) {
    if (options.json) {
      md::PrintJson(response);
    } else {
      std::printf("published=%llu  latency p50=%lldus p99=%lldus max=%lldus  uptime=%llds\n",
                  static_cast<unsigned long long>(response.published_snapshots()),
                  static_cast<long long>(response.aggregation_latency().p50_ns() / 1000),
                  static_cast<long long>(response.aggregation_latency().p99_ns() / 1000),
                  static_cast<long long>(response.aggregation_latency().max_ns() / 1000),
                  static_cast<long long>(response.uptime_ns() / 1000000000));
      for (const md::v1::VenueStatus& venue : response.venues()) {
        std::printf(
            "  %-8s %-22s %-12s msgs=%-8llu resync=%-4llu gaps=%-4llu overflow=%-4llu "
            "bids=%-5d asks=%-5d  %s / %s\n",
            venue.venue().c_str(), venue.venue_symbol().c_str(),
            md::v1::ConnState_Name(venue.state()).c_str(),
            static_cast<unsigned long long>(venue.messages()),
            static_cast<unsigned long long>(venue.resyncs()),
            static_cast<unsigned long long>(venue.sequence_gaps()),
            static_cast<unsigned long long>(venue.ring_overflows()), venue.bid_levels(),
            venue.ask_levels(), md::FmtPx(venue.best_bid_e8()).c_str(),
            md::FmtPx(venue.best_ask_e8()).c_str());
      }
    }
    std::fflush(stdout);
  }

  return live > 0 ? 0 : 1;
}
