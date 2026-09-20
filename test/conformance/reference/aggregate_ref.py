"""Independent reference implementation of the consolidated-book analytics.

Written from the specification and market_data.proto, deliberately WITHOUT
reading the C++ it checks. Never "fix" this file to match the C++ -- a
disagreement means one of them is wrong and which has to be established.

Python's arbitrary-precision integers are the point: this cannot overflow
where a naive int64 does, so it still yields the correct expected value for
cases that break the code under test.
"""

from dataclasses import dataclass, field

SCALE = 100_000_000          # 1e8
BPS_DENOM = 10_000
INT64_MAX = 2**63 - 1
INT64_MIN = -(2**63)


# --------------------------------------------------------------------------
# fixed point
# --------------------------------------------------------------------------

def _ascii_digits(s: str) -> bool:
    return all("0" <= c <= "9" for c in s)


def parse_e8(s: str) -> int:
    """Exchange decimal string -> int64 scaled by 1e8, truncating toward zero.

    Accepts the shapes the three venues actually emit: "0", "100000.25",
    "0.00010000", "1000". Rejects exponent notation loudly rather than
    guessing, since none of the three feeds use it for book levels.
    """
    # Deliberately NOT stripped. Surrounding whitespace is garbage, and a
    # parser that quietly tolerates it would also quietly accept a corrupted
    # frame; the venue feeds never pad their numbers.
    if not s:
        raise ValueError("empty numeric string")
    if "e" in s or "E" in s:
        raise ValueError(f"exponent notation not supported: {s!r}")
    neg = s.startswith("-")
    if s[:1] in ("+", "-"):
        s = s[1:]
    if "." in s:
        whole, frac = s.split(".", 1)
    else:
        whole, frac = s, ""
    # At least one digit must appear somewhere: "." and "-" are not numbers.
    if not (whole + frac):
        raise ValueError(f"no digits: {s!r}")
    # str.isdigit() is Unicode-aware: it accepts fullwidth digits (U+FF10..),
    # Devanagari digits and more, and int() converts them happily. An exchange
    # feed is ASCII, and a parser that accepts "１２３" is one that accepts a
    # corrupted frame. Restrict to ASCII explicitly.
    if whole and not _ascii_digits(whole):
        raise ValueError(f"not a decimal: {s!r}")
    if frac and not _ascii_digits(frac):
        raise ValueError(f"not a decimal: {s!r}")
    if not whole:
        whole = "0"
    frac = (frac + "0" * 8)[:8]          # pad, and TRUNCATE beyond 8dp
    v = int(whole) * SCALE + int(frac)
    return -v if neg else v


def format_e8(v: int) -> str:
    neg = v < 0
    v = abs(v)
    return f"{'-' if neg else ''}{v // SCALE}.{v % SCALE:08d}"


def trunc_div(num: int, den: int) -> int:
    """Truncate toward zero, matching C++ integer division."""
    if den == 0:
        raise ZeroDivisionError
    q = abs(num) // abs(den)
    return -q if (num < 0) != (den < 0) else q


def notional_e8(px_e8: int, qty_e8: int) -> int:
    """px * qty, rescaled back to 1e8.

    The intermediate px_e8 * qty_e8 exceeds int64 for any level above roughly
    0.01 BTC at BTC-scale prices, which is why the C++ must do this in
    __int128. Here it is exact by construction.
    """
    return trunc_div(px_e8 * qty_e8, SCALE)


def vwap_e8(total_notional_e8: int, total_qty_e8: int) -> int:
    if total_qty_e8 == 0:
        return 0
    return trunc_div(total_notional_e8 * SCALE, total_qty_e8)


def exceeds_int64(v: int) -> bool:
    return v > INT64_MAX or v < INT64_MIN


# --------------------------------------------------------------------------
# book model
# --------------------------------------------------------------------------

@dataclass
class MergedLevel:
    px_e8: int
    qty_total_e8: int       # summed over the venues that contributed


@dataclass
class Touch:
    best_bid_e8: int = 0
    best_ask_e8: int = 0
    mid_price_e8: int = 0
    spread_e8: int = 0
    spread_bps_e8: int = 0
    crossed: bool = False
    has_bid: bool = False
    has_ask: bool = False


