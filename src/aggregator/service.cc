#include "src/aggregator/service.h"

#include <algorithm>
#include <chrono>

#include "src/core/clock.h"

namespace md {
namespace {

constexpr std::chrono::milliseconds kWaitSlice{250};

// Fills the provenance block carried by every update.
void FillMeta(const ConsolidatedBook& book, v1::SnapshotMeta* meta) {
  meta->set_instrument(book.instrument);
  meta->set_sequence(book.sequence);
  meta->set_publish_ts_ns(book.publish_ts_ns);
  meta->set_ingest_recv_ts_ns(book.ingest_recv_ts_ns);
  meta->set_price_scale(kScale);
  meta->set_qty_scale(kScale);
  meta->set_bid_ladder_truncated(book.bids_truncated);
  meta->set_ask_ladder_truncated(book.asks_truncated);
  for (const std::string& name : book.contributing) meta->add_contributing_venues(name);
  for (const std::string& name : book.stale) meta->add_stale_venues(name);
  for (const VenueClockInfo& clock : book.clocks) {
    v1::VenueClock* out = meta->add_venue_clocks();
    out->set_venue(clock.name);
    out->set_last_recv_ts_ns(clock.last_recv_ts_ns);
    out->set_last_exchange_ts_ns(clock.last_exchange_ts_ns);
  }
}

void FillTouch(const TouchInfo& touch, v1::Touch* out) {
  out->set_has_bid(touch.has_bid);
  out->set_has_ask(touch.has_ask);
  out->set_best_bid_e8(touch.best_bid);
  out->set_best_ask_e8(touch.best_ask);
  out->set_mid_price_e8(touch.mid);
  out->set_spread_e8(touch.spread_e8);
  out->set_spread_bps_e8(touch.spread_bps_e8);
  out->set_crossed(touch.crossed);
}

void FillQuote(const ConsolidatedBook& book, const LadderView& view, v1::Quote* out) {
  const MergedLevel* best = ViewBestLevel(view);
  if (best == nullptr) return;
  out->set_price_e8(best->px);
  out->set_qty_e8(ViewQty(view, *best));
  for (int venue = 0; venue < kMaxVenues; ++venue) {
    if (!(view.mask & MaskOf(venue))) continue;
    if (best->by_venue[venue] <= 0) continue;
    v1::VenueQty* contribution = out->add_venues();
    contribution->set_venue(venue < static_cast<int>(book.venue_names.size())
                                ? book.venue_names[venue]
                                : std::to_string(venue));
    contribution->set_qty_e8(best->by_venue[venue]);
  }
}

LadderView MakeView(const ConsolidatedBook& book, bool bids, VenueMask mask, bool filtered) {
  LadderView view;
  view.levels = bids ? book.bids : book.asks;
  view.descending = bids;
  view.mask = filtered ? mask : book.contributing_mask;
  view.filtered = filtered;
  return view;
}

// Nothing else bounds this. Without a cap, a one-line request produces an
// arbitrarily large message on every publish, for every subscriber, forever --
// at no cost to the client that asked for it.
constexpr int kMaxBandsPerRequest = 32;

// Validates and normalises a client-supplied band list.
//
// A non-positive threshold is rejected rather than tolerated: it would otherwise
// produce a band reporting fully_filled=true with zero quantity ("you can fill
// $0 by doing nothing"), which is internally consistent and useless. Duplicates
// are rejected for the same reason a misspelled venue is -- silently returning
// two identical bands hides a client bug.
//
// Note what is NOT validated: how deep a target is. Whether a band can be
// satisfied is a runtime property of the book, not a static property of the
// configuration, and asking about 50M is entirely legitimate even though no
// venue publishes that much depth. That case is answered by fully_filled=false
// and depth_limited=true, per band, rather than by failing the request.
grpc::Status NormaliseBands(const char* field, std::vector<std::int64_t> requested,
                            std::vector<std::int64_t> defaults,
                            std::vector<std::int64_t>* out) {
  if (requested.empty()) {
    *out = std::move(defaults);
    return grpc::Status::OK;
  }
  if (static_cast<int>(requested.size()) > kMaxBandsPerRequest) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        std::string(field) + ": at most " +
                            std::to_string(kMaxBandsPerRequest) + " bands per request");
  }
  for (const std::int64_t value : requested) {
    if (value <= 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          std::string(field) + ": values must be positive");
    }
  }
  std::sort(requested.begin(), requested.end());
  if (std::adjacent_find(requested.begin(), requested.end()) != requested.end()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        std::string(field) + ": duplicate values");
  }
  *out = std::move(requested);
  return grpc::Status::OK;
}

