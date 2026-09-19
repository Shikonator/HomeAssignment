# Conformance suite

Differential tests for the consolidated-book analytics and the venue protocols.

The goldens come from `reference/`, an independent implementation written from
the specification and `market_data.proto` — not from the C++ it checks. Two
implementations agreeing on hand-checkable numbers is evidence; one agreeing
with itself is not. Python's arbitrary-precision integers matter here: the
reference cannot overflow where a naive `int64` does, so it still produces the
correct expected value for cases that break the code under test.

Fixture inputs are exchange-style decimal **strings**, so a run exercises the
whole path: parse → book → merge → analytics.

## Layout

    reference/aggregate_ref.py       independent analytics implementation
    reference/gen_*.py               build the fixtures and corpora
    reference/check_fixtures.py      invariant checks on the goldens themselves
    reference/capture.py             captures a live venue session
    reference/gen_replay_golden.py   replays it independently → expected book
    reference/measure_depth.py       REST depth survey
    fixtures/*.json                  inputs in wire form + expected outputs
    recordings/*.jsonl               real captured sessions, committed

## Running

    bazel test //test/conformance/...           # opt
    bazel test --config=debug //test/...        # with assertions live

    python3 reference/gen_fixtures.py           # regenerate
    python3 reference/check_fixtures.py         # validate the goldens

`check_fixtures.py` checks properties that hold independently of how the
goldens were computed — ladder ordering, `qty_total == sum(by_venue)`, band
monotonicity, VWAP bracketed by touch and worst price, every emitted value
fitting in `int64`. A buggy reference produces self-consistent nonsense, so the
goldens are verified before any C++ is asserted against them.

The goldens are also **non-vacuous**: perturbing any expected value by one unit
makes the suite fail.

## Coverage

| fixture | pins |
|---|---|
| `bbo_basic` | hand-verifiable; per-venue attribution at the touch; a sweep deeper than the book |
| `deep_50m_sweep` | intermediate `px*qty` of 1.1e22 — three orders past `INT64_MAX` |
| `crossed_book` | `best_bid > best_ask`: signed spread, `crossed=true` |
| `empty_ask_side`, `empty_book` | derived fields zeroed, never computed from a zero price |
| `bps_bound_exact` | a level exactly on the 50bps bound, both sides, outward rounding |
| `venue_filter_*` | filtered view re-derived from attribution; excluding a venue un-crosses the book |
| `parser_corpus` | 465 `ParseFixed` cases incl. the `INT64_MAX` boundary, non-ASCII digits, embedded NUL |
| `random_books_corpus` | 180 books with targets placed **exactly on level boundaries** |
| `venue_scenarios` | 16 constructed frame sequences: gaps, heartbeats, mid-stream snapshots, fatal rejections |
| `recordings/*` | real sessions replayed per venue, compared level for level |

Replay is the divergence control that replaces the OKX checksum: continuity
detects **loss**, replay detects a stream correctly sequenced and applied
**wrongly**. It is not circular — the recordings were captured by an
independent client, the seeding snapshot is the venue's own, and the expected
books come from an independent implementation of each venue's rules.

The scenario worth naming is `okx_gap_then_no_change_heartbeat_must_resync`.
OKX repeats its sequence number when nothing moved; if that shortcut is checked
before continuity, a gap followed by a heartbeat skips validation and the book
diverges silently forever.
