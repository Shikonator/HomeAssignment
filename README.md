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

**The 50M band often does not fill, and wide bps bands are depth-limited.**
Both are correct. Read them per update rather than trusting this document.

**The published ladder is bounded to 500 bps from the touch**
(`--max-publish-bps`). Venue books are never truncated internally, so the diff
stream accumulates levels far outside any snapshot for as long as the process
runs. Publishing that unbounded would make every number depend on our uptime,
and would let a 50M sweep "fill" by running 800–2500 bps out at several percent
slippage. Internal books stay full depth; only what is published is bounded.

Measured over 2,748 consecutive published states:

| side | filled 50M | rate |
|---|---|---|
| bid | 0 of 2,748 | 0.0% |
| ask | 934 of 2,748 | 34.0% |

The sides differ because the ask fills happen when a large round-number sell
wall sits inside the bound — the worst price touched was 85,000 in 652 of them,
84,500 in 177, 83,000 in 91. **This is why `fully_filled` is per band, per
update, per side**: it is not a property of the configuration that a consumer
could hardcode from a README. 1M/5M/10M/25M fill reliably; the trailing
open-ended band (`50M+`, `1000bps+`) always reports everything inside the bound.

The conclusion is robust. Widening the window nearly fivefold adds ~9%
liquidity, and two snapshot captures a day apart — during which bid depth fell
30% and ask depth rose 17% — never came within $11M of 50M:

| window / date | bid | ask |
|---|---|---|
| ~106 bps (snapshot) | $23.8M | — |
| 500 bps (published ladder) | $25.9M | — |
| 2026-09-17 snapshot | $34.2M | $33.0M |
| 2026-09-18 snapshot | $23.8M | $38.7M |

Regenerate with `python3 test/conformance/reference/measure_depth.py`.

## Assessment criteria