grpc::Status MakeVolumeConfig(const v1::StreamVolumeBandsRequest& request, BandConfig* config) {
  return NormaliseBands("notional_bands_e8",
                        {request.notional_bands_e8().begin(), request.notional_bands_e8().end()},
                        DefaultNotionalBands(), &config->notionals_e8);
}

grpc::Status MakePriceConfig(const v1::StreamPriceBandsRequest& request, BandConfig* config) {
  return NormaliseBands("offsets_bps_e8",
                        {request.offsets_bps_e8().begin(), request.offsets_bps_e8().end()},
                        DefaultBpsOffsets(), &config->offsets_bps_e8);
}

void FillVolumeBands(const SideBands& bands,
                     google::protobuf::RepeatedPtrField<v1::VolumeBand>* out) {
  for (const VolumeBandResult& band : bands.volume) {
    v1::VolumeBand* message = out->Add();
    message->set_notional_target_e8(band.target_e8);
    message->set_vwap_price_e8(band.vwap_e8);
    message->set_worst_price_e8(band.worst_e8);
    message->set_filled_qty_e8(band.filled_qty_e8);
    message->set_filled_notional_e8(band.filled_notional_e8);
    message->set_fully_filled(band.fully_filled);
    message->set_levels_consumed(band.levels_consumed);
    message->set_open_ended(band.open_ended);
  }
}

void FillPriceBands(const SideBands& bands,
                    google::protobuf::RepeatedPtrField<v1::PriceBand>* out) {
  for (const PriceBandResult& band : bands.price) {
    v1::PriceBand* message = out->Add();
    message->set_offset_bps_e8(band.offset_bps_e8);
    message->set_bound_price_e8(band.bound_e8);
    message->set_qty_e8(band.qty_e8);
    message->set_notional_e8(band.notional_e8);
    message->set_vwap_price_e8(band.vwap_e8);
    message->set_levels(band.levels);
    message->set_open_ended(band.open_ended);
    message->set_depth_limited(band.depth_limited);
  }
}

}  // namespace

std::vector<Notional> DefaultNotionalBands() {
  return {
      static_cast<Notional>(1'000'000) * kScale,
      static_cast<Notional>(5'000'000) * kScale,
      static_cast<Notional>(10'000'000) * kScale,
      static_cast<Notional>(25'000'000) * kScale,
      static_cast<Notional>(50'000'000) * kScale,
  };
}

std::vector<std::int64_t> DefaultBpsOffsets() {
  return {50 * kScale, 100 * kScale, 200 * kScale, 500 * kScale, 1000 * kScale};
}

grpc::Status MarketDataService::ResolveSubscription(const v1::Subscription& subscription,
                                                    VenueMask* mask, bool* filtered) const {
  const std::string& instrument = subscription.instrument();
  if (!instrument.empty() && instrument != engine_->config().instrument) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "this server aggregates " + engine_->config().instrument + ", not " +
                            instrument);
  }

  if (subscription.venues().empty()) {
    *filtered = false;
    *mask = 0;
    return grpc::Status::OK;
  }

  *filtered = true;
  *mask = 0;
  for (const std::string& requested : subscription.venues()) {
    bool found = false;
    for (std::size_t i = 0; i < engine_->venues().size(); ++i) {
      if (engine_->venues()[i].name == requested) {
        *mask |= MaskOf(static_cast<int>(i));
        found = true;
        break;
      }
    }
    // A typo must never be silently ignored: the client would receive a
    // plausible book built from venues it did not ask for.
    if (!found) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown venue: " + requested);
    }
  }
  return grpc::Status::OK;
}

