// Publisher for the consolidated best bid and offer.
#include <cstdio>

#include "src/clients/common.h"

namespace {

void PrintQuote(const char* label, const md::v1::Quote& quote) {
  std::printf("      %s %14s x %14s   %s\n", label, md::FmtPx(quote.price_e8()).c_str(),
              md::FmtQty(quote.qty_e8()).c_str(), md::VenueBreakdown(quote).c_str());
}

void Print(const md::v1::BboUpdate& update) {
  const md::v1::Touch& touch = update.touch();
  std::printf("[bbo] %s\n", md::Header(update.meta(), touch).c_str());
  PrintQuote("bid", update.bid());
  PrintQuote("ask", update.ask());
  std::printf("      spread %s (%s bps)  mid %s\n", md::FmtPx(touch.spread_e8()).c_str(),
              md::Bps(touch.spread_bps_e8()).c_str(), md::FmtPx(touch.mid_price_e8()).c_str());
}

}  // namespace

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  md::RequireKnownClientFlags(flags);
  const md::ClientOptions options = md::ParseClientOptions(flags);

  auto stub = md::v1::MarketData::NewStub(md::Connect(options.server));
  md::v1::StreamBboRequest request;
  md::FillSubscription(options, request.mutable_subscription());

  grpc::ClientContext context;
  auto reader = stub->StreamBbo(&context, request);
  return md::StreamLoop<md::v1::BboUpdate>(options, reader.get(), Print);
}
