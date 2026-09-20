// Publisher for price-band liquidity (BBO + 50/100/200/500/1000+ bps).
#include <algorithm>
#include <string>

#include "src/clients/common.h"

namespace {

// As with volume bands, the trailing "+" is the open-ended band and is labelled
// from the largest finite offset so the output reads as the requirement.
std::string RenderSide(const char* label,
                       const google::protobuf::RepeatedPtrField<md::v1::PriceBand>& bands) {
  std::int64_t largest = 0;
  for (const md::v1::PriceBand& band : bands) {
    if (!band.open_ended()) largest = std::max(largest, band.offset_bps_e8());
  }
  std::string out;
  for (const md::v1::PriceBand& band : bands) {
    const std::string offset = band.open_ended() ? md::BpsLabel(largest) + "bps+"
                                                 : md::BpsLabel(band.offset_bps_e8()) + "bps ";
    out += md::Line(
        "      %s %9s  bound %14s  qty %14s  notional %s  vwap %14s  levels %5d%s", label,
        offset.c_str(), md::FmtPx(band.bound_price_e8()).c_str(),
        md::FmtQty(band.qty_e8()).c_str(), md::Millions(band.notional_e8()).c_str(),
        md::FmtPx(band.vwap_price_e8()).c_str(), band.levels(),
        band.depth_limited() ? "  [depth-limited]" : "");
  }
  return out;
}

std::string Render(const md::v1::PriceBandsUpdate& update) {
  return md::Line("[px ] %s", md::Header(update.meta(), update.touch()).c_str()) +
         RenderSide("BID", update.bid()) + RenderSide("ASK", update.ask());
}

}  // namespace

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  md::RequireKnownClientFlags(flags, {"bands-bps"});
  const md::ClientOptions options = md::ParseClientOptions(flags);

  auto stub = md::v1::PriceBands::NewStub(md::Connect(options.server));
  md::v1::StreamPriceBandsRequest request;
  md::FillSubscription(options, request.mutable_subscription());
  for (const std::string& band : md::SplitCsv(flags.Get("bands-bps", ""))) {
    request.add_offsets_bps_e8(std::atoll(band.c_str()) * md::kScale);
  }

  md::Output out(options.out_file);
  grpc::ClientContext context;
  auto reader = stub->Stream(&context, request);
  return md::StreamLoop<md::v1::PriceBandsUpdate>(options, &out, &context, reader.get(), Render);
}
