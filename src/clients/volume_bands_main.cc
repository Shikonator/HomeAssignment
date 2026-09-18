// Publisher for notional-volume band prices (1M/5M/10M/25M/50M+).
#include <cstdio>

#include "src/clients/common.h"

namespace {

void PrintSide(const char* label,
               const google::protobuf::RepeatedPtrField<md::v1::VolumeBand>& bands) {
  // The specification asks for "1M/5M/10M/25M/50M+". The trailing "+" IS the
  // open-ended band, so it is labelled with the largest finite threshold rather
  // than something generic -- otherwise the output cannot be matched against
  // the requirement by eye. The band itself carries notional_target_e8 = 0,
  // which stays the unambiguous wire key; this is purely how it is drawn.
  std::int64_t largest_finite = 0;
  for (const md::v1::VolumeBand& band : bands) {
    if (!band.open_ended() && band.notional_target_e8() > largest_finite) {
      largest_finite = band.notional_target_e8();
    }
  }

  for (const md::v1::VolumeBand& band : bands) {
    const std::string target = band.open_ended()
                                   ? md::Millions(largest_finite) + "+"
                                   : md::Millions(band.notional_target_e8()) + " ";
    std::printf("      %s %s  vwap %14s  worst %14s  qty %14s  filled %s  levels %d\n", label,
                target.c_str(), md::FmtPx(band.vwap_price_e8()).c_str(),
                md::FmtPx(band.worst_price_e8()).c_str(), md::FmtQty(band.filled_qty_e8()).c_str(),
                md::Millions(band.filled_notional_e8()).c_str(), band.levels_consumed());
  }
}

}  // namespace

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  const md::ClientOptions options = md::ParseClientOptions(flags);

  auto stub = md::v1::MarketData::NewStub(md::Connect(options.server));

  md::v1::StreamVolumeBandsRequest request;
  md::FillSubscription(options, request.mutable_subscription());
  // Bands are server-side defaults unless the operator overrides them, which is
  // what makes the thresholds a property of the request rather than the build.
  const std::string bands = flags.Get("bands-usd", "");
  std::string number;
  for (const char c : bands + ",") {
    if (c == ',') {
      if (!number.empty()) {
        request.add_notional_bands_e8(std::atoll(number.c_str()) * md::kScale);
      }
      number.clear();
    } else {
      number.push_back(c);
    }
  }

  grpc::ClientContext context;
  auto reader = stub->StreamVolumeBands(&context, request);

  md::v1::VolumeBandsUpdate update;
  int count = 0;
  while (reader->Read(&update)) {
    if (options.json) {
      md::PrintJson(update);
    } else {
      std::printf("[vol] %s\n", md::Header(update.meta(), update.touch()).c_str());
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
