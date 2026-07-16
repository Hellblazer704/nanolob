#!/usr/bin/env python3
"""Capture Binance spot L2 depth diffs + trades + periodic REST snapshots.

Output is one JSONL file with three record types, all wrapped as
{"type": ..., "ts": <local unix ms>, "data": <raw exchange payload>}:

  snapshot  REST /api/v3/depth (lastUpdateId + full bids/asks arrays).
            Captured at start and every --snapshot-interval seconds; the
            replay tool seeds the book from the first one and *validates*
            against the later ones.
  diff      <symbol>@depth@100ms websocket events (U/u sequence ids, b/a
            level updates).
  trade     <symbol>@trade websocket events (needed by the market-making
            simulator for queue-position fill modeling).

Uses the public market-data-only endpoints (data-api.binance.vision /
data-stream.binance.vision) so no API key is needed and regional blocks on
the main domain don't bite.

Usage:
  pip install websockets
  python scripts/download_binance.py --minutes 5 --out data/btcusdt.jsonl
"""

import argparse
import asyncio
import json
import sys
import time
import urllib.request
from pathlib import Path

REST_BASE = "https://data-api.binance.vision"
WS_BASE = "wss://data-stream.binance.vision"

try:
    import websockets
except ImportError:  # pragma: no cover
    print("this script needs the 'websockets' package: pip install websockets",
          file=sys.stderr)
    sys.exit(1)


def now_ms() -> int:
    return int(time.time() * 1000)


def fetch_snapshot(symbol: str, limit: int) -> dict:
    url = f"{REST_BASE}/api/v3/depth?symbol={symbol.upper()}&limit={limit}"
    with urllib.request.urlopen(url, timeout=15) as resp:
        return json.load(resp)


class Writer:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self._f = path.open("w", encoding="utf-8", newline="\n")
        self.counts = {"snapshot": 0, "diff": 0, "trade": 0}

    def write(self, rec_type: str, data: dict) -> None:
        self._f.write(json.dumps({"type": rec_type, "ts": now_ms(), "data": data},
                                 separators=(",", ":")))
        self._f.write("\n")
        self.counts[rec_type] += 1

    def close(self) -> None:
        self._f.close()


async def stream(symbol: str, minutes: float, snapshot_interval: float,
                 depth_limit: int, writer: Writer) -> None:
    sym = symbol.lower()
    url = f"{WS_BASE}/stream?streams={sym}@depth@100ms/{sym}@trade"
    deadline = time.monotonic() + minutes * 60
    next_snapshot = 0.0  # immediately

    async with websockets.connect(url, ping_interval=15, max_size=2**23) as ws:
        # Per Binance book-management docs: open the diff stream *first*,
        # then take the snapshot, so the snapshot's lastUpdateId falls
        # inside the buffered diff sequence.
        while time.monotonic() < deadline:
            if time.monotonic() >= next_snapshot:
                snap = await asyncio.to_thread(fetch_snapshot, symbol, depth_limit)
                writer.write("snapshot", snap)
                print(f"  snapshot lastUpdateId={snap['lastUpdateId']}")
                next_snapshot = time.monotonic() + snapshot_interval

            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=5)
            except asyncio.TimeoutError:
                continue
            msg = json.loads(raw)
            data = msg.get("data", msg)
            event = data.get("e")
            if event == "depthUpdate":
                writer.write("diff", data)
            elif event == "trade":
                writer.write("trade", data)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbol", default="BTCUSDT")
    ap.add_argument("--minutes", type=float, default=5.0)
    ap.add_argument("--snapshot-interval", type=float, default=60.0,
                    help="seconds between validation snapshots")
    ap.add_argument("--depth-limit", type=int, default=1000,
                    help="REST snapshot depth (max 5000)")
    ap.add_argument("--out", default=None,
                    help="output path (default data/<symbol>_<utc stamp>.jsonl)")
    args = ap.parse_args()

    out = Path(args.out) if args.out else Path("data") / (
        f"{args.symbol.lower()}_{time.strftime('%Y%m%d_%H%M%S', time.gmtime())}.jsonl")

    writer = Writer(out)
    print(f"capturing {args.symbol} depth@100ms + trades for {args.minutes:g} min "
          f"-> {out}")
    try:
        asyncio.run(stream(args.symbol, args.minutes, args.snapshot_interval,
                           args.depth_limit, writer))
    except KeyboardInterrupt:
        print("interrupted — keeping partial capture")
    finally:
        writer.close()
    print(f"done: {writer.counts}")


if __name__ == "__main__":
    main()
