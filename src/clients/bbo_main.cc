// Publisher for the consolidated best bid and offer.
#include <string>

#include "src/clients/common.h"

namespace {

std::string Quote(const char* label, const md::v1::Quote& quote) {
  return md::Line("      %s %14s x %14s   %s", label, md::FmtPx(quote.price_e8()).c_str(),
                  md::FmtQty(quote.qty_e8()).c_str(), md::VenueBreakdown(quote).c_str());
}

std::string Render(const md::v1::BboUpdate& update) {
  const md::v1::Touch& touch = update.touch();
  return md::Line("[bbo] %s", md::Header(update.meta(), touch).c_str()) +
         Quote("bid", update.bid()) + Quote("ask", update.ask()) +
         md::Line("      spread %s (%s bps)  mid %s", md::FmtPx(touch.spread_e8()).c_str(),
                  md::Bps(touch.spread_bps_e8()).c_str(),
                  md::FmtPx(touch.mid_price_e8()).c_str());
}

}  // namespace

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  md::RequireKnownClientFlags(flags);
  const md::ClientOptions options = md::ParseClientOptions(flags);

  auto stub = md::v1::Bbo::NewStub(md::Connect(options.server));
  md::v1::StreamBboRequest request;
  md::FillSubscription(options, request.mutable_subscription());

  grpc::ClientContext context;
  auto reader = stub->Stream(&context, request);
  return md::StreamLoop<md::v1::BboUpdate>(options, reader.get(), Render);
}
