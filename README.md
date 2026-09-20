# Consolidated BTCUSDT Market Data Aggregator

Aggregates the BTCUSDT order book from three exchanges into one consolidated
book and serves it over gRPC to three publisher services.

```
   Binance            OKX              Bybit        public WSS (+ REST for Binance)
      └────────────────┼─────────────────┘
                       ▼
                  Aggregator                        one consolidated book
                  gRPC server
      ┌────────────────┼─────────────────┐
      ▼                ▼                 ▼
    BBO         Volume Bands        Price Bands     gRPC clients → stdout
```

C++20, Bazel, `docker compose up`.

## Quick start

```bash
docker compose -f docker/docker-compose.yml up --build
```

* **Needs outbound internet** to reach the exchanges. The tests do not.
* **No API keys.** Spot was chosen over perpetual futures because all three
  venues publish full-depth spot books without authentication.
* **Give the Docker VM 8 GB.** Below that the build is OOM-killed inside gRPC
  and reports `cc1plus: signal 9`, which never mentions memory.
* **The build prints nothing for ~40 minutes. That is not a hang** — BuildKit
  buffers output. Add `--progress=plain` to watch it.
* `docker compose` is a CLI plugin. Without it you get `unknown shorthand flag:
  'f'`, which looks like a bad compose file. Install `docker-compose` and
  `docker-buildx`.

Without Docker (Bazel 9.2; on Linux also needs `lld`, see `.bazelrc`):

```bash
bazel build //...
bazel-bin/src/aggregator/aggregator --listen=127.0.0.1:50051 &
bazel-bin/src/clients/bbo_client          --server=127.0.0.1:50051
bazel-bin/src/clients/volume_bands_client --server=127.0.0.1:50051
bazel-bin/src/clients/price_bands_client  --server=127.0.0.1:50051
bazel-bin/src/clients/status_client       --server=127.0.0.1:50051
```

All three venues reach `LIVE` in about 1.5 seconds.

## What you will see first

Two of the numbers the assignment asks for often cannot be answered, because
the liquidity to answer them is not there. The system says so explicitly instead
of returning a misleading figure, so **this is expected output, not a bug**:

* **`fully_filled=false` on the 50M volume band.** That band asks "if I traded
  50 million dollars right now, what average price would I get?" Usually there
  is nowhere near 50M of resting liquidity, so the honest answer is "you could
  not trade that much — here is what you could trade, and at what price".
* **`depth_limited=true` on the wider bps bands.** Those ask "how much liquidity
  sits within X% of the best price?" The exchanges only publish their books a
  short distance out — roughly 1% — so for 2%, 5% and 10% the true answer is
  "further than anything we can see".

Which of these you see changes minute to minute, so read the flags on each
message rather than assuming from this document.

**Why the book is cut off at 500 bps.** Venue books are never truncated
internally, so the diff stream keeps accumulating price levels far outside any
snapshot for as long as the process runs. If we published all of that, every
number would depend on how long our process had been up, and a 50M trade would
"fill" only by sweeping 8-25% through the book at several percent slippage —
not a price anyone would trade at. So the published book stops 500 bps (5%) from
the best price (`--max-publish-bps`).

Over 2,748 consecutive published states:

| side | filled 50M | why |
|---|---|---|
| bid | 0 of 2,748 (0.0%) | no wall in range |
| ask | 934 of 2,748 (34.0%) | round-number sell walls — 85,000 in 652 of them |

The two sides differ because filling 50M on the ask depends on a large
round-number sell order happening to sit within range, which comes and goes.
**That is why `fully_filled` is reported on every band of every message** rather
than being something a client could look up once. The smaller bands
(1M/5M/10M/25M) fill reliably, and the trailing open-ended band (`50M+`) always
reports whatever liquidity does exist.

The conclusion is robust to both obvious objections. Widening the window
fivefold (106 -> 500 bps) adds only ~9% liquidity, and two snapshot captures a
day apart — bid depth fell 30%, ask rose 17% — never came within $11M of 50M.
Regenerate with `python3 test/conformance/reference/measure_depth.py`.

