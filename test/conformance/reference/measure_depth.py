"""Measure how much notional the three venues actually publish.

The assignment asks for a 50M notional band. The venues do not publish enough
depth to fill one. That is not a defect in the aggregator -- fully_filled=false
is the correct answer -- but it is the single most suspicious-looking output in
the system, and a reader who meets it without explanation will assume the thing
is broken. So the claim is measured, dated and reproducible rather than
asserted.

Fetches one REST snapshot per venue, merges them with the reference
implementation, and writes a Markdown table plus the raw numbers as JSON.

Run:  python3 measure_depth.py            (writes depth_survey.md / .json)
      python3 measure_depth.py --print    (stdout only)
"""

import json
import os
import sys
import urllib.request
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate_ref as R                                    # noqa: E402

OUT = os.path.dirname(os.path.abspath(__file__))
TIMEOUT = 25

SOURCES = {
    "binance": (
        "https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=5000",
        lambda d: (d["bids"], d["asks"]),
    ),
    "okx": (
        "https://www.okx.com/api/v5/market/books?instId=BTC-USDT&sz=400",
        lambda d: ([l[:2] for l in d["data"][0]["bids"]],
                   [l[:2] for l in d["data"][0]["asks"]]),
    ),
    "bybit": (
        "https://api.bybit.com/v5/market/orderbook?category=spot&symbol=BTCUSDT&limit=200",
        lambda d: (d["result"]["b"], d["result"]["a"]),
    ),
}

BANDS = [1, 5, 10, 25, 50]
OFFSETS = [50, 100, 200, 500, 1000]


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "hermeneutic-depth-survey"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as response:
        return json.loads(response.read().decode())


def side_stats(levels):
    touch = levels[0].px_e8
    total_qty = sum(l.qty_total_e8 for l in levels)
    total_notional = sum(R.notional_e8(l.px_e8, l.qty_total_e8) for l in levels)
    span = abs(levels[-1].px_e8 - touch) * 10000 / touch
    return touch, total_qty, total_notional, span


def main():
    venues = {}
    for name, (url, extract) in SOURCES.items():
        try:
            bids, asks = extract(fetch(url))
        except Exception as exc:                     # noqa: BLE001
            print(f"  {name}: unavailable ({exc})", file=sys.stderr)
            continue
        venues[name] = {"bids": [list(l) for l in bids], "asks": [list(l) for l in asks]}
    if not venues:
        sys.exit("no venue reachable; not overwriting the committed survey")

    captured = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")
    survey = {"captured_utc": captured, "instrument": "BTCUSDT", "per_venue": {},
              "consolidated": {}}

    for name, book in venues.items():
        entry = {}
        for side in ("bids", "asks"):
            levels = R.merge_side({name: book}, side)
            touch, qty, notional, span = side_stats(levels)
            entry[side] = {"levels": len(levels), "qty_e8": qty,
                           "notional_e8": notional, "span_bps": round(span, 1)}
        survey["per_venue"][name] = entry

    for side in ("bids", "asks"):
        levels = R.merge_side(venues, side)
        touch, qty, notional, span = side_stats(levels)
        vol = R.volume_bands(levels, [b * 1_000_000 * R.SCALE for b in BANDS])
        price = R.price_bands(levels, [o * R.SCALE for o in OFFSETS], touch, side)
        survey["consolidated"][side] = {
            "levels": len(levels), "touch_e8": touch, "qty_e8": qty,
            "notional_e8": notional, "span_bps": round(span, 1),
            "volume_bands": [
                {"target_musd": b.notional_target_e8 // (1_000_000 * R.SCALE),
                 "fully_filled": b.fully_filled,
                 "worst_e8": b.worst_price_e8, "vwap_e8": b.vwap_price_e8}
                for b in vol if not b.open_ended],
            "price_bands": [
                {"offset_bps": p.offset_bps_e8 // R.SCALE,
                 "depth_limited": p.depth_limited,
                 "qty_e8": p.qty_e8, "notional_e8": p.notional_e8}
                for p in price if not p.open_ended],
        }

    lines = []
    add = lines.append
    add(f"# Published depth survey — BTCUSDT\n")
    add(f"Captured {captured}. Regenerate with "
        f"`python3 test/conformance/reference/measure_depth.py`.\n")
    add("## Per venue, at maximum available REST depth\n")
    add("| venue | side | levels | BTC | notional | span |")
    add("|---|---|---:|---:|---:|---:|")
    for name, entry in survey["per_venue"].items():
        for side in ("bids", "asks"):
            e = entry[side]
            add(f"| {name} | {side} | {e['levels']} | {R.format_e8(e['qty_e8'])} | "
                f"${R.format_e8(e['notional_e8'])} | {e['span_bps']} bps |")
    add("\n## Consolidated\n")
    add("| side | levels | BTC | notional | span |")
    add("|---|---:|---:|---:|---:|")
    for side in ("bids", "asks"):
        c = survey["consolidated"][side]
        add(f"| {side} | {c['levels']} | {R.format_e8(c['qty_e8'])} | "
            f"${R.format_e8(c['notional_e8'])} | {c['span_bps']} bps |")

    add("\n## What this means for the requested bands\n")
    add("| band | bids | asks |")
    add("|---|---|---|")
    for i, target in enumerate(BANDS):
        cells = []
        for side in ("bids", "asks"):
            b = survey["consolidated"][side]["volume_bands"][i]
            cells.append("fills" if b["fully_filled"] else "**does not fill**")
        add(f"| {target}M | {cells[0]} | {cells[1]} |")
    add("")
    add("| offset | bids | asks |")
    add("|---|---|---|")
    for i, offset in enumerate(OFFSETS):
        cells = []
        for side in ("bids", "asks"):
            p = survey["consolidated"][side]["price_bands"][i]
            cells.append("**depth limited**" if p["depth_limited"] else "within published depth")
        add(f"| {offset} bps | {cells[0]} | {cells[1]} |")

    add("\n## Reading this\n")
    add("The consolidated book spans roughly 100 bps and holds tens of millions of "
        "dollars, not hundreds. So the specification's headline 50M band cannot be "
        "filled from what these venues publish, and the 200/500/1000 bps bands lie "
        "outside the published ladder entirely. Both are reported honestly rather "
        "than hidden: `fully_filled=false` and `depth_limited=true` are the correct "
        "answers, and the trailing open-ended band reports all available liquidity, "
        "which is the meaningful response to \"50M+\" when 50M exceeds the book.")

    text = "\n".join(lines) + "\n"
    if "--print" in sys.argv:
        print(text)
        return
    with open(os.path.join(OUT, "depth_survey.md"), "w") as f:
        f.write(text)
    with open(os.path.join(OUT, "depth_survey.json"), "w") as f:
        json.dump(survey, f, indent=2)
        f.write("\n")
    print(f"wrote depth_survey.md and depth_survey.json ({captured})")


if __name__ == "__main__":
    main()