grpc::Status MarketDataService::RunStream(
    grpc::ServerContext* context, const v1::Subscription& subscription,
    const std::function<bool(const ConsolidatedBook&, VenueMask, bool)>& emit) {
  VenueMask mask = 0;
  bool filtered = false;
  const grpc::Status status = ResolveSubscription(subscription, &mask, &filtered);
  if (!status.ok()) return status;

  auto slot = engine_->AddSubscriber();
  const auto min_interval = std::chrono::microseconds(subscription.min_interval_micros());
  auto last_sent = std::chrono::steady_clock::time_point::min();

  // AddSubscriber registers the slot before Latest() is read, so a publish
  // landing between the two delivers the same state twice: once as the initial
  // send and once from the first wait. Harmless -- every message is absolute
  // state -- but it prints the same sequence twice on a client's first two
  // lines. Tracking what we last sent is immune to the race, rather than
  // narrowing it by reordering locks.
  std::uint64_t last_sequence = 0;

  // Send the current state immediately rather than making a new subscriber wait
  // for the next tick, which on a quiet book could be a long time.
  if (auto initial = engine_->Latest()) {
    if (!emit(*initial, mask, filtered)) {
      engine_->RemoveSubscriber(slot);
      return grpc::Status::OK;
    }
    last_sequence = initial->sequence;
    last_sent = std::chrono::steady_clock::now();
  }

  while (!context->IsCancelled()) {
    // Bounded wait. The synchronous server cannot interrupt a thread parked in
    // user code, so an unbounded wait would leak this handler thread whenever a
    // client disconnects while the book is quiet.
    auto book = slot->WaitNextFor(kWaitSlice);
    if (slot->stopped()) break;
    if (book == nullptr) continue;  // timed out; loop back and re-check cancellation
    if (book->sequence <= last_sequence) continue;
    last_sequence = book->sequence;

    if (min_interval.count() > 0) {
      const auto now = std::chrono::steady_clock::now();
      if (last_sent != std::chrono::steady_clock::time_point::min() &&
          now - last_sent < min_interval) {
        continue;
      }
      last_sent = now;
    }
    if (!emit(*book, mask, filtered)) break;
  }

  engine_->RemoveSubscriber(slot);
  return grpc::Status::OK;
}

grpc::Status MarketDataService::StreamBbo(grpc::ServerContext* context,
                                          const v1::StreamBboRequest* request,
                                          grpc::ServerWriter<v1::BboUpdate>* writer) {
  return RunStream(context, request->subscription(),
                   [&](const ConsolidatedBook& book, VenueMask mask, bool filtered) {
                     const LadderView bids = MakeView(book, true, mask, filtered);
                     const LadderView asks = MakeView(book, false, mask, filtered);

                     v1::BboUpdate update;
                     FillMeta(book, update.mutable_meta());
                     FillTouch(ComputeTouch(bids, asks), update.mutable_touch());
                     FillQuote(book, bids, update.mutable_bid());
                     FillQuote(book, asks, update.mutable_ask());
                     return writer->Write(update);
                   });
}

grpc::Status MarketDataService::StreamVolumeBands(
    grpc::ServerContext* context, const v1::StreamVolumeBandsRequest* request,
    grpc::ServerWriter<v1::VolumeBandsUpdate>* writer) {
  BandConfig config;
  const grpc::Status valid = MakeVolumeConfig(*request, &config);
  if (!valid.ok()) return valid;
  return RunStream(context, request->subscription(),
                   [&](const ConsolidatedBook& book, VenueMask mask, bool filtered) {
                     const LadderView bids = MakeView(book, true, mask, filtered);
                     const LadderView asks = MakeView(book, false, mask, filtered);

                     v1::VolumeBandsUpdate update;
                     FillMeta(book, update.mutable_meta());
                     FillTouch(ComputeTouch(bids, asks), update.mutable_touch());

                     SideBands side;
                     ComputeSideBands(bids, config, &side);
                     FillVolumeBands(side, update.mutable_bid());
                     ComputeSideBands(asks, config, &side);
                     FillVolumeBands(side, update.mutable_ask());
                     return writer->Write(update);
                   });
}