## Layout

| Path | What |
|---|---|
| `proto/md/v1/market_data.proto` | The API. Start here. |
| `src/core/fixed.h` | Fixed-point arithmetic and the `__int128` rule |
| `src/core/book.h` | One venue's book; sorted vectors, batch apply |
| `src/core/consolidated.*` | Merged ladder, k-way merge, publish bounds |
| `src/core/analytics.*` | Single-pass band walk |
| `src/core/conflating_slot.h` | Aggregator → subscriber hand-off |
| `src/core/spsc_ring.h` | Venue → aggregator hand-off |
| `src/venues/venue.h` | `VenueProtocol`: bytes in, book updates out, no I/O |
| `src/venues/{binance,okx,bybit}.*` | Sequencing and resync rules, one file each |
| `src/venues/runner.*` | All I/O: connect, reconnect, backoff, watchdog |
| `src/net/` | WebSocket and HTTP clients, URL parsing, JSON |
| `src/aggregator/` | Engine (owns the book), gRPC service, main |
| `src/clients/` | The four client binaries |

**`VenueProtocol` has no sockets, no threads and no clock** — sequence
validation, snapshot reconciliation and resync triggers are pure logic, tested
from recorded bytes with no network.

## Design decisions

**Fixed-point, not double.** `int64` scaled 1e8. A price of 100,000 times 0.01
BTC already overflows `int64`, so every product routes through `__int128`. The
scale travels on the wire.

**Sorted vectors, not `std::map`.** The hot loop is a sequential merge scan.
Venue books are never truncated internally — dropping deep levels would make a
later update indistinguishable from an insert.

**One writer per book.** One thread per venue parses in parallel and hands
deltas over a lock-free SPSC ring; one aggregation thread owns every book.

**Conflation is safe downstream, unsafe upstream.** Downstream carries absolute
state, so overwriting loses resolution and nothing else — and is *more* timely
than a queue, which hands a lagging subscriber a stale state. Upstream carries
deltas, so the ring never drops: on a full ring the runner resyncs.

**Crossed books are reported, not clamped.** Three venues at different cadences
means `best_bid >= best_ask` happens routinely. Signed spread, `crossed` flag,
per-venue attribution. Fee-adjusting would bake a trading assumption into a data
service.

**Venue filtering is free** — per-level attribution is required by staleness
exclusion anyway, which is the identical operation.

**Bands are one outward walk**, cumulative from the touch, boundary level
consumed partially, so `filled_qty x vwap == filled_notional`. Thresholds are
client-supplied.

**One TLS stack.** gRPC links BoringSSL, Boost.Asio defaults to OpenSSL, both
export `SSL_*`. `.bazelrc` sets `--@boost.asio//:ssl=boringssl`;
`tools/probe/tls_probe.cc` proves it with a live handshake. Certificates are
verified.

**Venue I/O is async.** Beast's concurrent read/write exemption belongs to
`basic_stream_socket` and does *not* pass through `ssl::stream`. All three
venues are WSS, so a read-thread-plus-ping-thread design would corrupt TLS under
load and look like a venue disconnect.

**Keepalives are application frames** on a fixed cadence — OKX wants the text
`ping`, Bybit wants `{"op":"ping"}` every 20s whether or not data flows.

**Shutdown.** The signal handler sets a flag; a watcher thread calls
`Server::Shutdown()`. SIGTERM to exit in **0.10s**.

## Per-venue sequencing

| Venue | Feed | Seeding | Continuity |
|---|---|---|---|
| Binance | `btcusdt@depth@100ms` | REST `depth?limit=5000` | `U == prev u + 1` |
| OKX | `books` (400 levels) | WS snapshot | `prevSeqId == last seqId` |
| Bybit | `orderbook.200` | WS snapshot | `u == prev u + 1` |

