// Publisher for price-band liquidity (BBO + 50/100/200/500/1000+ bps).
#include <cstdio>

#include "src/clients/common.h"

namespace {

void PrintSide(const char* label,
               const google::protobuf::RepeatedPtrField<md::v1::PriceBand>& bands) {
  // The specification asks for "BBO+ 50bps/100bps/200bps/500bps/1000bps+". As
  // with volume bands, the trailing "+" is the open-ended band and is labelled
  // from the largest finite offset so the output reads as the requirement.
  std::int64_t largest_finite = 0;
  for (const md::v1::PriceBand& band : bands) {
    if (!band.open_ended() && band.offset_bps_e8() > largest_finite) {
      largest_finite = band.offset_bps_e8();
    }
  }

  for (const md::v1::PriceBand& band : bands) {
    const std::string offset = band.open_ended()
                                   ? md::BpsLabel(largest_finite) + "bps+"
                                   : md::BpsLabel(band.offset_bps_e8()) + "bps ";
    std::printf("      %s %9s  bound %14s  qty %14s  notional %s  vwap %14s  levels %5d%s\n",
                label, offset.c_str(), md::FmtPx(band.bound_price_e8()).c_str(),
                md::FmtQty(band.qty_e8()).c_str(), md::Millions(band.notional_e8()).c_str(),
                md::FmtPx(band.vwap_price_e8()).c_str(), band.levels(),
                band.depth_limited() ? "  [depth-limited]" : "");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  md::RequireKnownClientFlags(flags, {"bands-bps"});
  const md::ClientOptions options = md::ParseClientOptions(flags);

  auto stub = md::v1::MarketData::NewStub(md::Connect(options.server));

  md::v1::StreamPriceBandsRequest request;
  md::FillSubscription(options, request.mutable_subscription());
  const std::string offsets = flags.Get("bands-bps", "");
  std::string number;
  for (const char c : offsets + ",") {
    if (c == ',') {
      if (!number.empty()) {
        request.add_offsets_bps_e8(std::atoll(number.c_str()) * md::kScale);
      }
      number.clear();
    } else {
      number.push_back(c);
    }
  }

  grpc::ClientContext context;
  auto reader = stub->StreamPriceBands(&context, request);

  md::v1::PriceBandsUpdate update;
  int count = 0;
  while (reader->Read(&update)) {
    if (options.json) {
      md::PrintJson(update);
    } else {
      std::printf("[px ] %s\n", md::Header(update.meta(), update.touch()).c_str());
      PrintSide("BID", update.bid());
      PrintSide("ASK", update.ask());
    }
    std::fflush(stdout);
    if (options.max_updates > 0 && ++count >= options.max_updates) break;
  }

  const grpc::Status status = reader->Finish();
  if (!status.ok() && options.max_updates == 0) {
    std::fprintf(stderr, "stream ended: %s\n", status.error_message().c_str());
    return 1;
  }
  return 0;
}
