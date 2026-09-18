"""Generate a randomised differential corpus for the band walk.

The hand-built fixtures pin the scenarios we reasoned about. This generates the
ones we did not: awkward prices and quantities chosen so that every product
truncates, and -- crucially -- notional targets placed EXACTLY on cumulative
level boundaries and one unit either side of them.

That boundary is where a single-pass walk is most likely to disagree with a
straightforward one. If a band completes exactly at a level, the whole level is
consumed, and re-deriving the quantity from the target instead of taking the
level's own quantity loses a unit to truncation. A random corpus that only ever
lands mid-level would never exercise it.

Run:  python3 gen_random_corpus.py
"""

import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate_ref as R                                    # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures")
VENUES = ["binance", "bybit", "okx"]
CASES = 180


def odd_price(rng, base):
    """A price with digits in every decimal place, so px*qty truncates.

    Strictly positive. A price of zero or below is not a thing any venue can
    publish, it is rejected at ingestion, and zero doubles as the "no
    liquidity" sentinel -- so generating one produces a book that cannot exist
    and a disagreement that means nothing. The jitter is scaled to the base so
    small bases cannot wander below 1.
    """
    span = max(1, min(50, base // 2))
    whole = max(1, base + rng.randint(-span, span))
    frac = rng.randint(1, R.SCALE - 1)
    return f"{whole}.{frac:08d}"


def odd_qty(rng):
    magnitude = rng.choice([rng.randint(1, 9), rng.randint(1, 500)])
    frac = rng.randint(1, R.SCALE - 1)
    return f"{magnitude}.{frac:08d}"


def build_case(rng, index):
    n_venues = rng.randint(1, 3)
    names = rng.sample(VENUES, n_venues)
    base = rng.choice([1, 76, 500, 76547, 250000])

    venues = {}
    for name in names:
        bids, asks = [], []
        for _ in range(rng.randint(1, 9)):
            bids.append([odd_price(rng, base), odd_qty(rng)])
        for _ in range(rng.randint(1, 9)):
            asks.append([odd_price(rng, base + 120), odd_qty(rng)])
        venues[name] = {"bids": bids, "asks": asks}

    merged = {side: R.merge_side(venues, side) for side in ("bids", "asks")}
    # Guard the generator against itself: a book with a non-positive price is
    # unrepresentable in production and any disagreement it produced would be
    # noise rather than a finding.
    for levels in merged.values():
        for level in levels:
            assert level.px_e8 > 0, f"generated a non-positive price: {level.px_e8}"
            assert level.qty_total_e8 > 0, f"generated a non-positive qty"

    # Cumulative notional after each level, on both sides. These are the values
    # a target can land exactly on.
    boundaries = []
    for levels in merged.values():
        cum = 0
        for level in levels:
            cum += R.notional_e8(level.px_e8, level.qty_total_e8)
            boundaries.append(cum)

    targets = set()
    if boundaries:
        for b in rng.sample(boundaries, min(len(boundaries), 3)):
            targets.add(b)          # exactly on the boundary
            targets.add(b - 1)      # one unit short
            targets.add(b + 1)      # one unit past
    # Plus some that land mid-level, and some the book cannot fill at all.
    total = max(boundaries) if boundaries else R.SCALE
    for _ in range(2):
        targets.add(rng.randint(1, max(total, 2)))
    targets.add(total * 4)
    targets = sorted(t for t in targets if t > 0)

    offsets = sorted({rng.randint(1, 5000) * R.SCALE for _ in range(rng.randint(1, 4))})

    venue_filter = None
    if n_venues > 1 and rng.random() < 0.35:
        venue_filter = sorted(rng.sample(names, rng.randint(1, n_venues - 1)))

    return venues, targets, offsets, venue_filter, index


def expected_for(venues, targets, offsets, venue_filter):
    flt = set(venue_filter) if venue_filter else None
    bids = R.merge_side(venues, "bids", venues=flt)
    asks = R.merge_side(venues, "asks", venues=flt)
    touch = R.compute_touch(bids, asks)
    return {
        "touch": vars(touch),
        "merged": {
            "bids": [dict(px_e8=l.px_e8, qty_total_e8=l.qty_total_e8, by_venue=l.by_venue)
                     for l in bids],
            "asks": [dict(px_e8=l.px_e8, qty_total_e8=l.qty_total_e8, by_venue=l.by_venue)
                     for l in asks],
        },
        "volume_bands": {
            "bid": [vars(b) for b in R.volume_bands(bids, targets)],
            "ask": [vars(b) for b in R.volume_bands(asks, targets)],
        },
        "price_bands": {
            "bid": [vars(b) for b in R.price_bands(bids, offsets, touch.best_bid_e8, "bids")],
            "ask": [vars(b) for b in R.price_bands(asks, offsets, touch.best_ask_e8, "asks")],
        },
    }


if __name__ == "__main__":
    rng = random.Random(20260917)
    cases = []
    exact_boundary_targets = 0
    for i in range(CASES):
        venues, targets, offsets, venue_filter, _ = build_case(rng, i)
        expected = expected_for(venues, targets, offsets, venue_filter)
        for side in ("bid", "ask"):
            for band in expected["volume_bands"][side]:
                if band["fully_filled"] and band["filled_notional_e8"] == band["notional_target_e8"]:
                    exact_boundary_targets += 1
        cases.append({
            "name": f"random_{i:03d}",
            "description": (
                f"{len(venues)} venue(s), "
                f"filter={','.join(venue_filter) if venue_filter else 'none'}, "
                f"{len(targets)} notional targets, {len(offsets)} bps offsets"
            ),
            "input": {"venues": venues, "venue_filter": venue_filter, "merge_depth": None},
            "request": {"notional_bands_e8": targets, "offsets_bps_e8": offsets},
            "expected": expected,
        })

    doc = {
        "name": "random_books_corpus",
        "description": (
            "Randomised differential corpus for the band walk. Prices and "
            "quantities carry digits in every decimal place so that every "
            "product truncates, and notional targets are placed exactly on "
            "cumulative level boundaries and one unit either side."
        ),
        "seed": 20260917,
        "cases": cases,
    }
    path = os.path.join(OUT, "random_books_corpus.json")
    with open(path, "w") as f:
        json.dump(doc, f, separators=(",", ":"))
        f.write("\n")
    size = os.path.getsize(path)
    print(f"{len(cases)} cases -> {os.path.normpath(path)} ({size // 1024} KiB)")
    print(f"bands landing EXACTLY on a level boundary: {exact_boundary_targets}")