| Criterion | Where |
|---|---|
| Correctness | [Testing](#testing); [Per-venue sequencing](#per-venue-sequencing) |
| Architectural design | [Layout](#layout) — `VenueProtocol` has no sockets, threads or clock |
| API/protocol design | `proto/md/v1/market_data.proto`; band sets live in the request |
| System scalability | [Scalability](#scalability) |
| Code quality and coverage | [Testing](#testing), including what is *not* covered |

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

The key structural decision: **`VenueProtocol` has no sockets, no threads and
no clock.** Sequence validation, snapshot reconciliation and resync triggers are
pure logic, so all of it is tested from recorded bytes with no network.

## Design decisions

**Fixed-point, not double.** Prices and quantities are `int64` scaled 1e8.
A price of 100,000 times 0.01 BTC already overflows `int64`, so every product
routes through `__int128` helpers. The scale travels on the wire in
`SnapshotMeta`.

**Sorted vectors, not `std::map`.** The hot loop is a sequential merge scan;
contiguous storage beats node-chasing. Updates cluster near the touch, so
insertion memmoves are short. Venue books are never truncated internally —
dropping deep levels would make a later update indistinguishable from an insert.

**One writer per book.** One thread per venue parses in parallel and hands
deltas over a lock-free SPSC ring; one aggregation thread owns every book, so
the books need no synchronisation.

**Conflation is safe downstream, unsafe upstream.** Aggregator → subscriber
carries absolute state, so overwriting loses resolution and nothing else — and
is *more* timely than a queue, which would hand a lagging subscriber a stale
state rather than the newest. Venue → aggregator carries deltas, so the ring
never drops: on a full ring the runner discards the batch and resyncs.

**Crossed books are reported, not clamped.** Three venues at different cadences
means `best_bid >= best_ask` happens routinely. The system emits a signed
spread, a `crossed` flag and per-venue attribution. Fee-adjusting would bake a
trading assumption into a data service.

**Venue filtering is free.** Per-level venue attribution is required by
staleness exclusion anyway — it is the identical operation.

**Bands are computed in one outward walk**, cumulative from the touch, with the
boundary level consumed partially. `filled_notional` is the actual swept
notional, so `filled_qty × vwap == filled_notional` holds. Thresholds are
client-supplied with server defaults.

**One TLS stack.** gRPC links BoringSSL; Boost.Asio defaults to OpenSSL, and
both export `SSL_*`. `.bazelrc` sets `--@boost.asio//:ssl=boringssl` so the
process has exactly one. `tools/probe/tls_probe.cc` proves it with a live
handshake. Certificates are verified, with hostname verification.

**Venue I/O is async.** Beast's concurrent read/write exemption belongs to
`basic_stream_socket` and does *not* pass through `ssl::stream`. All three
venues are WSS, so a read-thread-plus-ping-thread design would corrupt TLS under
load and look like a venue disconnect. Each venue runs one `io_context` on one
thread.

**Keepalives are application frames** on a fixed cadence. OKX wants the text
`ping`, Bybit wants `{"op":"ping"}` every 20s whether or not data flows.

**Shutdown.** The signal handler sets a flag and nothing else; a watcher thread
calls `Server::Shutdown()`. Measured: SIGTERM to exit in **0.10s**.

## Per-venue sequencing

| Venue | Feed | Seeding | Continuity |
|---|---|---|---|
| Binance | `btcusdt@depth@100ms` | REST `depth?limit=5000` | `U == prev u + 1` |
| OKX | `books` (400 levels) | WS snapshot | `prevSeqId == last seqId` |
| Bybit | `orderbook.200` | WS snapshot | `u == prev u + 1` |

**Binance buffers before it fetches.** The REST request is issued only after the
read loop is running, so updates arriving during the round trip are buffered.
Reconciliation drops `u <= lastUpdateId`, requires the first surviving event to
straddle at `U <= id+1 <= u`, then replays in order.

**OKX checks continuity before its no-change heartbeat.** OKX repeats the
sequence number when nothing moved; if that shortcut ran first, a gap followed
by a heartbeat would skip validation and diverge silently. Pinned by
`okx_gap_then_no_change_heartbeat_must_resync`.

`snapshot_limit` is 5000, not less: measured, 5000 gives $28.2M of depth against
$10.4M at 1000, and Binance carries ~79% of the consolidated book. Request
weight is handled by rate-limiting resyncs (6/min, backoff with jitter).

**The OKX checksum is deliberately not verified.** It is computed over the
venue's own decimal *string* rendering, which cannot be reproduced after
normalising to fixed point, and it only ever covered the top 25 levels — nowhere
near the depth the volume bands consume. Continuity detects *loss*; a checksum
detects *divergence*. Divergence is covered offline for all three venues by the
replay test, which is broader than one venue's shallow checksum.

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

**Message rate.** Measured host-native on macOS/arm64 over 61s: 1559 publishes
(~25/s), p50 **192 µs**, p99 1181 µs, max 4718 µs. At ~25/s the aggregator has
~39 ms per cycle and uses ~192 µs — roughly **200× headroom** against the
consolidated rate. (Container-measured figures are not representative: the same
build inside colima shows a 2.0 s max from VM scheduling.)

**Venues.** `kMaxVenues` is 4; raising it is one constant. The merge is a linear
scan over cursor heads, which beats a heap at this count and stops being optimal
somewhere around 8–16 venues.

**Subscribers.** Fan-out is O(subscribers) pointer swaps. The real bound is the
**synchronous gRPC API: one thread per active stream**, so the ceiling is in the
hundreds. The callback API is the migration path.

**Instruments.** One per process; the proto carries `instrument` throughout, so
scaling is horizontal.

**Memory.** ~528 KB per published snapshot (5500 levels × 48 B × 2 sides), about
13 MB/s of allocation at the measured rate. The price bound is what stops this
growing with uptime.

## Testing

```bash
bazel test //...                  # 11 targets
bazel test --config=debug //...   # assertions live
bazel test --config=tsan //test/unit:concurrency_test //test/unit:retry_test
```

All offline and deterministic. **No test needs network access.**

| Suite | Covers |
|---|---|
| `fixed_test` | Decimal parsing, formatting, `__int128` overflow |
| `book_test` | Ordering, batch-vs-individual apply equivalence, self-cross |
| `analytics_test` | Bands and touch against hand-computed values |
| `concurrency_test` | SPSC ring, conflation, bounded waits, shutdown |
| `retry_test` | Backoff growth/saturation/reset, resync rate budget |
| `ws_url_test` | URL parsing including IPv6 |
| `venue_protocol_test` | Sequencing, resync, fatal verdicts, instrument filtering |
| `grpc_integration_test` | Real engine + server + client: BBO, bands, filtering, validation, staleness, crossed book |
| `analytics_conformance_test` | Differential against an independent Python implementation |
| `parser_conformance_test` | 465 parser cases, differential |
| `replay_conformance_test` | Real captured sessions replayed per venue, plus 16 constructed failure scenarios |

**Not automatically tested:** `src/net`, the runner's connection lifecycle, and
the gRPC service under real network conditions. Those are covered by manual
end-to-end runs. The pure logic they wrap — URL parsing, backoff, the rate
budget, the conflating slot — is covered, because that is where the bugs were.

The conformance suite was written independently of this implementation, from the
specification and the proto comments rather than from the code it checks, and
contains a reference implementation in Python. Disagreements were resolved on
their merits before either side changed, which found real bugs in both. The
goldens are verified non-vacuous: perturbing any expected value by one unit makes
the suite fail. The Python appears only as offline tooling — there are no `py_*`
rules in the build at all.

`tools/check_compose.py` cross-checks the compose file against the Dockerfile
for the mistakes that build cleanly and fail at `docker run`.

### Eight-hour soak

```
published=38154  latency p50=203us  uptime=28975s
  binance  LIVE  msgs=18697   resync=1  gaps=1  overflow=0
  okx      LIVE  msgs=51521   resync=0  gaps=0  overflow=0
  bybit    LIVE  msgs=108613  resync=0  gaps=0  overflow=0
```

178,831 venue messages, 38,154 publishes, **87 disconnects, one sequence gap** —
detected and resynced correctly. All three venues finished LIVE. 39 of the
disconnects came in multi-venue clusters (the host VM losing network overnight);
the rest were venue-initiated. Every one recovered automatically.

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
Dockerfile, so gRPC compiles once per build rather than four times. Built and
run on arm64 (Apple Silicon via colima); x86_64 takes the same path but was not
executed.

## Known limitations

* **50M is marginal**, and which side fills changes with the book. That is
  reported per update rather than assumed.
* `GetVenueStatus` does not carry the fatal reason text; it is logged to stderr.
* The OKX checksum could be implemented with a side table of raw strings inside
  the OKX adapter only.
* The gRPC server uses the synchronous API — bounded pool, keepalive to reap
  half-dead peers, and a bounded wait so cancelled subscribers are reclaimed.
* Snapshots are allocated per publish rather than pooled: ~13 MB/s, deliberately
  traded for simpler code.
* One instrument per process. The proto carries `instrument` throughout, so
  multi-instrument is additive.
* No raw ladder RPC. At full depth that is ~440 KB per message and ~11 MB/s per
  subscriber, so it would need a bounded `max_levels`; the subscription message
  is factored so adding it is additive.
