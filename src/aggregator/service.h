#pragma once

#include <grpcpp/grpcpp.h>

#include <functional>
#include <string>
#include <vector>

#include "proto/md/v1/market_data.grpc.pb.h"
#include "src/aggregator/engine.h"

namespace md {

// Server defaults, used when a request leaves the band list empty. These are
// the sets named in the assignment; the trailing open-ended band is appended by
// the band walk itself, not listed here.
std::vector<Notional> DefaultNotionalBands();
std::vector<std::int64_t> DefaultBpsOffsets();

class MarketDataService final : public v1::MarketData::Service {
 public:
  explicit MarketDataService(Engine* engine) : engine_(engine) {}

  grpc::Status StreamBbo(grpc::ServerContext* context, const v1::StreamBboRequest* request,
                         grpc::ServerWriter<v1::BboUpdate>* writer) override;
  grpc::Status StreamVolumeBands(grpc::ServerContext* context,
                                 const v1::StreamVolumeBandsRequest* request,
                                 grpc::ServerWriter<v1::VolumeBandsUpdate>* writer) override;
  grpc::Status StreamPriceBands(grpc::ServerContext* context,
                               const v1::StreamPriceBandsRequest* request,
                               grpc::ServerWriter<v1::PriceBandsUpdate>* writer) override;
  grpc::Status GetVenueStatus(grpc::ServerContext* context,
                              const v1::GetVenueStatusRequest* request,
                              v1::GetVenueStatusResponse* response) override;

 private:
  // Resolves a Subscription into a venue mask, rejecting unknown venues and
  // instruments rather than silently ignoring them.
  grpc::Status ResolveSubscription(const v1::Subscription& subscription, VenueMask* mask,
                                   bool* filtered) const;

  // Shared drive loop for all three streams: subscribe, wait, honour
  // cancellation and the client's conflation floor, emit. `emit` returns false
  // when the write fails, which ends the stream.
  grpc::Status RunStream(
      grpc::ServerContext* context, const v1::Subscription& subscription,
      const std::function<bool(const ConsolidatedBook&, VenueMask, bool)>& emit);

  Engine* const engine_;
};

}  // namespace md
