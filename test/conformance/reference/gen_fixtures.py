"""Generate golden conformance fixtures from the independent reference.

Each fixture carries venue books in WIRE FORM (decimal strings, exactly as the
exchanges emit them) so that a conformance test exercises the production path
end to end: parse -> merge -> analytics. Expected values come from
aggregate_ref.py, which was written from the specification and the proto
comments rather than from the C++.

Run:  python3 gen_fixtures.py
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate_ref as R                                    # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures")

M = 1_000_000 * R.SCALE                       # 1M USDT of notional, scaled
DEFAULT_NOTIONALS = [1 * M, 5 * M, 10 * M, 25 * M, 50 * M]
DEFAULT_OFFSETS = [b * R.SCALE for b in (50, 100, 200, 500, 1000)]


def lvl(px, qty):
    return [px, qty]


def ladder(start, step, n, qty0, dqty, side, px_dp=2, qty_dp=8):
    """Synthetic ladder as wire-form decimal strings."""
    out = []
    for i in range(n):
        px = start - step * i if side == "bids" else start + step * i
        qty = qty0 + dqty * i
        out.append(lvl(f"{px:.{px_dp}f}", f"{qty:.{qty_dp}f}"))
    return out


# --------------------------------------------------------------------------

def build(name, description, venues, notionals=None, offsets=None,
          venue_filter=None, merge_depth=None, notes=None):
    notionals = DEFAULT_NOTIONALS if notionals is None else notionals
    offsets = DEFAULT_OFFSETS if offsets is None else offsets

    bids = R.merge_side(venues, "bids", venues=venue_filter, depth=merge_depth)
    asks = R.merge_side(venues, "asks", venues=venue_filter, depth=merge_depth)
    touch = R.compute_touch(bids, asks)

    vb_bid = R.volume_bands(bids, notionals)
    vb_ask = R.volume_bands(asks, notionals)
    pb_bid = R.price_bands(bids, offsets, touch.best_bid_e8, "bids")
    pb_ask = R.price_bands(asks, offsets, touch.best_ask_e8, "asks")

    worst = 0
    for side in (bids, asks):
        for l in side:
            worst = max(worst, abs(l.px_e8 * l.qty_total_e8))

    fx = {
        "name": name,
        "description": description,
        "instrument": "BTCUSDT",
        "scale": {"price_e8": R.SCALE, "qty_e8": R.SCALE},
        "input": {
            "venues": venues,
            "venue_filter": sorted(venue_filter) if venue_filter else None,
            "merge_depth": merge_depth,
        },
        "request": {
            "notional_bands_e8": notionals,
            "offsets_bps_e8": offsets,
        },
        "expected": {
            "touch": vars(touch),
            "merged": {
                "bids": [dict(px_e8=l.px_e8, qty_total_e8=l.qty_total_e8) for l in bids],
                "asks": [dict(px_e8=l.px_e8, qty_total_e8=l.qty_total_e8) for l in asks],
            },
            "volume_bands": {"bid": [vars(b) for b in vb_bid],
                             "ask": [vars(b) for b in vb_ask]},
            "price_bands": {"bid": [vars(b) for b in pb_bid],
                            "ask": [vars(b) for b in pb_ask]},
        },
        "properties": {
            # Proof the fixture actually exercises the 128-bit path: a naive
            # int64 px*qty would wrap here. Carried as a STRING precisely
            # because it does not fit in 64 bits -- a strict JSON reader
            # rejects the document outright if it is emitted as a number.
            "max_px_times_qty_intermediate": str(worst),
            "overflows_int64_if_naive": R.exceeds_int64(worst),
            "bid_levels": len(bids),
            "ask_levels": len(asks),
        },
    }
    if notes:
        fx["notes"] = notes
    return fx


def emit(fx):
    path = os.path.join(OUT, fx["name"] + ".json")
    with open(path, "w") as f:
        json.dump(fx, f, indent=2, sort_keys=False)
        f.write("\n")
    p = fx["properties"]
    print(f"  {fx['name']:<24} bids={p['bid_levels']:>4} asks={p['ask_levels']:>4} "
          f"max_intermediate={int(p['max_px_times_qty_intermediate']):.3e} "
          f"overflow={p['overflows_int64_if_naive']}")


FIXTURES = []

# 1 -- small, entirely hand-checkable, overlapping prices across venues.
FIXTURES.append(build(
    "bbo_basic",
    "Three venues, overlapping prices, uncrossed. Small enough to verify by "
    "hand. Overlapping prices must sum across venues, and the sweep "
    "exhausts the book (fully_filled=false).",
    {
        "binance": {
            "bids": [lvl("100000.00", "1.50000000"), lvl("99999.50", "2.00000000"),
                     lvl("99999.00", "3.00000000")],
            "asks": [lvl("100001.00", "1.20000000"), lvl("100001.50", "2.50000000")],
        },
        "okx": {
            "bids": [lvl("100000.00", "0.75000000"), lvl("99999.00", "1.00000000")],
            "asks": [lvl("100000.50", "0.90000000"), lvl("100001.00", "1.10000000")],
        },
        "bybit": {
            "bids": [lvl("99999.50", "4.00000000")],
            "asks": [lvl("100001.00", "0.30000000"), lvl("100002.00", "5.00000000")],
        },
    },
    notes="best_bid 100000.00 = binance 1.5 + okx 0.75 = 2.25. best_ask is "
          "okx alone at 100000.50. Book is far too thin for a 1M sweep, so "
          "every finite band reports fully_filled=false.",
))

# 2 -- the deep sweep. This is the fixture that catches the int64 overflow.
FIXTURES.append(build(
    "deep_50m_sweep",
    "Deep three-venue book carrying well over 500 BTC per side, so every "
    "notional band up to 50M fills. The intermediate px*qty exceeds int64 by "
    "orders of magnitude, so an implementation multiplying scaled price by "
    "scaled quantity in int64 produces wrong numbers here and correct ones on "
    "any shallow fixture.",
    {
        "binance": {
            "bids": ladder(100000.00, 0.01, 500, 0.5, 0.01, "bids"),
            "asks": ladder(100000.50, 0.01, 500, 0.5, 0.01, "asks"),
        },
        "okx": {
            "bids": ladder(99999.90, 0.10, 300, 0.8, 0.02, "bids", px_dp=1),
            "asks": ladder(100000.60, 0.10, 300, 0.8, 0.02, "asks", px_dp=1),
        },
        "bybit": {
            "bids": ladder(100000.00, 0.05, 400, 0.3, 0.015, "bids"),
            "asks": ladder(100000.50, 0.05, 400, 0.3, 0.015, "asks"),
        },
    },
    notes="Venue ladders deliberately share prices at some levels (binance "
          "0.01 tick vs bybit 0.05 vs okx 0.10) so the merge must SUM at shared "
          "prices rather than concatenate.",
))

# 3 -- crossed consolidated book.
FIXTURES.append(build(
    "crossed_book",
    "best_bid > best_ask across venues, which is normal for a consolidated "
    "book and must be reported rather than clamped. spread_e8 and "
    "spread_bps_e8 are negative, crossed=true, and band walks still proceed "
    "from each own-side touch.",
    {
        "binance": {
            "bids": [lvl("100005.00", "2.00000000"), lvl("100004.00", "3.00000000")],
            "asks": [lvl("100006.00", "1.00000000")],
        },
        "okx": {
            # okx is stale/lagging: its ask sits below binance's bid.
            "bids": [lvl("99998.00", "1.00000000")],
            "asks": [lvl("100002.00", "1.50000000"), lvl("100003.00", "2.00000000")],
        },
        "bybit": {
            "bids": [lvl("100001.00", "5.00000000")],
            "asks": [lvl("100004.50", "4.00000000")],
        },
    },
    notes="best_bid 100005.00 (binance) >= best_ask 100002.00 (okx). "
          "mid = 100003.50, spread = -3.00, both signed and both emitted.",
))

# 4 -- degraded: one side entirely absent (P1).
FIXTURES.append(build(
    "empty_ask_side",
    "Every venue has bids but no asks -- reachable before the first venue "
    "syncs, when all venues go stale, and in single-venue filtered views. "
    "has_ask=false, and mid/spread/spread_bps are 0 rather than derived from "
    "a zero price. crossed must be false, not true-by-accident.",
    {
        "binance": {"bids": [lvl("100000.00", "1.00000000")], "asks": []},
        "okx": {"bids": [lvl("99999.00", "2.00000000")], "asks": []},
        "bybit": {"bids": [], "asks": []},
    },
    notes="The trap: computing mid as (best_bid + 0)/2 yields 50000.00 and a "
          "spread of -100000.00, which would also set crossed=true. All three "
          "must be 0/false.",
))

# 5 -- both sides absent.
FIXTURES.append(build(
    "empty_book",
    "No venue has any liquidity. Every scalar is 0, every band list is still "
    "emitted with zeroed members, and nothing divides by zero.",
    {"binance": {"bids": [], "asks": []},
     "okx": {"bids": [], "asks": []},
     "bybit": {"bids": [], "asks": []}},
))

# 6 -- level resting exactly on the bps bound, both sides (P5).
#     best_bid 100000, 50bps -> bound exactly 99500.00
#     best_ask 100000, 50bps -> bound exactly 100500.00
FIXTURES.append(build(
    "bps_bound_exact",
    "A level rests exactly on the 50bps bound on BOTH sides. With outward "
    "rounding the bound is inclusive symmetrically, so both boundary levels "
    "are counted. Truncating toward zero instead would include the bid "
    "boundary and drop the ask boundary for the same offset.",
    {
        "binance": {
            "bids": [lvl("100000.00", "1.00000000"), lvl("99500.00", "2.00000000"),
                     lvl("99499.99", "8.00000000")],
            "asks": [lvl("100100.00", "1.00000000"), lvl("100600.50", "2.00000000"),
                     lvl("100600.51", "8.00000000")],
        },
        "okx": {"bids": [], "asks": []},
        "bybit": {"bids": [], "asks": []},
    },
    offsets=[50 * R.SCALE],
    notes="Touch is 100000.00 / 100100.00, deliberately UNCROSSED so the "
          "crossing behaviour of other fixtures cannot mask a bounds bug. "
          "Both bounds are exact: 100000 * 0.995 = 99500.00 and "
          "100100 * 1.005 = 100600.50. Expect exactly 2 levels and qty 3.0 "
          "per side; the third level on each side sits one unit outside.",
))

# 7 -- venue filter / staleness exclusion (the same operation).
_FILTER_VENUES = {
    "binance": {
        "bids": [lvl("100000.00", "1.00000000"), lvl("99999.00", "2.00000000")],
        "asks": [lvl("100001.00", "1.00000000")],
    },
    "okx": {
        "bids": [lvl("100000.00", "5.00000000"), lvl("99998.00", "7.00000000")],
        "asks": [lvl("100000.50", "3.00000000")],
    },
    "bybit": {
        "bids": [lvl("100000.50", "9.00000000")],
        "asks": [lvl("100000.75", "4.00000000")],
    },
}
FIXTURES.append(build(
    "stale_venue_excluded",
    "Same book with bybit excluded, which is what the engine does when a venue "
    "goes stale: it is simply not a merge input. bybit sets the touch on both "
    "sides here, so removing it must MOVE the touch rather than merely reduce "
    "a quantity -- and it un-crosses the book.",
    _FILTER_VENUES,
    venue_filter={"binance", "okx"},
    notes="With all three venues the touch is bybit 100000.50 / 100000.75 and "
          "the book is crossed. Excluding bybit moves the touch to 100000.00 / "
          "100000.50 and un-crosses it.",
))
FIXTURES.append(build(
    "all_venues_baseline",
    "The same book with every venue contributing, so the excluded-venue case "
    "above can be compared against it.",
    _FILTER_VENUES,
))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print(f"generating {len(FIXTURES)} fixtures ->", os.path.normpath(OUT))
    for fx in FIXTURES:
        emit(fx)
