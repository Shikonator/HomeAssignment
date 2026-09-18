"""Hand-built venue frame sequences, written from each venue's documented rules.

Replay proves the adapters agree with a HEALTHY live session. It cannot prove
they react correctly to the unhealthy ones, because a healthy recording
contains no gaps, no mid-stream snapshots and no rejections. Those paths are
the ones that matter most -- they are what stands between a lost message and a
permanently wrong book -- and they have to be constructed.

Each scenario is a sequence of frames with the verdict the venue's rules
require. Data-driven rather than written as C++ so the sequence is legible
without reading the harness.

Run:  python3 gen_venue_scenarios.py
"""

import json
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures")
TS = 1758138000000


def okx(action, seq, prev_seq, bids=None, asks=None, inst="BTC-USDT"):
    return json.dumps({
        "arg": {"channel": "books", "instId": inst},
        "action": action,
        "data": [{
            "asks": [[p, q, "0", "1"] for p, q in (asks or [])],
            "bids": [[p, q, "0", "1"] for p, q in (bids or [])],
            "ts": str(TS), "seqId": seq, "prevSeqId": prev_seq,
        }],
    })


def okx_two_entries(first, second):
    """One frame carrying two data entries, so partial application is possible."""
    return json.dumps({
        "arg": {"channel": "books", "instId": "BTC-USDT"},
        "action": "update",
        "data": [first, second],
    })


def okx_entry(seq, prev_seq, bids=None, asks=None):
    return {
        "asks": [[p, q, "0", "1"] for p, q in (asks or [])],
        "bids": [[p, q, "0", "1"] for p, q in (bids or [])],
        "ts": str(TS), "seqId": seq, "prevSeqId": prev_seq,
    }


def bybit(kind, u, bids=None, asks=None, topic="orderbook.200.BTCUSDT"):
    return json.dumps({
        "topic": topic, "type": kind, "ts": TS,
        "data": {"s": "BTCUSDT", "b": [[p, q] for p, q in (bids or [])],
                 "a": [[p, q] for p, q in (asks or [])], "u": u, "seq": u},
    })


def binance(first_id, final_id, bids=None, asks=None):
    return json.dumps({
        "e": "depthUpdate", "E": TS, "s": "BTCUSDT",
        "U": first_id, "u": final_id,
        "b": [[p, q] for p, q in (bids or [])],
        "a": [[p, q] for p, q in (asks or [])],
    })


def binance_rest(last_update_id, bids, asks):
    return json.dumps({
        "lastUpdateId": last_update_id,
        "bids": [[p, q] for p, q in bids],
        "asks": [[p, q] for p, q in asks],
    })


OK, RESYNC, PARSE_ERROR, FATAL = "ok", "needs_resync", "parse_error", "fatal"