def merge_side(venue_books: dict, side: str, venues=None, depth=None):
    """K-way merge of per-venue ladders into one consolidated ladder.

    `venues` restricts which venues contribute. That is what staleness
    exclusion does: a stale venue is simply not a merge input.
    """
    assert side in ("bids", "asks")
    totals = {}
    for venue, book in venue_books.items():
        if venues is not None and venue not in venues:
            continue
        for px_s, qty_s in book.get(side, []):
            px, qty = parse_e8(px_s), parse_e8(qty_s)
            if qty == 0:                 # qty 0 deletes; never a resting level
                continue
            totals[px] = totals.get(px, 0) + qty

    levels = [MergedLevel(px, qty) for px, qty in totals.items()]
    levels.sort(key=lambda l: l.px_e8, reverse=(side == "bids"))
    if depth is not None:
        levels = levels[:depth]
    return levels


def compute_touch(bids, asks) -> Touch:
    t = Touch()
    t.has_bid = bool(bids)
    t.has_ask = bool(asks)
    t.best_bid_e8 = bids[0].px_e8 if bids else 0
    t.best_ask_e8 = asks[0].px_e8 if asks else 0
    if not (t.has_bid and t.has_ask):
        # One side empty: mid and spread are not defined. Report zero rather
        # than a number derived from a sentinel, and never claim crossed.
        return t
    t.mid_price_e8 = trunc_div(t.best_bid_e8 + t.best_ask_e8, 2)
    t.spread_e8 = t.best_ask_e8 - t.best_bid_e8          # signed
    if t.mid_price_e8 != 0:
        t.spread_bps_e8 = trunc_div(t.spread_e8 * BPS_DENOM * SCALE, t.mid_price_e8)
    t.crossed = t.best_bid_e8 >= t.best_ask_e8
    return t


# --------------------------------------------------------------------------
# volume bands: "sweep N dollars of notional, what price do I get?"
# --------------------------------------------------------------------------

@dataclass
class VolumeBand:
    notional_target_e8: int
    vwap_price_e8: int = 0
    worst_price_e8: int = 0
    filled_qty_e8: int = 0
    filled_notional_e8: int = 0
    fully_filled: bool = False
    levels_consumed: int = 0
    open_ended: bool = False


def volume_bands(levels, targets, max_intermediate=None):
    """Cumulative sweeps from the touch, plus one trailing open-ended band.

    A single walk outward serves every target because the targets are
    monotonic in distance from the touch; the reference does the walk once
    per band for clarity, which must produce identical numbers.
    """
    out = []
    for target in sorted(targets):
        out.append(_sweep(levels, target, open_ended=False,
                          max_intermediate=max_intermediate))
    # The "50M+" of the specification: everything the consolidated book has.
    # Target is 0, so notional_target_e8 stays an unambiguous key across the set.
    out.append(_sweep(levels, None, open_ended=True, echo_target=0,
                      max_intermediate=max_intermediate))
    return out


def _sweep(levels, target_e8, open_ended, echo_target=0, max_intermediate=None):
    """Walk outward from the touch accumulating qty and notional.

    `fully_filled` answers "did the BOOK run out before the target was met",
    NOT "did filled_notional reach the target exactly". Those differ: the
    boundary level is consumed partially and the partial quantity truncates
    toward zero, so a satisfied target normally lands a hair BELOW it. Testing
    cum_notional >= target would report a filled sweep as unfilled on almost
    every real book.
    """
    band = VolumeBand(
        notional_target_e8=(echo_target if open_ended else target_e8),
        open_ended=open_ended,
    )
    cum_qty = 0
    cum_notional = 0
    target_met = False

    for lvl in levels:
        lvl_notional = notional_e8(lvl.px_e8, lvl.qty_total_e8)
        if max_intermediate is not None:
            max_intermediate[0] = max(max_intermediate[0],
                                      abs(lvl.px_e8 * lvl.qty_total_e8))

        if open_ended or cum_notional + lvl_notional <= target_e8:
            take_qty, take_notional = lvl.qty_total_e8, lvl_notional
        else:
            # Partial consumption of the boundary level.
            remaining = target_e8 - cum_notional
            take_qty = trunc_div(remaining * SCALE, lvl.px_e8)
            if take_qty <= 0:
                # The residual is smaller than one representable unit of
                # quantity at this price (under a hundredth of a cent on a
                # 1M target). The target is met; the level is not touched.
                target_met = True
                break
            take_notional = notional_e8(lvl.px_e8, take_qty)
            cum_qty += take_qty
            cum_notional += take_notional
            band.levels_consumed += 1
            band.worst_price_e8 = lvl.px_e8
            target_met = True
            break

        cum_qty += take_qty
        cum_notional += take_notional
        band.levels_consumed += 1
        band.worst_price_e8 = lvl.px_e8
        if not open_ended and cum_notional >= target_e8:
            target_met = True
            break

    band.filled_qty_e8 = cum_qty
    band.filled_notional_e8 = cum_notional
    band.vwap_price_e8 = vwap_e8(cum_notional, cum_qty)
    # Open-ended exhausts the ladder by construction, so "the target was met"
    # has no meaning for it; report False rather than a vacuous True.
    band.fully_filled = (not open_ended) and target_met
    return band