**Binance buffers before it fetches.** The REST request is issued only after the
read loop is running, so updates arriving during the round trip are buffered,
then reconciled: drop `u <= lastUpdateId`, require the first surviving event to
straddle at `U <= id+1 <= u`, replay in order.

**OKX checks continuity before its no-change heartbeat.** OKX repeats the
sequence number when nothing moved; if that shortcut ran first, a gap followed
by a heartbeat would skip validation and diverge silently.

`snapshot_limit` is 5000: measured, that gives $28.2M of depth against $10.4M at
1000, and Binance carries ~79% of the book. Request weight is handled by
rate-limiting resyncs (6/min, backoff with jitter).

**The OKX checksum is deliberately not verified.** It covers the venue's own
decimal *string* rendering, unreproducible after normalising to fixed point, and
only the top 25 levels — nowhere near the depth the bands consume. Continuity
detects *loss*; a checksum detects *divergence*, and divergence is covered
offline for all three venues by the replay test.

## Failure handling

| Condition | Response |
|---|---|
| Sequence gap / parse error | Discard batch, `Reset()`, resync from a fresh snapshot |
| Ingest ring full | Discard batch, force resync — never drop a delta, never block the venue thread |
| Rejected subscription | `kFatal`; venue marked down. Reconnecting cannot fix an unknown symbol |
| Disconnect | Exponential backoff with jitter, reset only once the venue is LIVE again |
| Socket black-holed | Silence watchdog reconnects after 30s with no inbound bytes |
| Venue silent but connected | Excluded from the merge after `--staleness-ms`, reported in `stale_venues` |
| Slow subscriber | Conflated; never back-pressures the book |

The engine publishes on a heartbeat as well as on change, because staleness is
computed at publish time — otherwise the one case staleness exists for (every
venue silent) is the case where nothing is ever published.

## Scalability

**Message rate.** Host-native on macOS/arm64 over 61s: 1559 publishes (~25/s),
p50 **192 µs**, p99 1181 µs. That is ~192 µs of a ~39 ms cycle — **200×
headroom**. (Container figures are not representative; colima shows a 2.0 s max
from VM scheduling.)

**Venues.** `kMaxVenues` is 4; raising it is one constant. The linear scan over
cursor heads beats a heap at this count, and stops being optimal around 8–16.

**Subscribers.** Fan-out is O(subscribers) pointer swaps. The real bound is the
**synchronous gRPC API: one thread per stream**, so the ceiling is in the
hundreds; the callback API is the migration path.

**Instruments.** One per process. The proto carries `instrument`, so scaling is
horizontal.

**Memory.** ~528 KB per snapshot (5500 levels × 48 B × 2 sides), ~13 MB/s at the
measured rate. The price bound stops this growing with uptime.

## Testing

```bash
bazel test //...                  # 11 targets
bazel test --config=debug //...   # assertions live
bazel test --config=tsan //test/unit:concurrency_test //test/unit:retry_test
```

All offline and deterministic. **No test needs network access.**

| Suite | Covers |
|---|---|
| `fixed_test`, `book_test`, `analytics_test` | Parsing, book ordering, bands against hand-computed values |
| `concurrency_test`, `retry_test`, `ws_url_test` | SPSC ring, conflation, shutdown, backoff, rate budget, URL parsing |
| `venue_protocol_test` | Sequencing, resync, fatal verdicts, instrument filtering |
| `grpc_integration_test` | Real engine + server + client: BBO, bands, filtering, validation, staleness, crossed book |
| `analytics_conformance_test`, `parser_conformance_test` | Differential against an independent Python implementation (465 parser cases) |
| `replay_conformance_test` | Real captured sessions per venue, plus 16 constructed failure scenarios |

**Not automatically tested:** `src/net`, the runner's connection lifecycle, and
the gRPC service under real network conditions — covered by manual end-to-end
runs. The pure logic they wrap is covered, because that is where the bugs were.

