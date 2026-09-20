#pragma once

#include <grpcpp/grpcpp.h>

#include <functional>
#include <string>
#include <vector>

#include "proto/md/v1/bbo.grpc.pb.h"
#include "proto/md/v1/price_bands.grpc.pb.h"
#include "proto/md/v1/status.grpc.pb.h"
#include "proto/md/v1/volume_bands.grpc.pb.h"
#include "src/aggregator/engine.h"

namespace md {

// Server defaults, used when a request leaves the band list empty. The trailing
// open-ended band is appended by the band walk, not listed here.
std::vector<Notional> DefaultNotionalBands();
std::vector<std::int64_t> DefaultBpsOffsets();

// Subscription handling shared by the three streaming services: they differ
// only in what they put in each message.
class StreamingBase {
 public:
  explicit StreamingBase(Engine* engine) : engine_(engine) {}

 protected:
  // Subscribe, wait, honour cancellation and the client's conflation floor,
  // emit. `emit` returns false when the write fails, ending the stream.
  grpc::Status CheckInstrument(const v1::Subscription& subscription) const;

  grpc::Status RunStream(grpc::ServerContext* context, const v1::Subscription& subscription,
                         const std::function<bool(const ConsolidatedBook&)>& emit);

  Engine* const engine_;
};

class BboService final : public v1::Bbo::Service, public StreamingBase {
 public:
  explicit BboService(Engine* engine) : StreamingBase(engine) {}

  grpc::Status Stream(grpc::ServerContext* context, const v1::StreamBboRequest* request,
                      grpc::ServerWriter<v1::BboUpdate>* writer) override;
};

class VolumeBandsService final : public v1::VolumeBands::Service, public StreamingBase {
 public:
  explicit VolumeBandsService(Engine* engine) : StreamingBase(engine) {}

  grpc::Status Stream(grpc::ServerContext* context, const v1::StreamVolumeBandsRequest* request,
                      grpc::ServerWriter<v1::VolumeBandsUpdate>* writer) override;
};

class PriceBandsService final : public v1::PriceBands::Service, public StreamingBase {
 public:
  explicit PriceBandsService(Engine* engine) : StreamingBase(engine) {}

  grpc::Status Stream(grpc::ServerContext* context, const v1::StreamPriceBandsRequest* request,
                      grpc::ServerWriter<v1::PriceBandsUpdate>* writer) override;
};

class StatusService final : public v1::Status::Service {
 public:
  explicit StatusService(Engine* engine) : engine_(engine) {}

  grpc::Status GetVenueStatus(grpc::ServerContext* context,
                              const v1::GetVenueStatusRequest* request,
                              v1::GetVenueStatusResponse* response) override;

 private:
  Engine* const engine_;
};

}  // namespace md
