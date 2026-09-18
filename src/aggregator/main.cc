// Aggregator service: connects to three exchanges, maintains one consolidated
// BTCUSDT book, and serves it over gRPC.
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <atomic>
#include <csignal>
#include <signal.h>
#include <thread>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "src/aggregator/engine.h"
#include "src/aggregator/service.h"
#include "src/common/flags.h"
#include "src/venues/binance.h"
#include "src/venues/bybit.h"
#include "src/venues/okx.h"
#include "src/venues/runner.h"

namespace {

// The ONLY thing the handler touches. `volatile sig_atomic_t` is the type the
// standard actually blesses inside a handler, and a single store to it is
// async-signal-safe.
//
// grpc::Server::Shutdown() is emphatically not: it takes internal mutexes,
// allocates, and coordinates with the completion queues. Calling it from a
// handler risks deadlocking against a lock the interrupted thread already
// holds -- and since the signal lands on whichever thread is not blocking it,
// and the sync handler threads are inside gRPC most of the time, that is a real
// possibility rather than a theoretical one. It would hang for the full grace
// period and then be SIGKILLed: exactly the delay this shutdown path exists to
// avoid, failing intermittently rather than consistently.
volatile std::sig_atomic_t g_shutdown = 0;

void HandleSignal(int) { g_shutdown = 1; }

void InstallSignalHandlers() {
  // sigaction rather than std::signal: std::signal has implementation-defined
  // reinstatement semantics, and SA_RESTART is a deliberate choice here.
  struct sigaction action {};
  action.sa_handler = HandleSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  const md::Flags flags(argc, argv);

  if (flags.Has("help")) {
    std::printf(
        "cex-aggregator\n"
        "  --listen=host:port        gRPC listen address (default 0.0.0.0:50051)\n"
        "  --instrument=SYMBOL       instrument to aggregate (default BTCUSDT)\n"
        "  --venues=a,b,c            subset of binance,okx,bybit (default all)\n"
        "  --binance-ws=URL          override the Binance stream endpoint\n"
        "  --binance-rest=URL        override the Binance depth endpoint\n"
        "  --okx-ws=URL              override the OKX stream endpoint\n"
        "  --bybit-ws=URL            override the Bybit stream endpoint\n"
        "  --snapshot-limit=N        Binance REST depth (default 5000)\n"
        "  --staleness-ms=N          exclude a venue silent this long (default 5000)\n"
        "  --ca-file=PATH            CA bundle; empty uses the system trust store\n"
        "  --record-dir=PATH         write raw frame recordings for replay tests\n");
    return 0;
  }

  const std::string listen = flags.Get("listen", "0.0.0.0:50051");
  const std::string instrument = flags.Get("instrument", "BTCUSDT");
  const std::string selected = flags.Get("venues", "binance,okx,bybit");
  const std::string ca_file = flags.Get("ca-file", "");
  const std::string record_dir = flags.Get("record-dir", "");

  md::VenueRunner::Options runner_options;
  runner_options.ca_file = ca_file;

  // Each venue gets its own ring. Single producer, single consumer, so no
  // shared ring and no contention between venues.
  std::vector<std::unique_ptr<md::FeedRing>> rings;
  std::vector<std::unique_ptr<md::VenueRunner>> runners;

  const auto add_venue = [&](std::unique_ptr<md::VenueProtocol> protocol) {
    const int index = static_cast<int>(runners.size());
    if (index >= md::kMaxVenues) {
      std::fprintf(stderr,
                   "configured %d venues but kMaxVenues is %d; raise it in consolidated.h\n",
                   index + 1, md::kMaxVenues);
      std::exit(1);
    }
    md::VenueRunner::Options options = runner_options;
    if (!record_dir.empty()) {
      options.record_path = record_dir + "/" + std::string(protocol->name()) + ".jsonl";
    }
    rings.push_back(std::make_unique<md::FeedRing>(1024));
    runners.push_back(std::make_unique<md::VenueRunner>(index, std::move(protocol),
                                                        rings.back().get(), options));
  };

  const auto wants = [&](const std::string& name) {
    return selected.find(name) != std::string::npos;
  };

  if (wants("binance")) {
    md::BinanceProtocol::Config config;
    config.symbol = instrument;
    config.snapshot_limit = flags.GetInt("snapshot-limit", 5000);
    config.stream_url = flags.Get("binance-ws", config.stream_url);
    config.rest_url = flags.Get("binance-rest", config.rest_url);
    add_venue(std::make_unique<md::BinanceProtocol>(config));
  }
  if (wants("okx")) {
    md::OkxProtocol::Config config;
    // OKX spells the instrument differently from the other two.
    config.symbol = flags.Get("okx-symbol", instrument == "BTCUSDT" ? "BTC-USDT" : instrument);
    config.stream_url = flags.Get("okx-ws", config.stream_url);
    add_venue(std::make_unique<md::OkxProtocol>(config));
  }
  if (wants("bybit")) {
    md::BybitProtocol::Config config;
    config.symbol = instrument;
    config.stream_url = flags.Get("bybit-ws", config.stream_url);
    add_venue(std::make_unique<md::BybitProtocol>(config));
  }

  if (runners.empty()) {
    std::fprintf(stderr, "no venues selected\n");
    return 1;
  }

  md::EngineConfig engine_config;
  engine_config.instrument = instrument;
  engine_config.staleness_timeout = std::chrono::milliseconds(flags.GetInt("staleness-ms", 5000));

  std::vector<md::VenueFeed> feeds;
  for (std::size_t i = 0; i < runners.size(); ++i) {
    feeds.push_back({runners[i]->name(), runners[i]->venue_symbol(), &runners[i]->stats(),
                     rings[i].get()});
  }

  md::Engine engine(engine_config, std::move(feeds));
  engine.Start();
  for (auto& runner : runners) runner->Start();

  md::MarketDataService service(&engine);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  // Keepalive so a peer that stops reading is reaped rather than parking a
  // handler thread forever. A synchronous server-streaming Write() blocks
  // indefinitely when the peer's TCP window is full, and enough stuck clients
  // would exhaust the thread pool and stop new RPCs being served. Conflation
  // protects the book, not the pool -- this is what protects the pool.
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 20000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  builder.SetSyncServerOption(grpc::ServerBuilder::MAX_POLLERS, 16);

  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (server == nullptr) {
    std::fprintf(stderr, "failed to bind %s\n", listen.c_str());
    return 1;
  }
  InstallSignalHandlers();

  // The real shutdown happens here, on an ordinary thread, where calling into
  // gRPC is safe. A 100ms poll is nothing against the grace period it protects.
  std::thread shutdown_watcher([&server] {
    while (g_shutdown == 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server->Shutdown();
  });

  std::printf("aggregator listening on %s, instrument=%s, venues=%zu\n", listen.c_str(),
              instrument.c_str(), runners.size());
  std::fflush(stdout);

  server->Wait();
  // Also releases the watcher if the server stopped for some other reason.
  g_shutdown = 1;
  shutdown_watcher.join();

  // Ordered teardown: stop accepting, then stop feeding, then stop the book.
  for (auto& runner : runners) runner->Stop();
  engine.Stop();
  std::printf("aggregator stopped\n");
  return 0;
}
