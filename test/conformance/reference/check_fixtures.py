"""Validate the golden fixtures against invariants that must hold regardless
of the numbers.

The goldens are only evidence if they are themselves checked: a reference
implementation with a bug produces self-consistent nonsense, and the
conformance suite would then fail correct C++. This file encodes the
properties independently of how aggregate_ref computes them.

Run:  python3 check_fixtures.py
Exit status is non-zero if any invariant fails.
"""

import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate_ref as R                                    # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures")

failures = []


def check(cond, fixture, msg):
    if not cond:
        failures.append(f"{fixture}: {msg}")


def fits_int64(v):
    return R.INT64_MIN <= v <= R.INT64_MAX


def walk_ints(obj, path=""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield from walk_ints(v, f"{path}.{k}")
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from walk_ints(v, f"{path}[{i}]")
    elif isinstance(obj, int) and not isinstance(obj, bool):
        yield path, obj


def validate(path):
    fx = json.load(open(path))
    n = fx["name"]
    exp = fx["expected"]
    t = exp["touch"]
    bids, asks = exp["merged"]["bids"], exp["merged"]["asks"]

    # -- everything that goes on the wire must fit in int64, even though the
    #    intermediate px*qty deliberately does not.
    for p, v in walk_ints(exp, n):
        check(fits_int64(v), n, f"emitted value does not fit int64 at {p}: {v}")
    worst = int(fx["properties"]["max_px_times_qty_intermediate"])
    check(worst >= 0, n, "bad intermediate")
    check(fx["properties"]["overflows_int64_if_naive"] == (worst > R.INT64_MAX), n,
          "overflow flag disagrees with the recorded intermediate")

    # -- ladder shape
    for side, levels, desc in (("bids", bids, "descending"), ("asks", asks, "ascending")):
        pxs = [l["px_e8"] for l in levels]
        ordered = sorted(pxs, reverse=(side == "bids"))
        check(pxs == ordered, n, f"{side} not sorted {desc}")
        check(len(set(pxs)) == len(pxs), n, f"{side} has duplicate price levels")
        for l in levels:
            check(l["qty_total_e8"] > 0, n, f"{side} level with non-positive qty")
            check(l["px_e8"] > 0, n, f"{side} level with non-positive price")

    # -- the merge must equal the sum of its INPUTS at every price.
    #
    # Checked against the raw wire-form input rather than against a derived
    # field, so this catches a merge that sums wrongly -- not merely one that
    # is internally consistent with itself.
    flt = fx["input"]["venue_filter"]
    for side, levels in (("bids", bids), ("asks", asks)):
        expected = {}
        for venue, book in fx["input"]["venues"].items():
            if flt is not None and venue not in flt:
                continue
            for px_s, qty_s in book.get(side, []):
                px, qty = R.parse_e8(px_s), R.parse_e8(qty_s)
                if qty == 0:
                    continue
                expected[px] = expected.get(px, 0) + qty
        got = {l["px_e8"]: l["qty_total_e8"] for l in levels}
        check(got == expected, n,
              f"{side} merge does not equal the sum of its inputs "
              f"({len(got)} levels vs {len(expected)} expected)")

    # -- touch
    check(t["has_bid"] == bool(bids), n, "has_bid disagrees with ladder")
    check(t["has_ask"] == bool(asks), n, "has_ask disagrees with ladder")
    check(t["best_bid_e8"] == (bids[0]["px_e8"] if bids else 0), n, "best_bid mismatch")
    check(t["best_ask_e8"] == (asks[0]["px_e8"] if asks else 0), n, "best_ask mismatch")
    if t["has_bid"] and t["has_ask"]:
        check(t["spread_e8"] == t["best_ask_e8"] - t["best_bid_e8"], n, "spread mismatch")
        check(t["mid_price_e8"] == R.trunc_div(t["best_bid_e8"] + t["best_ask_e8"], 2),
              n, "mid mismatch")
        check(t["crossed"] == (t["best_bid_e8"] >= t["best_ask_e8"]), n, "crossed mismatch")
        check((t["spread_e8"] < 0) == (t["spread_bps_e8"] < 0), n,
              "spread and spread_bps disagree in sign")
    else:
        # P1: never derived from a zero price.
        for f in ("mid_price_e8", "spread_e8", "spread_bps_e8"):
            check(t[f] == 0, n, f"{f} must be 0 when a side is absent, got {t[f]}")
        check(t["crossed"] is False, n, "crossed must be false when a side is absent")

    # -- volume bands
    for side in ("bid", "ask"):
        vb = exp["volume_bands"][side]
        finite = [b for b in vb if not b["open_ended"]]
        opens = [b for b in vb if b["open_ended"]]
        check(len(opens) == 1, n, f"{side}: expected exactly one open-ended volume band")
        op = opens[0]
        check(op is vb[-1], n, f"{side}: open-ended band must be last")
        check(op["notional_target_e8"] == 0, n,
              f"{side}: open-ended target must be 0 to stay an unambiguous key")
        check(op["fully_filled"] is False, n,
              f"{side}: open-ended band must report fully_filled=false")

        targets = [b["notional_target_e8"] for b in finite]
        check(targets == sorted(targets), n, f"{side}: volume bands not ascending")
        check(len(set(targets)) == len(targets), n, f"{side}: duplicate volume targets")

        for b in vb:
            check(b["filled_notional_e8"] <= op["filled_notional_e8"], n,
                  f"{side}: band exceeds all available liquidity")
            check(b["vwap_price_e8"] == R.vwap_e8(b["filled_notional_e8"], b["filled_qty_e8"]),
                  n, f"{side}: vwap not consistent with notional/qty")
            if b["filled_qty_e8"] > 0:
                touch_px = t["best_bid_e8"] if side == "bid" else t["best_ask_e8"]
                lo, hi = sorted((touch_px, b["worst_price_e8"]))
                check(lo <= b["vwap_price_e8"] <= hi, n,
                      f"{side}: vwap {b['vwap_price_e8']} outside [touch, worst]")
            if b["fully_filled"]:
                # Truncation of the boundary level lands just under the target;
                # it must never land meaningfully under it.
                shortfall = b["notional_target_e8"] - b["filled_notional_e8"]
                check(0 <= shortfall < R.SCALE, n,
                      f"{side}: filled band short of target by {shortfall}")
        for a, b in zip(finite, finite[1:]):
            check(a["filled_qty_e8"] <= b["filled_qty_e8"], n,
                  f"{side}: cumulative qty not monotonic across bands")
            check(a["levels_consumed"] <= b["levels_consumed"], n,
                  f"{side}: levels_consumed not monotonic across bands")

    # -- price bands
    for side in ("bid", "ask"):
        pb = exp["price_bands"][side]
        # Bands are emitted unconditionally, zeroed on an empty ladder, so the
        # repeated field always matches the request one for one.
        check(len(pb) == len(fx["request"]["offsets_bps_e8"]) + 1, n,
              f"{side}: price bands must be one per requested offset plus the "
              f"open-ended band, got {len(pb)}")
        finite = [b for b in pb if not b["open_ended"]]
        opens = [b for b in pb if b["open_ended"]]
        check(len(opens) == 1, n, f"{side}: expected exactly one open-ended price band")
        op = opens[0]
        check(op is pb[-1], n, f"{side}: open-ended price band must be last")

        offs = [b["offset_bps_e8"] for b in finite]
        check(offs == sorted(offs), n, f"{side}: price bands not ascending by offset")
        touch_px = t["best_bid_e8"] if side == "bid" else t["best_ask_e8"]
        for b in finite:
            expect = R.bound_price(touch_px, b["offset_bps_e8"],
                                   "bids" if side == "bid" else "asks")
            check(b["bound_price_e8"] == expect, n,
                  f"{side}: bound {b['bound_price_e8']} != outward-rounded {expect}")
            check(b["qty_e8"] <= op["qty_e8"], n, f"{side}: band exceeds full ladder")
            check(b["vwap_price_e8"] == R.vwap_e8(b["notional_e8"], b["qty_e8"]), n,
                  f"{side}: price-band vwap inconsistent")
        for a, b in zip(finite, finite[1:]):
            check(a["qty_e8"] <= b["qty_e8"], n, f"{side}: price-band qty not monotonic")
            check(a["levels"] <= b["levels"], n, f"{side}: price-band levels not monotonic")


if __name__ == "__main__":
    # Identify ladder fixtures by SHAPE, not by filename. Corpora, replay
    # goldens and scenario files live in the same directory and carry different
    # structures; a naming convention silently breaks the next time a file is
    # added that does not follow it, which it already did twice.
    paths = []
    for candidate in sorted(glob.glob(os.path.join(FIXTURES, "*.json"))):
        try:
            with open(candidate) as f:
                doc = json.load(f)
        except (OSError, ValueError):
            continue
        if isinstance(doc, dict) and "merged" in doc.get("expected", {}):
            paths.append(candidate)
    if not paths:
        sys.exit("no fixtures found; run gen_fixtures.py first")
    for p in paths:
        validate(p)
    print(f"checked {len(paths)} fixtures")
    if failures:
        print(f"\n{len(failures)} INVARIANT FAILURE(S):")
        for f in failures:
            print("  -", f)
        sys.exit(1)
    print("all invariants hold")
