// Publisher for the consolidated best bid and offer.
#include <cstdio>

#include "src/clients/common.h"

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);
  const md::ClientOptions options = md::ParseClientOptions(flags);

  auto stub = md::v1::MarketData::NewStub(md::Connect(options.server));

  md::v1::StreamBboRequest request;
  md::FillSubscription(options, request.mutable_subscription());

  grpc::ClientContext context;
  auto reader = stub->StreamBbo(&context, request);

  md::v1::BboUpdate update;
  int count = 0;
  while (reader->Read(&update)) {
    if (options.json) {
      md::PrintJson(update);
    } else {
      const md::v1::Touch& touch = update.touch();
      std::printf("[bbo] %s\n", md::Header(update.meta(), touch).c_str());
      std::printf("      bid %14s x %14s   %s\n", md::FmtPx(update.bid().price_e8()).c_str(),
                  md::FmtQty(update.bid().qty_e8()).c_str(),
                  [&] {
                    std::string venues;
                    for (const auto& contribution : update.bid().venues()) {
                      venues += contribution.venue() + ":" +
                                md::FmtQty(contribution.qty_e8()) + " ";
                    }
                    return venues;
                  }()
                      .c_str());
      std::printf("      ask %14s x %14s   %s\n", md::FmtPx(update.ask().price_e8()).c_str(),
                  md::FmtQty(update.ask().qty_e8()).c_str(),
                  [&] {
                    std::string venues;
                    for (const auto& contribution : update.ask().venues()) {
                      venues += contribution.venue() + ":" +
                                md::FmtQty(contribution.qty_e8()) + " ";
                    }
                    return venues;
                  }()
                      .c_str());
      std::printf("      spread %s (%s bps)  mid %s\n", md::FmtPx(touch.spread_e8()).c_str(),
                  md::Bps(touch.spread_bps_e8()).c_str(), md::FmtPx(touch.mid_price_e8()).c_str());
    }
    // Line-buffered output interleaves cleanly in a combined compose log.
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
