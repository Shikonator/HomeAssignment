// Publisher for notional-volume band prices (1M/5M/10M/25M/50M+).
#include <cstdio>

#include "src/clients/common.h"

namespace {

// The specification asks for "1M/5M/10M/25M/50M+". The trailing "+" IS the
// open-ended band, so it is labelled from the largest finite threshold rather
// than generically -- otherwise the output cannot be matched against the
// requirement by eye. On the wire it stays notional_target_e8 = 0.
void PrintSide(const char* label,
               const google::protobuf::RepeatedPtrField<md::v1::VolumeBand>& bands) {
  std::int64_t largest = 0;
  for (const md::v1::VolumeBand& band : bands) {
    if (!band.open_ended()) largest = std::max(largest, band.notional_target_e8());
  }
  for (const md::v1::VolumeBand& band : bands) {
    const std::string target = band.open_ended() ? md::Millions(largest) + "+"
                                                 : md::Millions(band.notional_target_e8()) + " ";
    std::printf("      %s %s  vwap %14s  worst %14s  qty %14s  filled %s  levels %d\n", label,
                target.c_str(), md::FmtPx(band.vwap_price_e8()).c_str(),
                md::FmtPx(band.worst_price_e8()).c_str(), md::FmtQty(band.filled_qty_e8()).c_str(),
                md::Millions(band.filled_notional_e8()).c_str(), band.levels_consumed());
  }
}

void Print(const md::v1::VolumeBandsUpdate& update) {
  std::printf("[vol] %s\n", md::Header(update.meta(), update.touch()).c_str());
  PrintSide("BID", update.bid());
  PrintSide("ASK", update.ask());
}

}  // namespace

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  md::RequireKnownClientFlags(flags, {"bands-usd"});
  const md::ClientOptions options = md::ParseClientOptions(flags);

  auto stub = md::v1::MarketData::NewStub(md::Connect(options.server));
  md::v1::StreamVolumeBandsRequest request;
  md::FillSubscription(options, request.mutable_subscription());
  // Bands are a property of the request, not the build.
  for (const std::string& band : md::SplitCsv(flags.Get("bands-usd", ""))) {
    request.add_notional_bands_e8(std::atoll(band.c_str()) * md::kScale);
  }

  grpc::ClientContext context;
  auto reader = stub->StreamVolumeBands(&context, request);
  return md::StreamLoop<md::v1::VolumeBandsUpdate>(options, reader.get(), Print);
}
