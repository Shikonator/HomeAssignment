# hermeneutic — consolidated BTCUSDT market data aggregator

Take-home assignment. Connects to Binance, OKX and Bybit, merges their BTCUSDT
order books into one consolidated book, serves it over gRPC to three publisher
clients. C++20, Bazel, Docker.

Full reasoning is in `README.md`. This file is the things a session needs that
are NOT obvious from the code.

## Build and test

```bash
bazel build //...
bazel test  //...                 # 11 targets
bazel test  --config=debug //...  # assertions live (NarrowSaturating aborts)
bazel test  --config=tsan //test/unit:concurrency_test //test/unit:retry_test
```

**It is Bazel, not CMake.** `cmake-build-debug/` is a leftover from the CLion
skeleton this started as. Ignore it; do not resurrect it.

Run it:

```bash
bazel-bin/src/aggregator/aggregator --listen=127.0.0.1:50051 &
bazel-bin/src/clients/bbo_client    --server=127.0.0.1:50051
bazel-bin/src/clients/status_client --server=127.0.0.1:50051   # also the healthcheck
```

Needs outbound internet. No API keys — all three venues are public feeds.
Venues reach LIVE in ~1.5s.

## Layout

```
proto/md/v1/     one file per service: bbo, volume_bands, price_bands, status
                 + common.proto for shared messages. START HERE.
src/core/        fixed-point, book, merge, analytics, SPSC ring, conflating slot
src/venues/      VenueProtocol (pure logic) + one file per exchange + runner (all I/O)
src/net/         websocket client, http client, url parsing, json
src/aggregator/  engine (owns the book), four gRPC services, main
src/clients/     four client binaries
test/unit/       ours
test/integration/ real engine + server + client, no network
test/conformance/ NOT OURS — see below
```

## Rules

**Do not edit `test/conformance/`.** It is written and owned by a separate
reviewer session, deliberately independent of this implementation — it contains
a Python reference implementation derived from the spec, not from our code. Its
value is that it can disagree with us. If it fails, bring the disagreement to
the user rather than editing the fixtures to match.

**Do not add a comment unless removing it would let someone reverse a decision
by accident.** src/ sits at ~15% comments and two passes were spent getting it
there. The load-bearing ones are marked by explaining *why*, not *what*.

## Decisions that look wrong but are not

- **Every product of two scaled values goes through `__int128`.** At 1e8 scale a
  $100k price times 0.01 BTC already overflows int64. Never write a bare `*` on
  two `_e8` values — use the helpers in `src/core/fixed.h`.
- **Venue books are never truncated internally**, only at publish time. Dropping
  deep levels makes a later update indistinguishable from an insert.
- **The published ladder is bounded to 500 bps** (`--max-publish-bps`). Without
  it, results depend on process uptime, because the diff stream accumulates
  far-out levels forever. This is a correctness fix, not a bandwidth one.
- **Crossed books (`best_bid >= best_ask`) are normal** and reported, not
  clamped. Three venues, three cadences.
- **OKX checks sequence continuity BEFORE its no-change heartbeat shortcut.**
  Reversing those two lines reintroduces silent divergence. Pinned by
  `okx_gap_then_no_change_heartbeat_must_resync`.
- **Keepalives fire on a fixed cadence, not when idle.** Bybit wants a ping
  every 20s regardless of traffic; idle-gated pings never fire on a busy feed
  and the venue drops us.
- **`VenueProtocol` has no sockets, threads or clock.** Keep it that way — it is
  why every sequencing path is testable from recorded bytes.
- **No `reserved` ranges in the protos.** Deliberately removed: nothing has ever
  been deleted, and 1–15 are the one-byte field tags.
- **Venue I/O is async** because `ssl::stream` does not inherit
  `basic_stream_socket`'s concurrent read/write exemption.

## Environment gotchas

- `.bazelrc` sets `--@boost.asio//:ssl=boringssl` so grpc and Beast share one
  TLS stack. Removing it gives duplicate `SSL_*` symbols.
- `build:linux --linkopt=-fuse-ld=lld` — GNU gold crashes linking aarch64
  binaries this size, at the link step, after the whole compile.
- Docker build takes ~40 min on first run and prints NOTHING until it finishes
  (BuildKit buffers). Needs an 8 GB VM.
- macOS has no `timeout`; use `perl -e 'alarm N; exec @ARGV' ...`.
- zsh does not word-split unquoted parameters, which can make a shell test pass
  for the wrong reason.

## State

16+ commits on `main`. **Not pushed — no git remote yet.** That is the one
incomplete deliverable; it needs the user's GitHub account.
