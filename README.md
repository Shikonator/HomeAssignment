# Consolidated BTCUSDT Market Data Aggregator

Aggregates the BTCUSDT order book from three centralised exchanges into one
consolidated book, and serves it over gRPC to three publisher services.

```
   Binance            OKX              Bybit          public WSS feeds
      │                │                 │            (+ REST snapshot for Binance)
      └────────────────┼─────────────────┘
                       ▼
                  Aggregator                          one consolidated book
                  gRPC server
                       │
      ┌────────────────┼─────────────────┐
      ▼                ▼                 ▼
    BBO         Volume Bands        Price Bands       gRPC clients → stdout
```

Everything is C++20, built with Bazel, and runs from a single
`docker compose up`.

---

## Quick start

```bash
docker compose -f docker/docker-compose.yml up --build
```

That builds four images and starts the aggregator plus all three publishers.

**Spot, not perpetual futures.** The assignment allows either. Spot was chosen
because the full-depth order book feeds are public on all three venues and need
no API keys, so the only thing required to run this is outbound network access —
which also means a reviewer can run it without provisioning anything.

**Give the Docker VM at least 8 GB of RAM.** Below that the build is
OOM-killed somewhere inside gRPC and reports `cc1plus: signal 9`, which does not
mention memory. See [Build requirements](#build-requirements-and-build-times).

**The build prints nothing until it finishes. That is not a hang.** BuildKit
buffers output, so a first build shows a silent terminal for the better part of
half an hour while gRPC, Boost and protobuf compile from source. To watch it:

```bash
docker compose -f docker/docker-compose.yml build --progress=plain
```

`docker compose` is a CLI **plugin**. On a machine that has only the bare Docker
CLI it is not present, and the error is misleading — `unknown shorthand flag:
'f' in -f`, which reads as "your compose file is malformed" rather than "this
subcommand does not exist". If you see that, install the Compose and Buildx
plugins (`brew install docker-compose docker-buildx`, or Docker Desktop, which
bundles both). Buildx is needed because the Dockerfile uses a BuildKit cache
mount.
**The aggregator needs outbound internet** to reach the exchanges. The test
suites do not — `test/conformance/recordings/*.jsonl` are captured live
sessions committed on purpose, and they are what makes the replay suite
hermetic and runnable offline.

To run without Docker (needs Bazel 9.2; on **Linux** it also needs `lld`, for
the reason documented in `.bazelrc`):

```bash
bazel build //...
bazel-bin/src/aggregator/aggregator --listen=127.0.0.1:50051 &
bazel-bin/src/clients/bbo_client          --server=127.0.0.1:50051
bazel-bin/src/clients/volume_bands_client --server=127.0.0.1:50051
bazel-bin/src/clients/price_bands_client  --server=127.0.0.1:50051
bazel-bin/src/clients/status_client       --server=127.0.0.1:50051
```

All three venues reach `LIVE` in about 1.5 seconds from process start.
`status_client` shows what the aggregator is actually doing:

```
published=1559  latency p50=192us p99=1181us max=4718us  uptime=61s
  binance  BTCUSDT   LIVE  msgs=...  resync=0 gaps=0 overflow=0  bids=5011 asks=5002
  okx      BTC-USDT  LIVE  msgs=...  resync=0 gaps=0 overflow=0  bids=400  asks=400
  bybit    BTCUSDT   LIVE  msgs=...  resync=0 gaps=0 overflow=0  bids=200  asks=200
```

The per-venue level counts are themselves a check that the feeds are what we
think they are: OKX's `books` channel settles at exactly 400 levels a side and
Bybit's `orderbook.200` at exactly 200, while Binance's diff-maintained book
grows past its 5000-level seed. `latency` is venue receive to consolidated
publish, measured inside the aggregator.

---

## What you will see first, and why it is correct

**The 50M volume band does not fill, and the wider bps price bands are
depth-limited.** Both are correct answers, not defects, and you will see them
within seconds of starting the system.

### The published ladder is bounded at 500 bps from the touch

This matters before the numbers do, because it is what makes them mean anything.

Venue books here are maintained from diff streams and are **never truncated
internally** (see `src/core/book.h`). A REST snapshot returns the levels nearest
the touch — about 100 bps of range on BTCUSDT — but the diff stream then
delivers updates for levels far outside that window, and those accumulate for as
long as the process stays connected. Measured: after 40 minutes a running
aggregator held roughly **twice** the notional of a REST snapshot taken at the
same instant, reaching 2,500 bps from the touch.

Publishing that unbounded book would be wrong twice over:

* **It is not reproducible.** Two aggregators started ten minutes apart hold
  different books for the same market at the same instant. A consumer cannot
  reason about a number that depends on our process uptime.
* **It is not economically meaningful.** Unbounded, a 50M sweep does "fill" —
  by running 800–2,500 bps through the book at several percent average
  slippage. That is not a price anyone would trade at; it is deep resting dust
  that happened to tick since we connected.

So the published ladder covers a stated distance from the touch
(`--max-publish-bps`, default 500). The internal books stay full-depth, because
truncating them would break delta application; only what is published is
bounded.

### What that leaves

Measured live, with the 500 bps bound in force:

```
BID   1.000M   vwap 81210.37  worst 81206.90  qty  12.31  filled  1.000M  levels   49
BID   5.000M   vwap 81194.29  worst 81173.10  qty  61.58  filled  5.000M  levels  397
BID  10.000M   vwap 81174.42  worst 81135.50  qty 123.19  filled 10.000M  levels  666
BID  25.000M   vwap 81018.16  worst 80310.00  qty 308.57  filled 25.000M  levels 5336
BID  50.000M   vwap 80993.61  worst 77310.02  qty 319.39  filled 25.869M  levels 5509
BID  50.000M+  vwap 80993.61  worst 77310.02  qty 319.39  filled 25.869M  levels 5509
```

* 1M / 5M / 10M / 25M fill.
* **50M does not** — there is roughly 26M of consolidated liquidity within 500
  bps of the touch. `fully_filled=false` is the honest answer.
* The trailing open-ended band (`50M+`, `1000bps+`) reports **all** liquidity
  inside the bound, which is the meaningful response to "50M+" when 50M exceeds
  what is there.
* Wide bps bands report `depth_limited=true` when their bound lies outside the
  published ladder.

`depth_limited` discriminates rather than being permanently on: in one live
sample the ask side's 1000 bps band was flagged while the bid side genuinely
extended past its bound.

### Independently measured

`test/conformance/reference/depth_survey.md` measures **snapshot depth** — what
a single REST call from each venue contains — and is regenerated with one
command:

```bash
python3 test/conformance/reference/measure_depth.py
```

Two captures a day apart agreed: the consolidated snapshot spans ~100 bps and
holds tens of millions, not hundreds. That is a different quantity from the
running book above, and the README is careful to say which is which — the
running book is deeper, and the 500 bps bound is what makes the two comparable.

The price dependence runs in the helpful direction, which is worth stating
because the instinctive objection is "surely this depends on the price": it
does, weakly. A *higher* price makes a fixed dollar target easier in BTC terms —
50M needs 619 BTC at 80,838 against 653 BTC at 76,547 — and the gap is not close
enough for ordinary movement to close it.

## How this maps to the assessment criteria

| Criterion | Where it is answered |
|---|---|
| **Correctness of implementation** | [Testing](#testing) — eleven offline targets, a differential reference implementation, real captured sessions replayed per venue, and constructed failure scenarios. Plus [Per-venue sequencing](#per-venue-sequencing) for the rules being implemented. |
| **Quality of architectural design** | [Navigating the code](#navigating-the-code) — `VenueProtocol` has no sockets, threads or clock, so every hard part is pure logic. [Threading](#threading-and-what-lock-free-actually-buys-here) — one writer per book by construction. |
| **API/protocol design and extensibility** | `proto/md/v1/market_data.proto`, and [Thresholds live in the request](#thresholds-live-in-the-request) — band sets are a property of the request, not the build. Scale travels on the wire; unknown venues are rejected, not ignored. |
| **System scalability** | [System scalability](#system-scalability) — where each axis binds, and which one binds first. |
| **Code quality and test coverage** | [Testing](#testing), including an explicit statement of what is *not* covered. No C++ file exceeds 390 lines. Comments explain why, not what — and `market_data.proto` is 414 lines of which roughly half are comments, because every semantic decision is documented where the API is defined. |

## Navigating the code

| Path | What lives there |
|---|---|
| `proto/md/v1/market_data.proto` | The API. Start here — every semantic decision is documented in it. |
| `src/core/fixed.h` | Fixed-point arithmetic. The `__int128` discipline everything else depends on. |
| `src/core/book.h` | One venue's book: sorted vectors, batch apply, `ApplyFeedUpdate`. |
| `src/core/consolidated.h/.cc` | The merged ladder and the k-way merge. |
| `src/core/analytics.h/.cc` | The single-pass band walk. Both band families, one traversal. |
| `src/core/conflating_slot.h` | Aggregator → subscriber hand-off. |
| `src/core/spsc_ring.h` | Venue → aggregator hand-off. |
| `src/venues/venue.h` | The `VenueProtocol` interface: bytes in, book updates out, no I/O. |
| `src/venues/{binance,okx,bybit}.{h,cc}` | One file per exchange. Sequencing and resync rules only. |
| `src/venues/runner.{h,cc}` | All the I/O: connect, reconnect, backoff, resync policy, watchdog. |
| `src/net/ws_client.{h,cc}` | Async WebSocket client. |
| `src/net/http_client.{h,cc}` | One-shot async GET, for Binance's REST snapshot. |
| `src/aggregator/engine.{h,cc}` | Owns the consolidated book. One thread, one writer. |
| `src/aggregator/service.{h,cc}` | The gRPC service. |
| `src/clients/` | The four client binaries. |

The single most useful structural idea is in `src/venues/venue.h`:
**`VenueProtocol` has no sockets, no threads and no clock.** Every hard part of
a venue — sequence validation, snapshot reconciliation, resync triggers — is
pure logic, so all of it is testable from recorded bytes with no network. The
runner owns all I/O and knows nothing about any exchange's wire format.

---

## System scalability

Where each axis binds, and which binds first.

**Message rate — roughly 200× headroom.** Measured over a 61-second run: 1559
consolidated publishes (~25/s), p50 aggregation latency **192 µs**, p99 1181 µs,
max 4718 µs. Latency here means venue receive to consolidated publish, measured
inside the aggregator.

At ~25 publishes/s the aggregator has ~39 ms per cycle and uses ~192 µs of it —
about **200× headroom**, against the consolidated rate rather than the more
flattering per-venue 100 ms cadence, which would read as ~440×. Parsing, the
expensive part, is already parallel across venue threads, so the headroom grows
with cores rather than being consumed by more venues.

**Venues — linear, with a known crossover.** `kMaxVenues` is 4 (one spare);
raising it is a one-constant change and the aggregator hard-fails at startup
rather than silently dropping a venue. The merge is a linear scan over cursor
heads rather than a heap, deliberately: with a handful of venues the scan wins
on branch prediction and cache locality. That stops being true somewhere around
8–16 venues, at which point the heap becomes the right structure. The design is
optimal for its actual range, not for all ranges.

**Subscribers — the fan-out is cheap; the threading model is the limit.**
Publishing is O(subscribers) pointer swaps under one mutex, so thousands of
subscribers would be unremarkable at this rate. The binding constraint is the
**synchronous gRPC API: one thread per active stream**, which puts the practical
ceiling in the hundreds. The migration path is the callback API, at a real cost
in readability — see [Known limitations](#known-limitations-and-possible-extensions).

**Instruments — horizontal.** One instrument per process today. The proto
carries `instrument` throughout, so scaling is one process per instrument, which
is the standard shape for this kind of service and keeps a hot instrument from
sharing a book thread with a quiet one.

**Memory — bounded by the price limit, not by the venues.** `MergedLevel` is 48
bytes (price, total quantity, and a 4-slot per-venue array). The published
ladder is capped at 500 bps from the touch, which on BTCUSDT is ~5500 levels a
side: ~264 KB a side, so **~528 KB per published snapshot**, allocated fresh
each time. At the measured ~25 publishes/s that is ~13 MB/s of allocation churn,
which is irrelevant.
The bound is what stops this growing without limit. Internal books accumulate
deep levels for as long as the process runs; publishing them unbounded would
make snapshot size a function of uptime.

## Design decisions

### Fixed-point, not floating point

Prices, quantities and notionals are `int64` scaled by `1e8`. Exchanges publish
exact decimal strings; IEEE-754 cannot hold them exactly, and a double
round-trip perturbs price levels so book keys stop comparing equal. Integers are
exact and compare in one instruction.

This has one non-negotiable consequence. A price of 100,000 is `1e13` at this
scale, and a quantity of **0.01 BTC** is `1e6` — their product is `1e19`, past
`INT64_MAX` (9.22e18). Multiplying two scaled values in `int64` overflows at one
hundredth of a bitcoin. Every such product routes through `__int128` helpers in
`src/core/fixed.h` and never through a bare `*`. `NarrowSaturating` asserts in
debug builds, because a silent clamp would turn a genuine overflow into a
plausible-looking number.

The scale is transmitted in `SnapshotMeta.price_scale` / `qty_scale` so a client
never hardcodes it.

### Sorted vectors, not `std::map`

The hot loop is the k-way merge across venues — a sequential scan. A contiguous
array is prefetch-friendly; `std::map` pays a cache miss per level and 40+ bytes
of overhead for 16 bytes of payload. Insertion is a memmove, but updates cluster
near the touch, so the moved span is short. A frame changing many levels at once
takes a single linear merge instead of N binary-search-plus-shift insertions
(`SideBook::kBatchMergeThreshold`).

Venue books are **never truncated internally**. If deep levels were dropped, a
later update at a dropped price would be indistinguishable from an insert and
the book would silently desync. Truncation happens only at publish time.

### Threading, and what "lock-free" actually buys here

* **One thread per venue.** WebSocket I/O and JSON parsing — the expensive part
  — happen in parallel across venues.
* **Deltas cross to the aggregator over a lock-free SPSC ring.** One producer,
  one consumer, so no CAS; the two indices sit on separate cache lines.
* **One aggregation thread owns every venue book.** Single writer by
  construction, so the books need no synchronisation at all.
* **Fan-out is a one-deep conflating slot per subscriber.** Publishing
  overwrites whatever a subscriber has not collected.

The fan-out uses a mutex, and that is deliberate. These feeds produce roughly
25 publishes per second (measured); an uncontended mutex costs tens of
nanoseconds, and
`std::atomic<std::shared_ptr<T>>` is *not* lock-free on libstdc++ — it uses a
spinlock pool, i.e. a lock with worse semantics. There is no lock-free win
available at this rate. What matters is that **the book writer is never
blocked, no queue is unbounded, and no subscriber can slow another down**, and
all three hold.

### Conflation is safe downstream and unsafe upstream

The same asymmetry decides both queues:

* **Aggregator → subscriber** carries *absolute state*. Dropping an intermediate
  message loses resolution and nothing else, so the conflating slot overwrites
  freely. `meta.sequence` may skip; a gap means conflation, not data loss — and
  the two can never be confused, because every message downstream carries
  complete state, so there is nothing a gap could have lost.

  This is what makes the updates **timely** rather than merely frequent. A queue
  would hand a lagging subscriber the oldest state it had not yet seen; the
  conflating slot always hands it the newest. Under load conflation is strictly
  *more* timely than buffering, not less — the subscriber sees fewer states, and
  every one it sees is current.
* **Venue → aggregator** carries *deltas*. Dropping one corrupts the book
  permanently and undetectably. The ring therefore never drops: on a full ring
  the runner discards the whole batch and forces a resync, which is always safe.
  It does not block the venue thread either — that would back up the receive
  buffer until the exchange disconnected us, turning a hiccup into an outage.

### Crossed consolidated books

`best_bid >= best_ask` happens routinely and is **not an error**. Three
independent venues publish at different cadences over different network paths,
so the consolidated touch is always a blend of instants. A live sample from the
first run:

```
[bbo] seq=233 CROSSED venues=binance,okx,bybit
      bid 76563.80 x 0.65271100   bybit
      ask 76562.14 x 0.12206000   binance
      spread -1.66 (-0.22 bps)  mid 76562.97
```

The aggregator does not clamp it, suppress it, or fee-adjust it. It reports a
**signed** spread, a `crossed` flag, and per-venue attribution at the touch, so
a consumer can see which venue set each side and decide for itself. Adjusting
would require a per-venue fee and latency cost model, which is out of scope and
would bake a trading assumption into a data service.

A conformance fixture makes this concrete: in `venue_filter_binance_okx`,
excluding one venue **un-crosses** the book. That is the same operation as
staleness exclusion, which is the point below.

### Venue filtering is free

Each merged level carries per-venue attribution
(`MergedLevel::by_venue`). That array is not overhead paid for the optional
venue filter — **staleness exclusion is the identical operation**: "re-derive the
ladder including only these venues". The array is required regardless, so the
subscriber-facing filter costs one extra pass and no re-merge.

### Bands

Both families are computed in **one outward walk** of the ladder. Both are
monotonic in distance from the touch, so a single traversal closes both as it
goes, with no allocation.

* Bands are **cumulative from the touch**: the 5M band describes sweeping 5M
  starting at the best price, not the slice between 1M and 5M.
* The boundary level is consumed **partially**, which is what makes the VWAP
  correct. `fully_filled=false` means the *book* ran out, not that a level did.
* `filled_notional` is the **actual** swept notional, not the requested target,
  so `filled_qty × vwap == filled_notional` holds and a consumer can cross-check
  the triple.
* Price-band bounds round **outward on both sides** (bid down, ask up) so a
  level resting exactly on the boundary behaves identically either way.
* The specification's `50M+` and `1000bps+` are real bands: a trailing
  open-ended band sweeping everything to the end of the ladder.

### Why there is no raw ladder RPC

The consolidated book is served through three purpose-built views rather than as
a raw depth ladder. That is deliberate, and it is the sort of decision the
`Subscription` message was factored to make cheap to revisit.

The three specified clients need the touch, sweep prices and banded liquidity —
all of which are *derived from* the consolidated book and are what "updates about
changes in the consolidated book" means in practice. A raw ladder would be the
obvious fourth view, and adding it is additive: `ConsolidatedBook` is already the
shared published object, `MergedLevel` already carries per-venue attribution, and
`LadderView` and the venue filter already exist, so it is a message, an RPC and a
fill function.

The reason it is not there is bandwidth, and it is the same reasoning that caps
band counts. The full ladder is ~5500 levels a side; with per-venue attribution
that is roughly 440 KB per message — ~11 MB/s per subscriber at the measured
rate, and ~26 MB/s at a 60 Hz burst. Any such RPC therefore has to take a
bounded `max_levels` with a small default, and shipping one without that bound
would be a worse answer than not shipping it.

### Thresholds live in the request

`StreamVolumeBandsRequest.notional_bands_e8` and
`StreamPriceBandsRequest.offsets_bps_e8` are client-supplied, with server
defaults when empty. The five bands named in the assignment are a *default*, not
a build-time constant. A typo'd venue name is rejected with `INVALID_ARGUMENT`
rather than silently ignored, because the alternative is handing a client a
plausible book built from venues it did not ask for.

### One TLS stack, not two

gRPC links BoringSSL; Boost.Asio's TLS support defaults to OpenSSL. Both export
`SSL_*` symbols, so linking them into one binary is at best a duplicate-symbol
error and at worst links cleanly and has Beast call BoringSSL through OpenSSL's
struct layouts — a corruption that would present as "the exchange disconnected
us". `.bazelrc` sets:

```
build --@boost.asio//:ssl=boringssl
```

so the process contains exactly **one** TLS implementation, shared by the venue
connections and by gRPC. `tools/probe/tls_probe.cc` exists to prove it: it
constructs gRPC credentials and completes a real WSS handshake to Binance in one
binary. It is kept in the repo as evidence rather than deleted.

Certificates are verified properly, with hostname verification. That is why the
runtime image installs `ca-certificates`.

### Shutdown

A signal handler sets a `volatile sig_atomic_t` and nothing else. A watcher
thread outside the handler calls `grpc::Server::Shutdown()`, because that
function takes internal mutexes and allocates — calling it from a handler can
deadlock against a lock the interrupted thread already holds, which would hang
for the full grace period and then be SIGKILLed. Measured: **SIGTERM to exit in
0.10s**, so `docker compose down` is prompt rather than a per-container timeout.

### Why the venue I/O is asynchronous

Beast documents that one concurrent read and one concurrent write are safe — but
that exemption belongs to `basic_stream_socket` and **does not pass through
`ssl::stream`**, which is documented "Shared objects: Unsafe" with no carve-out.
Concurrent `SSL_read`/`SSL_write` touch the same record-layer state, and either
direction can need to write on its own for a renegotiation or alert. All three
venues are WSS, so a blocking-read-thread plus a ping-writer-thread would
corrupt the TLS stream rarely, under load, and present as a disconnect.

So each venue runs one `io_context` on one thread with everything async on it:
the read loop, a write queue with exactly one outstanding `async_write`, and the
keepalive timer. No mutex, no shared stream.

### Keepalives are application frames

OKX wants a bare text frame containing `ping`; Bybit wants `{"op":"ping"}`.
Neither is satisfied by a WebSocket protocol-level ping, so Beast's built-in
keepalive is explicitly disabled. The ping timer resets on every inbound frame,
so on a live feed almost nothing is sent — the venues' rule is "no data for N
seconds", and book updates are data.

---

## Per-venue sequencing

| Venue | Feed | Seeding | Continuity rule |
|---|---|---|---|
| Binance | `btcusdt@depth@100ms` | REST `/api/v3/depth?limit=5000` | `U == previous u + 1` |
| OKX | `books` (400 levels) | snapshot over WS | `prevSeqId == last seqId` |
| Bybit | `orderbook.200` | snapshot over WS | `u == previous u + 1` |

Two details that are easy to get wrong and are handled:

**Binance buffers before it fetches.** The REST request is issued only *after*
the read loop is running, so updates arriving during the round trip are already
being buffered. Fetching first is the classic way to start with a book that is
silently wrong. Reconciliation then drops events with `u <= lastUpdateId`,
requires the first surviving event to straddle at `U <= id+1 <= u`, and replays
the rest in order.

**OKX validates continuity before handling its no-change heartbeat.** OKX
repeats the sequence number when nothing moved. If that shortcut ran first, a
gap followed by a heartbeat would skip validation entirely and the book would
diverge permanently with no error. The ordering is load-bearing and is pinned by
a test written from OKX's rules rather than from our adapter
(`okx_gap_then_no_change_heartbeat_must_resync`).

`snapshot_limit` is **5000**, not a smaller value. Measured: 5000 → $28.2M of
depth, 1000 → $10.4M. At 1000 the 25M band stops filling from the venue carrying
~79% of consolidated depth. The request-weight cost is handled by rate-limiting
resyncs (6/minute, exponential backoff with jitter), not by fetching less.

### Why the OKX checksum is not verified

OKX publishes a CRC32 over the top 25 levels. It is deliberately not implemented:

* It is computed over the venue's own decimal **string** rendering, which cannot
  be reproduced after normalising to fixed point without retaining raw strings
  for one venue only.
* It only ever covered the **top 25 levels** — nowhere near the ~500 BTC the
  volume bands consume. It never validated the region that matters.
* A subtly wrong implementation resync-loops against a live venue.

Continuity checking detects **loss**; a checksum detects **divergence**. These
are orthogonal, and continuity does not subsume it. Divergence is instead
covered offline, for **all three venues**, by the replay-versus-fresh-snapshot
test below — which is strictly broader than one venue's shallow checksum.

---

## Failure handling

| Condition | Response |
|---|---|
| Sequence gap / parse error | Discard batch, `Reset()`, resync from a fresh snapshot |
| Ingest ring full | Discard batch, force resync (never drop a delta, never block the venue thread) |
| Rejected subscription | `kFatal` — venue marked down permanently. Reconnecting cannot fix an unknown symbol |
| Disconnect | Reconnect with exponential backoff + full jitter, reset **only once the venue is LIVE again** |
| Socket black-holed | Silence watchdog forces a reconnect after 30s with no inbound bytes |
| Venue silent but connected | Excluded from the merge after `--staleness-ms`, reported in `stale_venues` |
| Slow subscriber | Conflated. Never back-pressures the book |

Two subtleties worth calling out. Backoff resets on **success**, not on the
attempt — an endpoint that accepts TCP and drops you a second later is exactly
what a rate-limited venue does, and resetting per attempt would pin every
reconnect at the initial delay and defeat the resync rate limit. And the engine
**publishes on a heartbeat as well as on change**, because staleness is computed
at publish time: without it, the one scenario staleness exists to report — every
venue silent — is the scenario where nothing is ever published and no client is
ever told.

---

## Testing

```bash
bazel test //...                  # everything
bazel test --config=debug //...   # same, with assertions live
bazel test --config=tsan //test/unit:concurrency_test //test/unit:retry_test
```

The ThreadSanitizer run is scoped to the targets that do not pull in gRPC,
which brings enough TSAN noise of its own to bury a real finding. Within that
scope — the SPSC ring, the conflating slot and the retry policy, which is where
the concurrency actually lives — it is clean.

Eleven test targets, all offline and deterministic. **No test needs network
access.**

**What is not automatically tested, stated plainly:** `src/net` (the WebSocket
and HTTP clients), the venue runner's connection lifecycle, and the gRPC service
under real network conditions. Those are exercised by manual end-to-end runs
against the live venues, not by the suite. The pure logic they wrap — URL
parsing, backoff, the resync rate budget, the conflating slot — *is* covered,
because that is where the bugs were.

| Suite | What it covers |
|---|---|
| `//test/unit:fixed_test` | Decimal parsing, formatting, `__int128` overflow behaviour |
| `//test/unit:book_test` | Book ordering, batch-vs-individual apply equivalence, self-cross detection |
| `//test/unit:analytics_test` | Bands and touch against hand-computed values |
| `//test/unit:concurrency_test` | SPSC ring ordering and fullness, conflation, bounded waits, shutdown, non-blocking publish |
| `//test/unit:retry_test` | Backoff growth/saturation/reset, resync rate budget window |
| `//test/unit:ws_url_test` | URL parsing including IPv6 literals |
| `//test/unit:venue_protocol_test` | Sequencing, resync, fatal verdicts, instrument filtering, across all three venues |
| `//test/integration:grpc_integration_test` | Real engine + server + client stub: BBO, both band families, venue filter, request validation, staleness, crossed book |
| `//test/conformance:analytics_conformance_test` | Differential against an independent Python implementation |
| `//test/conformance:parser_conformance_test` | 465 parser cases, differential |
| `//test/conformance:replay_conformance_test` | Real captured sessions replayed per venue, plus 16 constructed failure scenarios |

`tools/check_compose.py` cross-checks `docker-compose.yml` against the
Dockerfile — that every `build.target` names a real stage, that each stage's
entrypoint binary is actually copied into it, and that each client's `--server`
host and port match a real service. Those are the mistakes that build perfectly
cleanly and then fail at `docker run`, twenty minutes later. It does not
validate the Compose schema; `docker compose config` does that.

### What the conformance suite does and does not establish

The conformance suite in `test/conformance/` was written **independently of this
implementation** — from the specification and the proto comments, not from the
code it checks — and contains a reference implementation of the analytics in
Python. Where the two disagreed, the disagreement was resolved on its merits
before either side changed. That process found real bugs in both.

It establishes that two implementations, in two languages, derived
independently, agree on the touch, the k-way merge, both band families, the
open-ended bands, the bps bounds, the degraded cases and the filtered view —
including a `deep_50m_sweep` case whose intermediate `px*qty` reaches 1.1e22,
three orders of magnitude past `INT64_MAX`.

It does **not** establish anything about the gRPC layer; that is covered end to
end by `//test/integration`. Single-writer discipline in the engine is a
structural property — one thread owns every book by construction — rather than
something the tests stress. The primitives that discipline rests on (the SPSC
ring and the conflating slot) are exercised under ThreadSanitizer.

The goldens are also verified to be **non-vacuous**: perturbing any expected
value by a single unit makes the suite fail. A count says how much was written;
that says the tests would notice.

Two things worth singling out:

* **Replay conformance.** Real captured sessions — Binance 251 frames, OKX 139,
  Bybit 208 — replayed through the adapters and compared **level for level**
  against books computed by an independent implementation of each venue's
  documented rules. The recordings were captured by a separate minimal WebSocket
  client, not by this code, so a systematic capture bug cannot cancel itself out.
  This is what replaces the OKX checksum, and it covers all 400 OKX levels
  rather than the top 25.
* **Cross-language checking earned its keep.** The Python reference initially
  accepted `" 1"` (it stripped whitespace) and `"１"` (U+FF11 — Python's `int()`
  accepts fullwidth digits). The C++ parser rejects both, and is right. A C++
  reference implementation would have shared the C++ blind spot.

---

## Build requirements and build times

The first `docker compose build` on a clean machine compiles gRPC, Boost and
protobuf from source: roughly **15–25 minutes**. Subsequent builds reuse a
BuildKit cache mount and take seconds. All four service images share one
`builder` stage, so that cost is paid once, not four times.

Local `bazel build //...` has the same one-time cost and is then incremental.

`MODULE.bazel.lock` is committed. It pins the resolved dependency graph, which
is what makes "builds with Bazel 9.2" a reproducible claim rather than a hope
about whatever the registry serves today.

---

## Configuration

Aggregator (`--help` lists all):

```
--listen=0.0.0.0:50051     gRPC listen address
--instrument=BTCUSDT       instrument to aggregate
--venues=binance,okx,bybit subset of venues
--snapshot-limit=5000      Binance REST depth
--staleness-ms=5000        exclude a venue silent this long
--ca-file=PATH             CA bundle; empty uses the system trust store
--record-dir=PATH          write raw frame recordings for the replay tests
--binance-ws / --okx-ws / --bybit-ws / --binance-rest    endpoint overrides
```

Clients:

```
--server=host:port      --json                  machine-readable output
--venues=a,b            --min-interval-us=N     client-requested conflation
--bands-usd=1000000,... --bands-bps=50,100,...  override the default bands
```

---

## Requirements traceability

| Requirement | Where | Verified by |
|---|---|---|
| Connect to 3 CEX | `src/venues/{binance,okx,bybit}` | `venue_protocol_test`, `replay_conformance_test` |
| Subscribe BTCUSDT market data | `VenueProtocol::SubscribeFrames` | replay recordings, live run |
| Aggregate into one consolidated book | `MergeSide`, `src/aggregator/engine.cc` | `analytics_test`, `analytics_conformance_test`, `grpc_integration_test` |
| Expose gRPC endpoints for subscribers | `proto/md/v1`, `src/aggregator/service.cc` | `grpc_integration_test` |
| Timely updates on book changes | conflating fan-out, heartbeat publish | `concurrency_test`, `grpc_integration_test` |
| Client: Best Bid-Offer | `src/clients/bbo_main.cc` | `grpc_integration_test`, live run |
| Client: Volume bands 1M/5M/10M/25M/50M+ | `src/clients/volume_bands_main.cc` | `analytics_test`, conformance, live run |
| Client: Price bands BBO+50/100/200/500/1000bps+ | `src/clients/price_bands_main.cc` | `analytics_test`, conformance, live run |
| Publish to stdout | all three clients | live run |
| Full implementation in C++ | C++20 throughout | grep below |
| Docker container per service | `docker/Dockerfile`, five stages | see note below |
| Compose file, single host | `docker/docker-compose.yml` | see note below |
| README: build, run, decisions | this file | — |

On "full implementation must be in C++": the built system is C++ only. Python
appears solely as offline test tooling — the independent reference
implementation and the fixture generators — and never in any binary or image.
It is not merely absent from the outputs, it is absent from the build graph:

```bash
grep -rn 'py_binary\|py_test\|py_library\|rules_python' \
     --include=BUILD.bazel --include=MODULE.bazel .    # returns nothing
```

On "Docker container files for every service": every service gets **its own
image and its own build target**, produced from a single multi-stage Dockerfile.
That is one file rather than four by design — all four service images share one
`builder` stage, so gRPC, Boost and protobuf compile once per `docker compose
build` instead of four times. Four near-identical Dockerfiles would quadruple
the slowest part of the build for no benefit.

Every entry in the "Verified by" column names something that was actually
executed, with one exception recorded here rather than glossed: the Docker
images __DOCKER_STATUS__

---

## Known limitations and possible extensions

* **50M never fills** and wide bps bands are always depth-limited. This is the
  market, not the code — see the top of this file. Subscribing to more venues or
  to futures books would deepen it.
* **`GetVenueStatus` does not carry the fatal reason text.** A venue rejected by
  its exchange reports `DISCONNECTED`; the reason is logged to stderr but has no
  proto field. Adding one is additive.
* **The OKX checksum could be implemented** by keeping a side table of raw
  price/size strings for the top 25 levels inside the OKX adapter only, with no
  contamination of the shared book type.
* **The gRPC server uses the synchronous API.** A server-streaming `Write()`
  blocks if a peer stops reading, so the pool is bounded and gRPC keepalive is
  configured to reap half-dead peers, and the stream loop waits with a timeout so
  a cancelled subscriber's thread is always reclaimed. The callback API would
  remove the bound at a significant cost in readability.
* **Snapshots are allocated per publish** rather than pooled — ~13 MB/s of
  short-lived allocation, derived in [System scalability](#system-scalability).
  Measurably irrelevant on any machine this runs on, so pooling was skipped
  deliberately in favour of simpler code.
* **One instrument per process.** The proto carries `instrument` throughout so
  multi-instrument is an additive change, not a redesign.