grpc::Status MarketDataService::StreamPriceBands(
    grpc::ServerContext* context, const v1::StreamPriceBandsRequest* request,
    grpc::ServerWriter<v1::PriceBandsUpdate>* writer) {
  BandConfig config;
  const grpc::Status valid = MakePriceConfig(*request, &config);
  if (!valid.ok()) return valid;
  return RunStream(context, request->subscription(),
                   [&](const ConsolidatedBook& book, VenueMask mask, bool filtered) {
                     const LadderView bids = MakeView(book, true, mask, filtered);
                     const LadderView asks = MakeView(book, false, mask, filtered);

                     v1::PriceBandsUpdate update;
                     FillMeta(book, update.mutable_meta());
                     FillTouch(ComputeTouch(bids, asks), update.mutable_touch());

                     SideBands side;
                     ComputeSideBands(bids, config, &side);
                     FillPriceBands(side, update.mutable_bid());
                     ComputeSideBands(asks, config, &side);
                     FillPriceBands(side, update.mutable_ask());
                     return writer->Write(update);
                   });
}

grpc::Status MarketDataService::GetVenueStatus(grpc::ServerContext* /*context*/,
                                               const v1::GetVenueStatusRequest* /*request*/,
                                               v1::GetVenueStatusResponse* response) {
  for (const VenueFeed& venue : engine_->venues()) {
    const VenueStats& stats = *venue.stats;
    v1::VenueStatus* status = response->add_venues();
    status->set_venue(venue.name);
    status->set_venue_symbol(venue.venue_symbol);
    status->set_state(static_cast<v1::ConnState>(stats.state.load()));
    status->set_last_recv_ts_ns(stats.last_recv_ts_ns.load());
    status->set_last_exchange_ts_ns(stats.last_exchange_ts_ns.load());
    status->set_messages(stats.messages.load());
    status->set_resyncs(stats.resyncs.load());
    status->set_sequence_gaps(stats.sequence_gaps.load());
    status->set_ring_overflows(stats.ring_overflows.load());
  }

  if (auto book = engine_->Latest()) {
    for (int i = 0; i < response->venues_size(); ++i) {
      v1::VenueStatus* status = response->mutable_venues(i);
      Qty bid_levels = 0;
      Qty ask_levels = 0;
      for (const MergedLevel& level : book->bids) {
        if (level.by_venue[i] > 0) ++bid_levels;
      }
      for (const MergedLevel& level : book->asks) {
        if (level.by_venue[i] > 0) ++ask_levels;
      }
      status->set_bid_levels(static_cast<int>(bid_levels));
      status->set_ask_levels(static_cast<int>(ask_levels));
      for (const MergedLevel& level : book->bids) {
        if (level.by_venue[i] > 0) {
          status->set_best_bid_e8(level.px);
          break;
        }
      }
      for (const MergedLevel& level : book->asks) {
        if (level.by_venue[i] > 0) {
          status->set_best_ask_e8(level.px);
          break;
        }
      }
    }
  }

  const LatencySummary latency = engine_->Latency();
  v1::LatencyStats* stats = response->mutable_aggregation_latency();
  stats->set_p50_ns(latency.p50_ns);
  stats->set_p99_ns(latency.p99_ns);
  stats->set_max_ns(latency.max_ns);
  stats->set_count(latency.count);

  response->set_published_snapshots(engine_->published());
  response->set_server_ts_ns(WallNowNs());
  response->set_uptime_ns(WallNowNs() - engine_->start_wall_ns());
  return grpc::Status::OK;
}

}  // namespace md
