"""Replay a recording independently and emit the expected final book.

This implements Binance's snapshot/diff reconciliation from the venue's own
documented rules, not from the C++ adapter. The C++ replay test feeds the SAME
recording through BinanceProtocol and SideBook and must arrive at the same
book, level for level.

Two independent things are being checked at once:
  * the C++ sequencing agrees with an independent implementation of the rules
  * both agree with a real venue's real stream, since the recording and the
    seeding snapshot came from Binance rather than from either implementation

Run:  python3 gen_replay_golden.py
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate_ref as R                                    # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
RECORDINGS = os.path.join(HERE, "..", "recordings")
FIXTURES = os.path.join(HERE, "..", "fixtures")


class ReplayError(Exception):
    pass


def apply_levels(side, entries):
    """qty 0 removes the level; anything else sets it."""
    for price, qty in entries:
        px, q = R.parse_e8(price), R.parse_e8(qty)
        if q == 0:
            side.pop(px, None)
        else:
            side[px] = q


def replay(path):
    entries = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                entries.append(json.loads(line))
    # Delivered in receive order. The snapshot's position in the FILE is a
    # convention; recv_ts_ns is what says when it actually arrived.
    entries.sort(key=lambda e: e["recv_ts_ns"])

    bids, asks = {}, {}
    buffered = []
    synced = False
    last_update_id = 0
    applied = 0
    dropped = 0

    for entry in entries:
        if entry.get("kind") == "rest_snapshot":
            snap = json.loads(entry["frame"])
            snapshot_id = int(snap["lastUpdateId"])
            bids, asks = {}, {}
            apply_levels(bids, snap["bids"])
            apply_levels(asks, snap["asks"])

            # Everything wholly older than the snapshot is already in it.
            before = len(buffered)
            buffered = [e for e in buffered if e["u"] > snapshot_id]
            dropped = before - len(buffered)

            # The first surviving event must straddle the snapshot.
            if buffered:
                first = buffered[0]
                if not (first["U"] <= snapshot_id + 1 <= first["u"]):
                    raise ReplayError(
                        f"gap between snapshot {snapshot_id} and stream "
                        f"[{first['U']},{first['u']}]")

            through = snapshot_id
            for i, event in enumerate(buffered):
                if i > 0 and event["U"] != through + 1:
                    raise ReplayError(
                        f"discontinuity replaying buffer: expected U={through + 1}, "
                        f"got {event['U']}")
                apply_levels(bids, event["b"])
                apply_levels(asks, event["a"])
                through = event["u"]
                applied += 1
            buffered = []
            last_update_id = through
            synced = True
            continue

        event = json.loads(entry["frame"])
        if event.get("e") != "depthUpdate":
            continue
        if not synced:
            buffered.append(event)
            continue
        if event["U"] != last_update_id + 1:
            raise ReplayError(
                f"discontinuity live: expected U={last_update_id + 1}, got {event['U']}")
        apply_levels(bids, event["b"])
        apply_levels(asks, event["a"])
        last_update_id = event["u"]
        applied += 1

    if not synced:
        raise ReplayError("recording contains no snapshot")
    return bids, asks, {"applied": applied, "dropped_as_stale": dropped,
                        "final_update_id": last_update_id}


def replay_okx(entries):
    """OKX `books`: a snapshot then updates, chained by prevSeqId -> seqId.

    Implemented from OKX's documented rules. Note the ordering that matters:
    continuity is validated BEFORE the no-change shortcut. OKX repeats the
    sequence number when nothing moved, and checking that first would let a gap
    followed by a no-change frame skip validation entirely.
    """
    bids, asks = {}, {}
    last_seq = None
    applied = 0
    for entry in entries:
        frame = entry["frame"]
        if frame == "pong":
            continue
        msg = json.loads(frame)
        if "event" in msg:                       # subscribe acknowledgement
            continue
        action = msg.get("action")
        if action not in ("snapshot", "update"):
            continue
        for data in msg.get("data", []):
            seq = int(data["seqId"])
            if action == "snapshot":
                bids, asks = {}, {}
                apply_levels(bids, [l[:2] for l in data["bids"]])
                apply_levels(asks, [l[:2] for l in data["asks"]])
                last_seq = seq
                applied += 1
                continue
            prev = int(data["prevSeqId"])
            if last_seq is None:
                raise ReplayError("update before any snapshot")
            if prev != last_seq:
                raise ReplayError(f"okx gap: prevSeqId={prev}, expected {last_seq}")
            if prev == seq:                      # nothing changed
                continue
            apply_levels(bids, [l[:2] for l in data["bids"]])
            apply_levels(asks, [l[:2] for l in data["asks"]])
            last_seq = seq
            applied += 1
    if last_seq is None:
        raise ReplayError("recording contains no snapshot")
    return bids, asks, {"applied": applied, "dropped_as_stale": 0,
                        "final_update_id": last_seq}


def replay_bybit(entries):
    """Bybit v5 orderbook: snapshot or delta, chained by a monotonic `u`.

    A snapshot can arrive mid-stream after Bybit's own internal reconnect; it
    replaces the book wholesale and restarts sequencing rather than amending.
    """
    bids, asks = {}, {}
    last_u = None
    applied = 0
    for entry in entries:
        msg = json.loads(entry["frame"])
        if "topic" not in msg:                   # subscribe / pong acknowledgement
            continue
        kind = msg.get("type")
        if kind not in ("snapshot", "delta"):
            continue
        data = msg["data"]
        u = int(data["u"])
        if kind == "snapshot":
            bids, asks = {}, {}
            apply_levels(bids, data["b"])
            apply_levels(asks, data["a"])
            last_u = u
            applied += 1
            continue
        if last_u is None:
            raise ReplayError("delta before any snapshot")
        if u != last_u + 1:
            raise ReplayError(f"bybit gap: u={u}, expected {last_u + 1}")
        apply_levels(bids, data["b"])
        apply_levels(asks, data["a"])
        last_u = u
        applied += 1
    if last_u is None:
        raise ReplayError("recording contains no snapshot")
    return bids, asks, {"applied": applied, "dropped_as_stale": 0,
                        "final_update_id": last_u}


def load_entries(path):
    entries = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                entries.append(json.loads(line))
    entries.sort(key=lambda e: e["recv_ts_ns"])
    return entries


REPLAYS = {
    "binance_btcusdt": None,          # handled by replay(), which needs the file
    "okx_btcusdt": replay_okx,
    "bybit_btcusdt": replay_bybit,
}


def emit(name, bids, asks, stats):
    bid_levels = sorted(((px, qty) for px, qty in bids.items() if qty > 0), reverse=True)
    ask_levels = sorted((px, qty) for px, qty in asks.items() if qty > 0)
    golden = {
        "name": "replay_" + name,
        "description": (
            "Expected final book after replaying a real recorded session, "
            "produced by an independent implementation of the venue's own "
            "documented sequencing rules."
        ),
        "recording": "recordings/" + name + ".jsonl",
        "stats": stats,
        "bids": [{"px_e8": px, "qty_e8": qty} for px, qty in bid_levels],
        "asks": [{"px_e8": px, "qty_e8": qty} for px, qty in ask_levels],
    }
    out = os.path.join(FIXTURES, "replay_" + name + "_golden.json")
    with open(out, "w") as f:
        json.dump(golden, f, separators=(",", ":"))
        f.write("\n")
    print(f"{name}: {stats['applied']} events applied, "
          f"{len(bid_levels)} bids / {len(ask_levels)} asks, "
          f"final id {stats['final_update_id']}")
    if bid_levels and ask_levels:
        assert bid_levels[0][0] < ask_levels[0][0], \
            f"{name}: a single venue's book must never cross"
        print(f"  touch {R.format_e8(bid_levels[0][0])} / "
              f"{R.format_e8(ask_levels[0][0])}  (uncrossed)")


def main():
    for name, fn in REPLAYS.items():
        path = os.path.join(RECORDINGS, name + ".jsonl")
        if not os.path.exists(path):
            print(f"{name}: no recording, skipping")
            continue
        if fn is None:
            bids, asks, stats = replay(path)
        else:
            bids, asks, stats = fn(load_entries(path))
        emit(name, bids, asks, stats)


if __name__ == "__main__":
    main()