SCENARIOS = [
    # ---- OKX -------------------------------------------------------------
    {
        "name": "okx_snapshot_then_update",
        "venue": "okx",
        "description": "Baseline: a snapshot seeds the book, a chained update amends it.",
        "steps": [
            {"frame": okx("snapshot", 100, -1, [("76500.0", "2.0")], [("76500.1", "1.5")]),
             "expect": OK, "updates": 1},
            {"frame": okx("update", 101, 100, [("76499.0", "3.0")]), "expect": OK, "updates": 1},
        ],
        "final_bids": [["76500.0", "2.0"], ["76499.0", "3.0"]],
        "final_asks": [["76500.1", "1.5"]],
    },
    {
        "name": "okx_gap_then_no_change_heartbeat_must_resync",
        "venue": "okx",
        "description": (
            "THE ordering bug. OKX repeats the sequence number when nothing "
            "moved. If that no-change shortcut is checked BEFORE continuity, a "
            "gap followed by a heartbeat skips validation entirely and the book "
            "diverges permanently with no error. Continuity must be checked "
            "first. This is the regression guard for that ordering, and it "
            "matters more than it looks: the OKX checksum was the only other "
            "detector for silent divergence and it is deliberately not "
            "implemented."
        ),
        "steps": [
            {"frame": okx("snapshot", 100, -1, [("76500.0", "2.0")], [("76500.1", "1.5")]),
             "expect": OK, "updates": 1},
            # Everything from 101..150 was lost. The heartbeat says 150/150.
            {"frame": okx("update", 150, 150), "expect": RESYNC, "updates": 0},
        ],
    },
    {
        "name": "okx_in_sequence_no_change_heartbeat_is_a_no_op",
        "venue": "okx",
        "description": (
            "The legitimate case the shortcut exists for must still work: a "
            "heartbeat whose prevSeqId matches our position is accepted and "
            "emits nothing, and the stream continues from the same sequence."
        ),
        "steps": [
            {"frame": okx("snapshot", 100, -1, [("76500.0", "2.0")], [("76500.1", "1.5")]),
             "expect": OK, "updates": 1},
            {"frame": okx("update", 100, 100), "expect": OK, "updates": 0},
            {"frame": okx("update", 101, 100, [("76498.0", "1.0")]), "expect": OK, "updates": 1},
        ],
        "final_bids": [["76500.0", "2.0"], ["76498.0", "1.0"]],
        "final_asks": [["76500.1", "1.5"]],
    },
    {
        "name": "okx_sequence_gap_forces_resync",
        "venue": "okx",
        "description": "An update whose prevSeqId does not match our position is a gap.",
        "steps": [
            {"frame": okx("snapshot", 100, -1, [("76500.0", "2.0")], [("76500.1", "1.5")]),
             "expect": OK, "updates": 1},
            {"frame": okx("update", 106, 105, [("76499.0", "1.0")]), "expect": RESYNC,
             "updates": 0},
        ],
    },
    {
        "name": "okx_failed_frame_appends_nothing",
        "venue": "okx",
        "description": (
            "A frame carrying two entries where the first is valid and the "
            "second breaks sequence must append NOTHING. Applying the first "
            "and then reporting failure would leave the caller holding half a "
            "batch with a verdict telling it to discard state."
        ),
        "steps": [
            {"frame": okx("snapshot", 100, -1, [("76500.0", "2.0")], [("76500.1", "1.5")]),
             "expect": OK, "updates": 1},
            {"frame": okx_two_entries(okx_entry(101, 100, [("76499.0", "1.0")]),
                                      okx_entry(999, 998, [("76498.0", "1.0")])),
             "expect": RESYNC, "updates": 0},
        ],
    },
    {
        "name": "okx_subscribe_rejection_is_fatal",
        "venue": "okx",
        "description": (
            "An unknown instrument fails identically on every reconnect. "
            "Treating it as transient loops forever while presenting as a "
            "network problem."
        ),
        "steps": [
            {"frame": json.dumps({"event": "error", "code": "60018",
                                  "msg": "Wrong URL or channel:books,instId:NOPE-USDT"}),
             "expect": FATAL, "updates": 0},
        ],
    },
    {
        "name": "okx_frame_for_another_instrument_is_ignored",
        "venue": "okx",
        "description": "Moot with one connection per subscription; it is the bug "
                       "that appears the day someone multiplexes.",
        "steps": [
            {"frame": okx("snapshot", 100, -1, [("76500.0", "2.0")], [("76500.1", "1.5")]),
             "expect": OK, "updates": 1},
            {"frame": okx("snapshot", 500, -1, [("1.0", "1.0")], [("2.0", "1.0")],
                          inst="ETH-USDT"), "expect": OK, "updates": 0},
        ],
        "final_bids": [["76500.0", "2.0"]],
        "final_asks": [["76500.1", "1.5"]],
    },

    # ---- Bybit -----------------------------------------------------------
    {
        "name": "bybit_snapshot_then_delta",
        "venue": "bybit",
        "description": "Baseline: snapshot then a delta whose u follows by one.",
        "steps": [
            {"frame": bybit("snapshot", 1, [("76500.00", "2.0")], [("76500.10", "1.5")]),
             "expect": OK, "updates": 1},
            {"frame": bybit("delta", 2, [("76499.00", "1.0")]), "expect": OK, "updates": 1},
        ],
        "final_bids": [["76500.00", "2.0"], ["76499.00", "1.0"]],
        "final_asks": [["76500.10", "1.5"]],
    },
    {
        "name": "bybit_gap_forces_resync",
        "venue": "bybit",
        "description": "A non-consecutive u means at least one delta was lost.",
        "steps": [
            {"frame": bybit("snapshot", 1, [("76500.00", "2.0")], [("76500.10", "1.5")]),
             "expect": OK, "updates": 1},
            {"frame": bybit("delta", 5, [("76499.00", "1.0")]), "expect": RESYNC, "updates": 0},
        ],
    },
    {
        "name": "bybit_midstream_snapshot_replaces_book",
        "venue": "bybit",
        "description": (
            "Bybit sends a fresh snapshot after its own internal reconnect, "
            "mid-stream and unannounced. It replaces the book wholesale and "
            "restarts sequencing; trying to amend with it would merge two "
            "different books."
        ),
        "steps": [
            {"frame": bybit("snapshot", 1, [("76500.00", "2.0")], [("76500.10", "1.5")]),
             "expect": OK, "updates": 1},
            {"frame": bybit("delta", 2, [("76499.00", "1.0")]), "expect": OK, "updates": 1},
            {"frame": bybit("snapshot", 90, [("76400.00", "5.0")], [("76400.10", "4.0")]),
             "expect": OK, "updates": 1},
            {"frame": bybit("delta", 91, [("76399.00", "1.0")]), "expect": OK, "updates": 1},
        ],
        "final_bids": [["76400.00", "5.0"], ["76399.00", "1.0"]],
        "final_asks": [["76400.10", "4.0"]],
    },
    {
        "name": "bybit_delete_level_with_zero_quantity",
        "venue": "bybit",
        "description": "Quantity zero removes the level rather than resting at zero.",
        "steps": [
            {"frame": bybit("snapshot", 1, [("76500.00", "2.0"), ("76499.00", "3.0")],
                            [("76500.10", "1.5")]), "expect": OK, "updates": 1},
            {"frame": bybit("delta", 2, [("76499.00", "0")]), "expect": OK, "updates": 1},
        ],
        "final_bids": [["76500.00", "2.0"]],
        "final_asks": [["76500.10", "1.5"]],
    },
    {
        "name": "bybit_subscribe_rejection_is_fatal",
        "venue": "bybit",
        "description": "A rejected subscription is not transient.",
        "steps": [
            {"frame": json.dumps({"success": False, "ret_msg": "Invalid symbol",
                                  "op": "subscribe", "conn_id": "abc"}),
             "expect": FATAL, "updates": 0},
        ],
    },

    # ---- Binance ---------------------------------------------------------
    {
        "name": "binance_buffers_before_snapshot_then_reconciles",
        "venue": "binance",
        "description": (
            "The reconciliation everyone gets wrong. Events arriving before the "
            "REST snapshot are BUFFERED, not applied and not dropped. On the "
            "snapshot, events wholly older than lastUpdateId are discarded, the "
            "first survivor must straddle it (U <= id+1 <= u), and the rest "
            "replay in order."
        ),
        "steps": [
            {"frame": binance(95, 99, [("76490.00", "1.0")]), "expect": OK, "updates": 0},
            {"frame": binance(100, 104, [("76499.00", "2.0")]), "expect": OK, "updates": 0},
            {"frame": binance(105, 108, [("76498.00", "3.0")]), "expect": OK, "updates": 0},
            {"kind": "rest_snapshot",
             "frame": binance_rest(103, [("76500.00", "5.0")], [("76500.10", "4.0")]),
             "expect": OK, "updates": 3},
        ],
        # Snapshot at 103: the 95-99 event is discarded, 100-104 straddles it
        # and applies, 105-108 follows on.
        "final_bids": [["76500.00", "5.0"], ["76499.00", "2.0"], ["76498.00", "3.0"]],
        "final_asks": [["76500.10", "4.0"]],
    },
    {
        "name": "binance_hole_between_snapshot_and_stream",
        "venue": "binance",
        "description": (
            "If the first surviving event starts after lastUpdateId+1 there is "
            "a hole the stream cannot fill, and the only correct response is a "
            "newer snapshot rather than applying across the gap."
        ),
        "steps": [
            {"frame": binance(200, 204, [("76499.00", "2.0")]), "expect": OK, "updates": 0},
            {"kind": "rest_snapshot",
             "frame": binance_rest(100, [("76500.00", "5.0")], [("76500.10", "4.0")]),
             "expect": RESYNC},
        ],
    },
    {
        "name": "binance_gap_after_sync_forces_resync",
        "venue": "binance",
        "description": "Live events must satisfy U == previous u + 1.",
        "steps": [
            {"frame": binance(101, 104, [("76499.00", "2.0")]), "expect": OK, "updates": 0},
            {"kind": "rest_snapshot",
             "frame": binance_rest(100, [("76500.00", "5.0")], [("76500.10", "4.0")]),
             "expect": OK, "updates": 2},
            {"frame": binance(106, 108, [("76498.00", "1.0")]), "expect": RESYNC, "updates": 0},
        ],
    },
    {
        "name": "binance_frame_for_another_symbol_is_ignored",
        "venue": "binance",
        "description": "Same multiplexing guard as the other two venues.",
        "steps": [
            {"frame": json.dumps({"e": "depthUpdate", "E": TS, "s": "ETHUSDT",
                                  "U": 1, "u": 2, "b": [["1.0", "1.0"]], "a": []}),
             "expect": OK, "updates": 0},
        ],
    },
]


if __name__ == "__main__":
    doc = {
        "name": "venue_scenarios",
        "description": (
            "Hand-built frame sequences exercising the unhealthy paths a live "
            "recording cannot contain: sequence gaps, no-change heartbeats, "
            "mid-stream snapshots, rejected subscriptions and cross-instrument "
            "frames. Written from each venue's documented rules."
        ),
        "scenarios": SCENARIOS,
    }
    path = os.path.join(OUT, "venue_scenarios.json")
    with open(path, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    steps = sum(len(s["steps"]) for s in SCENARIOS)
    print(f"{len(SCENARIOS)} scenarios, {steps} steps -> {os.path.normpath(path)}")