# --------------------------------------------------------------------------
# price bands: "how much liquidity rests within X bps of the touch?"
# --------------------------------------------------------------------------

@dataclass
class PriceBand:
    offset_bps_e8: int
    bound_price_e8: int = 0
    qty_e8: int = 0
    notional_e8: int = 0
    vwap_price_e8: int = 0
    levels: int = 0
    open_ended: bool = False
    depth_limited: bool = False


# bps offsets are themselves scaled by 1e8, so a full 100% is 1e4 bps * 1e8.
BPS_DENOM_E8 = BPS_DENOM * SCALE


def bound_price(touch_px_e8: int, offset_bps_e8: int, side: str) -> int:
    """Inclusive worst price of a bps band, measured from the OWN-SIDE touch.

    Rounding is OUTWARD on both sides: down for bids, up for asks. Truncating
    toward zero would widen the bid band and narrow the ask band by up to one
    unit, so a level resting exactly on the boundary would be included on one
    side and excluded on the other for the same offset.
    """
    if side == "bids":
        num = touch_px_e8 * (BPS_DENOM_E8 - offset_bps_e8)
        return num // BPS_DENOM_E8                    # floor
    num = touch_px_e8 * (BPS_DENOM_E8 + offset_bps_e8)
    return -((-num) // BPS_DENOM_E8)                  # ceil


def price_bands(levels, offsets_e8, touch_px_e8, side):
    out = []
    if touch_px_e8 == 0:
        # Empty ladder. Emit zeroed bands anyway so the repeated field stays
        # aligned with what the subscriber asked for -- a consumer should never
        # have to distinguish "no band" from "empty band". Each finite band IS
        # depth limited here: the ladder ended before its bound was reached.
        # The open-ended band's bound is the worst price present, which is
        # reached by construction, so it is never depth limited.
        for off in sorted(offsets_e8):
            out.append(PriceBand(offset_bps_e8=off, depth_limited=True))
        out.append(PriceBand(offset_bps_e8=0, open_ended=True))
        return out
    for off in sorted(offsets_e8):
        bound = bound_price(touch_px_e8, off, side)
        band = PriceBand(offset_bps_e8=off, bound_price_e8=bound)
        ran_out = True
        for lvl in levels:
            inside = lvl.px_e8 >= bound if side == "bids" else lvl.px_e8 <= bound
            if not inside:
                ran_out = False          # stopped at the bound, not at the end
                break
            band.qty_e8 += lvl.qty_total_e8
            band.notional_e8 += notional_e8(lvl.px_e8, lvl.qty_total_e8)
            band.levels += 1
        band.vwap_price_e8 = vwap_e8(band.notional_e8, band.qty_e8)
        # The published ladder ended before the bound was reached, so this band
        # reports all available depth rather than depth within the offset.
        band.depth_limited = ran_out
        out.append(band)

    # The "1000bps+" of the specification: to the end of the ladder.
    tail = PriceBand(offset_bps_e8=0, open_ended=True)
    for lvl in levels:
        tail.qty_e8 += lvl.qty_total_e8
        tail.notional_e8 += notional_e8(lvl.px_e8, lvl.qty_total_e8)
        tail.levels += 1
    tail.bound_price_e8 = levels[-1].px_e8 if levels else 0
    tail.vwap_price_e8 = vwap_e8(tail.notional_e8, tail.qty_e8)
    out.append(tail)
    return out