The conformance suite was written independently of this implementation, from the
specification rather than the code it checks, with a reference implementation in
Python. Disagreements were resolved on their merits before either side changed,
which found real bugs in both. The goldens are verified non-vacuous: perturbing
any expected value by one unit makes the suite fail. Python is offline tooling
only — there are no `py_*` rules in the build.

`tools/check_compose.py` cross-checks compose against the Dockerfile for the
mistakes that build cleanly and fail at `docker run`.

### Eight-hour soak

```
published=38154  latency p50=203us  uptime=28975s
  binance  LIVE  msgs=18697   resync=1  gaps=1  overflow=0
  okx      LIVE  msgs=51521   resync=0  gaps=0  overflow=0
  bybit    LIVE  msgs=108613  resync=0  gaps=0  overflow=0
```

178,831 venue messages, 38,154 publishes, all three venues finishing LIVE.

**`binance resync=1 gaps=1` is the important line.** A real sequence gap occurred
against a live venue, was detected, and the book was rebuilt — through the real
socket, runner, rate limiter and REST reconciliation. No test can produce that;
a test feeds a constructed gap through the protocol in isolation. Sequencing is
the hardest part of this system and the OKX checksum was deliberately dropped,
so this is the strongest single piece of evidence here.

**87 disconnects, every one recovered automatically.** 39 came in multi-venue
clusters (the host VM losing network overnight); the rest were venue-initiated.

## Configuration

```
aggregator:  --listen --instrument --venues --snapshot-limit --staleness-ms
             --max-publish-bps --ca-file --record-dir
             --binance-ws --binance-rest --okx-ws --bybit-ws --okx-symbol
clients:     --server --venues --instrument --min-interval-us --json
             --max-updates --bands-usd --bands-bps
```

Unknown flags, non-integer values and out-of-range values are rejected with a
message naming the flag, the rule and the offending value.

## Requirements traceability

| Requirement | Where | Verified by |
|---|---|---|
| Connect to 3 CEX | `src/venues/{binance,okx,bybit}` | `venue_protocol_test`, `replay_conformance_test` |
| Subscribe BTCUSDT | `VenueProtocol::SubscribeFrames` | replay recordings, live run |
| One consolidated book | `MergeSide`, `engine.cc` | `analytics_test`, conformance, integration |
| gRPC endpoints for subscribers | `proto/md/v1`, `service.cc` | `grpc_integration_test` |
| Timely updates on change | conflating fan-out, heartbeat publish | `concurrency_test`, integration |
| BBO client | `src/clients/bbo_main.cc` | integration, live run |
| Volume bands 1M–50M+ | `src/clients/volume_bands_main.cc` | `analytics_test`, conformance, live run |
| Price bands 50–1000bps+ | `src/clients/price_bands_main.cc` | `analytics_test`, conformance, live run |
| Publish to stdout | all three clients | live run |
| C++ implementation | C++20 throughout | no `py_*` rules in the build |
| Docker file per service | `docker/Dockerfile`, five stages | `docker compose build` |
| Compose, single host | `docker/docker-compose.yml` | `docker compose up`, 8h soak |
| README | this file | — |

Every service gets its own image and build target from one multi-stage
Dockerfile, so gRPC compiles once rather than four times. Built and run on arm64
(Apple Silicon via colima); x86_64 takes the same path but was not executed.

## Known limitations

* **50M is marginal**, and which side fills changes with the book — reported per
  update rather than assumed.
* `GetVenueStatus` has no fatal-reason field; it is logged to stderr instead.
* The OKX checksum could be added with a side table of raw strings inside the
  OKX adapter only.
* Synchronous gRPC server: bounded pool, keepalive to reap half-dead peers, and
  a bounded wait so cancelled subscribers are reclaimed.
* Snapshots are allocated per publish, not pooled — ~13 MB/s, traded for simpler
  code.
* One instrument per process; multi-instrument is additive.
* No raw ladder RPC: ~440 KB per message at full depth, so it would need a
  bounded `max_levels`. The subscription message is factored so adding it is
  additive.
