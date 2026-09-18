"""Capture a real venue recording, independently of the aggregator.

Deliberately does not use the aggregator's own recorder. If the same code both
captured and replayed, a systematic capture bug would cancel itself out and the
replay test would pass while the adapter was wrong. Capturing with an
independent client makes the venue itself the oracle.

Uses a minimal RFC 6455 client over ssl+socket, because no websocket library is
available here and the client only needs to do one thing: read text frames and
answer pings.

Output is the format the aggregator's --record-dir emits, so the same replay
test consumes either:
    {"recv_ts_ns": <int64>, "frame": "<raw frame text>"}   one per line
    {"kind": "rest_snapshot", "recv_ts_ns": ..., "frame": ...}   final line

Run:  python3 capture.py [seconds]
"""

import base64
import json
import os
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.request

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "recordings")

# Binance seeds from REST and amends by diff stream. OKX and Bybit both
# snapshot over the websocket, so they need a subscribe frame and no REST call
# -- which also means their replay has no reconciliation to model.
VENUES = {
    "binance_btcusdt": {
        "host": "stream.binance.com", "port": 9443,
        "target": "/ws/btcusdt@depth@100ms",
        "subscribe": [],
        "rest": "https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=5000",
        "ping": None, "ping_every": 0,
    },
    "okx_btcusdt": {
        "host": "ws.okx.com", "port": 8443,
        "target": "/ws/v5/public",
        "subscribe": ['{"op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT"}]}'],
        "rest": None,
        # A TEXT frame containing "ping", not a websocket control ping.
        "ping": "ping", "ping_every": 20,
    },
    "bybit_btcusdt": {
        "host": "stream.bybit.com", "port": 443,
        "target": "/v5/public/spot",
        "subscribe": ['{"op":"subscribe","args":["orderbook.200.BTCUSDT"]}'],
        "rest": None,
        "ping": '{"op":"ping"}', "ping_every": 15,
    },
}


def now_ns():
    return time.time_ns()


class MinimalWs:
    """Just enough RFC 6455 to read text frames and answer a ping."""

    def __init__(self, host, port, target):
        raw = socket.create_connection((host, port), timeout=30)
        context = ssl.create_default_context()
        self.sock = context.wrap_socket(raw, server_hostname=host)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {target} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        header = b""
        while b"\r\n\r\n" not in header:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed during handshake")
            header += chunk
        status = header.split(b"\r\n", 1)[0].decode(errors="replace")
        if "101" not in status:
            raise RuntimeError(f"handshake rejected: {status}")
        self.buf = header.split(b"\r\n\r\n", 1)[1]

    def _recv_exactly(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _send_frame(self, opcode, payload):
        # Client frames MUST be masked.
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        header = struct.pack("!BB", 0x80 | opcode, 0x80 | len(payload))
        self.sock.sendall(header + mask + masked)

    def read_text(self):
        """Returns the next text frame, answering control frames transparently."""
        while True:
            b0, b1 = struct.unpack("!BB", self._recv_exactly(2))
            opcode = b0 & 0x0F
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._recv_exactly(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._recv_exactly(8))[0]
            if b1 & 0x80:                       # server frames are never masked
                self._recv_exactly(4)
            payload = self._recv_exactly(length)
            if opcode == 0x1:
                return payload.decode()
            if opcode == 0x9:                   # ping -> pong, or Binance drops us
                self._send_frame(0xA, payload)
            elif opcode == 0x8:
                raise RuntimeError("server closed")

    def send_text(self, payload):
        self._send_frame(0x1, payload.encode())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "binance_btcusdt"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
    if name not in VENUES:
        sys.exit(f"unknown venue {name!r}; known: {', '.join(VENUES)}")
    venue = VENUES[name]

    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".jsonl")

    ws = MinimalWs(venue["host"], venue["port"], venue["target"])
    for frame in venue["subscribe"]:
        ws.send_text(frame)

    lines = []
    deadline = time.time() + seconds
    captured = {}
    next_ping = time.time() + venue["ping_every"] if venue["ping"] else None

    def fetch_snapshot():
        # On its own thread so the read loop keeps timestamping frames while the
        # round trip is in flight. Fetching inline stalls the reader, so frames
        # that genuinely arrived DURING the request get stamped after it, and
        # the replay then sees events older than the snapshot arriving after it
        # -- which looks exactly like a sequence break. The aggregator gets this
        # right by issuing the GET asynchronously on the same io_context; a
        # recording has to reproduce the same ordering to be faithful.
        request = urllib.request.Request(
            venue["rest"], headers={"User-Agent": "hermeneutic-capture"})
        with urllib.request.urlopen(request, timeout=25) as response:
            body = response.read().decode()
        captured["snapshot"] = {"kind": "rest_snapshot", "recv_ts_ns": now_ns(),
                                "frame": body}

    fetcher = None
    snapshot_at = time.time() + min(3.0, seconds / 4)
    # Buffering starts BEFORE the snapshot request, which is the whole point of
    # the Binance reconciliation and the thing a recording has to preserve.
    while time.time() < deadline:
        frame = ws.read_text()
        lines.append({"recv_ts_ns": now_ns(), "frame": frame})
        if venue["rest"] and fetcher is None and time.time() >= snapshot_at:
            fetcher = threading.Thread(target=fetch_snapshot, daemon=True)
            fetcher.start()
        if next_ping is not None and time.time() >= next_ping:
            ws.send_text(venue["ping"])
            next_ping = time.time() + venue["ping_every"]
    ws.close()

    snapshot = None
    if venue["rest"]:
        if fetcher is not None:
            fetcher.join(timeout=30)
        snapshot = captured.get("snapshot")
        if snapshot is None:
            sys.exit("never captured a snapshot; increase the duration")

    with open(path, "w") as f:
        for line in lines:
            f.write(json.dumps(line) + "\n")
        if snapshot is not None:
            f.write(json.dumps(snapshot) + "\n")

    span_ms = (lines[-1]["recv_ts_ns"] - lines[0]["recv_ts_ns"]) // 1_000_000
    print(f"wrote {path}")
    print(f"  {len(lines)} frames over {span_ms} ms "
          f"({os.path.getsize(path) // 1024} KiB)")
    if snapshot is not None:
        buffered = sum(1 for l in lines if l["recv_ts_ns"] < snapshot["recv_ts_ns"])
        print(f"  {buffered} frames captured BEFORE the snapshot request, "
              f"{len(lines) - buffered} after")


if __name__ == "__main__":
    main()
